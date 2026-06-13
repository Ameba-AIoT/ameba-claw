/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_driver_spi.h"

#include <string.h>

#include "ameba_soc.h"
#include "ameba_gdma.h"
#include "lauxlib.h"
#include "luhw.h"
#include "os_wrapper.h"

#define LUA_DRIVER_SPI_METATABLE  "spi.port"
#define SPI_BUS_CLK_HZ            100000000UL
#define SPI_XFER_BUF_SIZE         256
#define SPI_DEFAULT_TIMEOUT_MS    1000

/* Per-index state shared by DMA and interrupt callbacks */
typedef struct {
    /* interrupt RX */
    u8           *it_rx_buf;
    u32           it_rx_total;
    volatile u32  it_rx_done;
    /* DMA — channels pre-allocated at spi.new() time so WiFi never steals them */
    GDMA_InitTypeDef dma_tx;
    GDMA_InitTypeDef dma_rx;
    u32           dma_rx_len;
    u8            tx_ch;          /* pre-allocated GDMA channel for TX */
    u8            rx_ch;          /* pre-allocated GDMA channel for RX */
    int           dma_allocated;  /* 1 after tx_ch/rx_ch have been allocated */
    /* completion semaphores */
    rtos_sema_t   sema_tx;
    rtos_sema_t   sema_rx;
    int           sema_created;
} spi_ctx_t;

static spi_ctx_t s_ctx[2];
static int       s_in_use[2];

/* Non-cacheable buffers required by DMA and interrupt RX.
 * DMA buffers must be CACHE_LINE_SIZE-aligned (32 B) so a DCache_CleanInvalidate
 * never spills into an adjacent allocation. */
SRAM_NOCACHE_DATA_SECTION static u8 s_dma_tx_buf[2][SPI_XFER_BUF_SIZE] __attribute__((aligned(CACHE_LINE_SIZE)));
SRAM_NOCACHE_DATA_SECTION static u8 s_dma_rx_buf[2][SPI_XFER_BUF_SIZE] __attribute__((aligned(CACHE_LINE_SIZE)));
SRAM_NOCACHE_DATA_SECTION static u8 s_it_rx_buf[2][SPI_XFER_BUF_SIZE];

typedef struct {
    SPI_TypeDef *dev;
    u32          index;
    int          closed;
} lua_driver_spi_ud_t;

static const u32 s_spi_mosi_mux[] = { PINMUX_FUNCTION_SPI0_MOSI, PINMUX_FUNCTION_SPI1_MOSI };
static const u32 s_spi_miso_mux[] = { PINMUX_FUNCTION_SPI0_MISO, PINMUX_FUNCTION_SPI1_MISO };
static const u32 s_spi_clk_mux[]  = { PINMUX_FUNCTION_SPI0_CLK,  PINMUX_FUNCTION_SPI1_CLK  };
static const u32 s_spi_cs_mux[]   = { PINMUX_FUNCTION_SPI0_CS,   PINMUX_FUNCTION_SPI1_CS   };
static const u32 s_spi_periph[]   = { APBPeriph_SPI0,       APBPeriph_SPI1       };
static const u32 s_spi_clk_en[]   = { APBPeriph_SPI0_CLOCK, APBPeriph_SPI1_CLOCK };

/* ---- IRQ handler: drains RX FIFO into interrupt RX buffer ---- */
static u32 spi_irq_handler(void *param)
{
    u32 idx = (u32)(uintptr_t)param;
    spi_ctx_t *ctx = &s_ctx[idx];
    SPI_TypeDef *dev = SPI_DEV_TABLE[idx].SPIx;

    u32 isr = SSI_GetIsr(dev);
    SSI_SetIsrClean(dev, isr);

    if (isr & SPI_BIT_RXFIS) {
        u32 space = ctx->it_rx_total - ctx->it_rx_done;
        if (space > 0) {
            u32 recvd = SSI_ReceiveData(dev, ctx->it_rx_buf + ctx->it_rx_done, space);
            ctx->it_rx_done += recvd;
        }
        if (ctx->it_rx_done >= ctx->it_rx_total) {
            SSI_INTConfig(dev, SPI_BIT_RXFIM | SPI_BIT_RXOIM | SPI_BIT_RXUIM, DISABLE);
            InterruptDis(SPI_DEV_TABLE[idx].IrqNum);
            rtos_sema_give(ctx->sema_rx);
        }
    }
    return 0;
}

/* ---- DMA TX complete callback ---- */
static u32 spi_dma_tx_cb(void *param)
{
    u32 idx = (u32)(uintptr_t)param;
    spi_ctx_t *ctx = &s_ctx[idx];

    GDMA_ClearINT(ctx->dma_tx.GDMA_Index, ctx->dma_tx.GDMA_ChNum);
    GDMA_Cmd(ctx->dma_tx.GDMA_Index, ctx->dma_tx.GDMA_ChNum, DISABLE);
    SSI_SetDmaEnable(SPI_DEV_TABLE[idx].SPIx, DISABLE, SPI_BIT_TDMAE);
    /* Channel stays allocated — spi.close() releases it */
    rtos_sema_give(ctx->sema_tx);
    return 0;
}

/* ---- DMA RX complete callback ---- */
static u32 spi_dma_rx_cb(void *param)
{
    u32 idx = (u32)(uintptr_t)param;
    spi_ctx_t *ctx = &s_ctx[idx];

    GDMA_ClearINT(ctx->dma_rx.GDMA_Index, ctx->dma_rx.GDMA_ChNum);
    GDMA_Cmd(ctx->dma_rx.GDMA_Index, ctx->dma_rx.GDMA_ChNum, DISABLE);
    SSI_SetDmaEnable(SPI_DEV_TABLE[idx].SPIx, DISABLE, SPI_BIT_RDMAE);
    /* Channel stays allocated — spi.close() releases it */
    rtos_sema_give(ctx->sema_rx);
    return 0;
}

/* ---- helpers ---- */
static lua_driver_spi_ud_t *spi_get_ud(lua_State *L, int stack_idx)
{
    lua_driver_spi_ud_t *ud = (lua_driver_spi_ud_t *)luaL_checkudata(
        L, stack_idx, LUA_DRIVER_SPI_METATABLE);
    if (!ud || ud->closed) {
        luaL_error(L, "spi: invalid or closed port");
    }
    return ud;
}

/* Fill buf from a Lua string or integer-array table at stack position arg.
 * Returns 0 on success; calls luaL_error (longjmp) on failure. */
static int spi_extract_bytes(lua_State *L, int arg, u8 *buf, u32 buf_size, u32 *out_len)
{
    int type = lua_type(L, arg);
    if (type == LUA_TSTRING) {
        size_t len;
        const char *s = lua_tolstring(L, arg, &len);
        if ((u32)len > buf_size) {
            return luaL_error(L, "spi: data too large (max %d bytes)", buf_size);
        }
        memcpy(buf, s, len);
        *out_len = (u32)len;
    } else if (type == LUA_TTABLE) {
        lua_Integer n = luaL_len(L, arg);
        if (n < 0 || (u32)n > buf_size) {
            return luaL_error(L, "spi: table too large (max %d bytes)", buf_size);
        }
        for (lua_Integer i = 0; i < n; i++) {
            lua_rawgeti(L, arg, i + 1);
            lua_Integer b = luaL_checkinteger(L, -1);
            if (b < 0 || b > 0xFF) {
                return luaL_error(L, "spi: byte #%d out of range 0-255", (int)(i + 1));
            }
            buf[i] = (u8)b;
            lua_pop(L, 1);
        }
        *out_len = (u32)n;
    } else {
        return luaL_error(L, "spi: expected string or table of bytes");
    }
    return 0;
}

/* ---- spi.new(index, mosi, miso, sclk, cs, role [, opts]) ---- */
static int lua_driver_spi_new(lua_State *L)
{
    lua_Integer index = luaL_checkinteger(L, 1);
    PinName     mosi  = luhw_check_pin(L, 2);
    PinName     miso  = luhw_check_pin(L, 3);
    PinName     sclk  = luhw_check_pin(L, 4);
    PinName     cs    = luhw_check_pin(L, 5);
    lua_Integer role  = luaL_checkinteger(L, 6);  /* 0=slave, 1=master */

    if (index < 0 || index > 1) {
        return luaL_error(L, "spi: index must be 0 or 1");
    }
    if (role != 0 && role != 1) {
        return luaL_error(L, "spi: role must be 0 (slave) or 1 (master)");
    }

    u32 speed = 1000000;
    u32 bits  = 8;
    u32 mode  = 0;

    if (!lua_isnoneornil(L, 7)) {
        luaL_checktype(L, 7, LUA_TTABLE);
        lua_getfield(L, 7, "speed");
        if (!lua_isnil(L, -1)) { speed = (u32)luaL_checkinteger(L, -1); }
        lua_pop(L, 1);
        lua_getfield(L, 7, "bits");
        if (!lua_isnil(L, -1)) { bits = (u32)luaL_checkinteger(L, -1); }
        lua_pop(L, 1);
        lua_getfield(L, 7, "mode");
        if (!lua_isnil(L, -1)) { mode = (u32)luaL_checkinteger(L, -1); }
        lua_pop(L, 1);
    }

    if (bits < 4 || bits > 16) { return luaL_error(L, "spi: bits must be 4-16"); }
    if (mode > 3)               { return luaL_error(L, "spi: mode must be 0-3");  }
    if (speed < 1)              { return luaL_error(L, "spi: speed must be > 0"); }

    u32 idx = (u32)index;

    if (s_in_use[idx]) {
        return luaL_error(L, "spi%d is already in use", idx);
    }

    spi_ctx_t *ctx = &s_ctx[idx];
    if (!ctx->sema_created) {
        rtos_sema_create_binary(&ctx->sema_tx);
        rtos_sema_create_binary(&ctx->sema_rx);
        ctx->sema_created = 1;
    }

    RCC_PeriphClockCmd(s_spi_periph[idx], s_spi_clk_en[idx], ENABLE);

    Pinmux_Config((u8)mosi, s_spi_mosi_mux[idx]);
    Pinmux_Config((u8)miso, s_spi_miso_mux[idx]);
    Pinmux_Config((u8)sclk, s_spi_clk_mux[idx]);
    Pinmux_Config((u8)cs,   s_spi_cs_mux[idx]);
    PAD_PullCtrl((u8)cs, GPIO_PuPd_UP);

    SSI_SetRole(SPI_DEV_TABLE[idx].SPIx, (u32)role);

    SSI_InitTypeDef ssi;
    SSI_StructInit(&ssi);
    ssi.SPI_Role         = (u32)role;
    ssi.SPI_DataFrameSize = bits - 1;
    ssi.SPI_SclkPolarity  = ((mode >> 1) & 1) ? SCPOL_INACTIVE_IS_HIGH : SCPOL_INACTIVE_IS_LOW;
    ssi.SPI_SclkPhase     = (mode & 1) ? SCPH_TOGGLES_AT_START : SCPH_TOGGLES_IN_MIDDLE;

    u32 div = SPI_BUS_CLK_HZ / speed;
    if (div < 2) { div = 2; }
    if (div & 1) { div++; }  /* divider must be even */
    ssi.SPI_ClockDivider = div;

    if ((u32)role == SSI_MASTER) {
        ssi.SPI_SlaveSelectEnable = 0;  /* CS0: SSI_SetSlaveEnable takes an index, not a bitmask */
    } else {
        /* Slave: pull SCLK to match idle polarity so it doesn't float before master drives it */
        u32 scpol = ((mode >> 1) & 1);
        PAD_PullCtrl((u8)sclk, scpol ? GPIO_PuPd_UP : GPIO_PuPd_DOWN);
    }

    SSI_Init(SPI_DEV_TABLE[idx].SPIx, &ssi);
    SSI_Cmd(SPI_DEV_TABLE[idx].SPIx, ENABLE);

    InterruptRegister((IRQ_FUN)spi_irq_handler, SPI_DEV_TABLE[idx].IrqNum, idx, 5);

    /* Pre-allocate GDMA channels now, while no system DMA is active on them.
     * This prevents the "channel had used" race that occurs when WiFi DMA
     * frees a channel without disabling hardware and we later acquire it. */
    if (!ctx->dma_allocated) {
        ctx->tx_ch = GDMA_ChnlAlloc(0, (IRQ_FUN)spi_dma_tx_cb, idx, INT_PRI_MIDDLE);
        ctx->rx_ch = GDMA_ChnlAlloc(0, (IRQ_FUN)spi_dma_rx_cb, idx, INT_PRI_MIDDLE);
        if (ctx->tx_ch != 0xFF && ctx->rx_ch != 0xFF) {
            /* Sanitize: a previous owner may have called GDMA_ChnlFree without
             * GDMA_Cmd(DISABLE), leaving CH_EN=1 in hardware.  GDMA_Abort uses
             * SUSPEND+wait-for-INACTIVE before calling GDMA_Cmd, so it succeeds
             * even when the channel is armed and waiting for a DREQ. */
            GDMA_Abort(0, ctx->tx_ch);
            GDMA_Abort(0, ctx->rx_ch);
            ctx->dma_allocated = 1;
        } else {
            if (ctx->tx_ch != 0xFF) { GDMA_ChnlFree(0, ctx->tx_ch); }
            if (ctx->rx_ch != 0xFF) { GDMA_ChnlFree(0, ctx->rx_ch); }
        }
    }

    s_in_use[idx] = 1;

    lua_driver_spi_ud_t *ud = (lua_driver_spi_ud_t *)lua_newuserdata(L, sizeof(*ud));
    ud->dev    = SPI_DEV_TABLE[idx].SPIx;
    ud->index  = idx;
    ud->closed = 0;
    luaL_getmetatable(L, LUA_DRIVER_SPI_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

/* ---- spi:write(data [, timeout_ms]) ----
 * Push bytes into TX FIFO (non-blocking unless FIFO full).
 * Returns number of bytes written. */
static int lua_driver_spi_write(lua_State *L)
{
    lua_driver_spi_ud_t *ud      = spi_get_ud(L, 1);
    lua_Integer          timeout = luaL_optinteger(L, 3, SPI_DEFAULT_TIMEOUT_MS);

    u8  buf[SPI_XFER_BUF_SIZE];
    u32 len = 0;
    spi_extract_bytes(L, 2, buf, SPI_XFER_BUF_SIZE, &len);

    u32 written = 0;
    u32 wait    = (u32)timeout;
    while (written < len) {
        if (SSI_Writeable(ud->dev)) {
            SSI_WriteData(ud->dev, buf[written++]);
            wait = (u32)timeout;
        } else {
            if (wait == 0) { break; }
            rtos_time_delay_ms(1);
            wait--;
        }
    }

    lua_pushinteger(L, (lua_Integer)written);
    return 1;
}

/* ---- spi:read(n [, timeout_ms]) ----
 * Drain up to n bytes from RX FIFO. Returns a binary string. */
static int lua_driver_spi_read(lua_State *L)
{
    lua_driver_spi_ud_t *ud      = spi_get_ud(L, 1);
    lua_Integer          n       = luaL_checkinteger(L, 2);
    lua_Integer          timeout = luaL_optinteger(L, 3, 0);

    if (n <= 0 || n > SPI_XFER_BUF_SIZE) {
        return luaL_error(L, "spi: read count must be 1-%d", SPI_XFER_BUF_SIZE);
    }

    luaL_Buffer b;
    luaL_buffinit(L, &b);
    lua_Integer recvd   = 0;
    lua_Integer elapsed = 0;

    while (recvd < n) {
        if (SSI_Readable(ud->dev)) {
            luaL_addchar(&b, (char)(u8)SSI_ReadData(ud->dev));
            recvd++;
        } else {
            if (elapsed >= timeout) { break; }
            rtos_time_delay_ms(1);
            elapsed++;
        }
    }

    luaL_pushresult(&b);
    return 1;
}

/* ---- spi:transfer(tx_data [, timeout_ms]) ----
 * Full-duplex polling transfer (master drives clock).
 * Sends tx_data and returns received bytes as a string. */
static int lua_driver_spi_transfer(lua_State *L)
{
    lua_driver_spi_ud_t *ud      = spi_get_ud(L, 1);
    lua_Integer          timeout = luaL_optinteger(L, 3, SPI_DEFAULT_TIMEOUT_MS);

    u8  tx[SPI_XFER_BUF_SIZE];
    u32 tx_len = 0;
    spi_extract_bytes(L, 2, tx, SPI_XFER_BUF_SIZE, &tx_len);

    /* Fill TX FIFO */
    u32 written = 0;
    u32 wait    = (u32)timeout;
    while (written < tx_len) {
        if (SSI_Writeable(ud->dev)) {
            SSI_WriteData(ud->dev, tx[written++]);
        } else {
            if (wait == 0) {
                return luaL_error(L, "spi: transfer TX timeout");
            }
            rtos_time_delay_ms(1);
            wait--;
        }
    }

    /* Wait for hardware to finish clocking out data */
    wait = (u32)timeout;
    while (SSI_Busy(ud->dev)) {
        if (wait == 0) {
            return luaL_error(L, "spi: transfer busy timeout");
        }
        rtos_time_delay_ms(1);
        wait--;
    }

    /* Drain RX FIFO */
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    u32 avail = SSI_GetRxCount(ud->dev);
    for (u32 i = 0; i < avail; i++) {
        luaL_addchar(&b, (char)(u8)SSI_ReadData(ud->dev));
    }
    luaL_pushresult(&b);
    return 1;
}

/* Helper: build GDMA_InitTypeDef for SPI TX (memory → peripheral) using
 * a pre-allocated channel and call GDMA_Init + GDMA_Cmd(ENABLE). */
static void spi_dma_tx_arm(u32 idx, u32 len)
{
    spi_ctx_t    *ctx = &s_ctx[idx];
    SPI_TypeDef  *dev = SPI_DEV_TABLE[idx].SPIx;
    u32 dfs = SSI_GetDataFrameSize(dev);

    GDMA_StructInit(&ctx->dma_tx);
    ctx->dma_tx.GDMA_DIR        = TTFCMemToPeri;
    ctx->dma_tx.GDMA_DstHandshakeInterface = SPI_DEV_TABLE[idx].Tx_HandshakeInterface;
    ctx->dma_tx.GDMA_DstAddr    = (u32)&dev->SPI_DRx;
    ctx->dma_tx.GDMA_Index      = 0;
    ctx->dma_tx.GDMA_ChNum      = ctx->tx_ch;
    ctx->dma_tx.GDMA_IsrType    = (BlockType | TransferType | ErrType);
    ctx->dma_tx.GDMA_SrcInc     = IncType;
    ctx->dma_tx.GDMA_DstInc     = NoChange;

    if (dfs > 8) {
        ctx->dma_tx.GDMA_SrcMsize    = MsizeFour;
        ctx->dma_tx.GDMA_SrcDataWidth = TrWidthTwoBytes;
        ctx->dma_tx.GDMA_DstMsize    = MsizeFour;
        ctx->dma_tx.GDMA_DstDataWidth = TrWidthTwoBytes;
        ctx->dma_tx.GDMA_BlockSize   = len >> 1;
    } else {
        ctx->dma_tx.GDMA_SrcMsize    = MsizeFour;
        ctx->dma_tx.GDMA_SrcDataWidth = TrWidthOneByte;
        ctx->dma_tx.GDMA_DstMsize    = MsizeFour;
        ctx->dma_tx.GDMA_DstDataWidth = TrWidthOneByte;
        ctx->dma_tx.GDMA_BlockSize   = len;
    }
    ctx->dma_tx.GDMA_SrcAddr = (u32)s_dma_tx_buf[idx];

    DCache_CleanInvalidate((u32)s_dma_tx_buf[idx], len);
    GDMA_Init(0, ctx->tx_ch, &ctx->dma_tx);
    GDMA_Cmd(0, ctx->tx_ch, ENABLE);
}

/* Helper: build GDMA_InitTypeDef for SPI RX (peripheral → memory) using
 * a pre-allocated channel and call GDMA_Init + GDMA_Cmd(ENABLE). */
static void spi_dma_rx_arm(u32 idx, u32 n)
{
    spi_ctx_t    *ctx = &s_ctx[idx];
    SPI_TypeDef  *dev = SPI_DEV_TABLE[idx].SPIx;
    u32 dfs = SSI_GetDataFrameSize(dev);

    GDMA_StructInit(&ctx->dma_rx);
    ctx->dma_rx.GDMA_DIR        = TTFCPeriToMem;
    ctx->dma_rx.GDMA_SrcHandshakeInterface = SPI_DEV_TABLE[idx].Rx_HandshakeInterface;
    ctx->dma_rx.GDMA_SrcAddr    = (u32)&dev->SPI_DRx;
    ctx->dma_rx.GDMA_Index      = 0;
    ctx->dma_rx.GDMA_ChNum      = ctx->rx_ch;
    ctx->dma_rx.GDMA_IsrType    = (BlockType | TransferType | ErrType);
    ctx->dma_rx.GDMA_SrcInc     = NoChange;
    ctx->dma_rx.GDMA_DstInc     = IncType;

    if (dfs > 8) {
        ctx->dma_rx.GDMA_SrcMsize    = MsizeFour;
        ctx->dma_rx.GDMA_SrcDataWidth = TrWidthTwoBytes;
        ctx->dma_rx.GDMA_DstMsize    = MsizeFour;
        ctx->dma_rx.GDMA_DstDataWidth = TrWidthFourBytes;
        ctx->dma_rx.GDMA_BlockSize   = n >> 1;
    } else {
        ctx->dma_rx.GDMA_SrcMsize    = MsizeFour;
        ctx->dma_rx.GDMA_SrcDataWidth = TrWidthOneByte;
        if (((n & 0x03) == 0) && (((u32)s_dma_rx_buf[idx] & 0x03) == 0)) {
            ctx->dma_rx.GDMA_DstMsize    = MsizeOne;
            ctx->dma_rx.GDMA_DstDataWidth = TrWidthFourBytes;
        } else {
            ctx->dma_rx.GDMA_DstMsize    = MsizeFour;
            ctx->dma_rx.GDMA_DstDataWidth = TrWidthOneByte;
        }
        ctx->dma_rx.GDMA_BlockSize = n;
    }
    ctx->dma_rx.GDMA_DstAddr = (u32)s_dma_rx_buf[idx];

    DCache_CleanInvalidate((u32)s_dma_rx_buf[idx], n);
    GDMA_Init(0, ctx->rx_ch, &ctx->dma_rx);
    GDMA_Cmd(0, ctx->rx_ch, ENABLE);
}

/* ---- spi:send_dma(data [, timeout_ms]) ----
 * Send data via DMA TX. Blocking until all bytes are clocked out.
 * Returns true on success. */
static int lua_driver_spi_send_dma(lua_State *L)
{
    lua_driver_spi_ud_t *ud      = spi_get_ud(L, 1);
    lua_Integer          timeout = luaL_optinteger(L, 3, SPI_DEFAULT_TIMEOUT_MS);
    spi_ctx_t           *ctx     = &s_ctx[ud->index];

    if (!ctx->dma_allocated) {
        return luaL_error(L, "spi: DMA channels not available");
    }

    u32 len = 0;
    spi_extract_bytes(L, 2, s_dma_tx_buf[ud->index], SPI_XFER_BUF_SIZE, &len);
    if (len == 0) { return luaL_error(L, "spi: send_dma: empty data"); }

    spi_dma_tx_arm(ud->index, len);
    SSI_SetDmaEnable(ud->dev, ENABLE, SPI_BIT_TDMAE);

    if (rtos_sema_take(ctx->sema_tx, (u32)timeout) != RTK_SUCCESS) {
        SSI_SetDmaEnable(ud->dev, DISABLE, SPI_BIT_TDMAE);
        return luaL_error(L, "spi: send_dma timeout");
    }

    /* Wait for SPI bus to finish clocking out the last bytes in TX FIFO */
    u32 wait = (u32)timeout;
    while (SSI_Busy(ud->dev) && wait > 0) {
        rtos_time_delay_ms(1);
        wait--;
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* ---- spi:recv_dma_start(n) ----
 * Set up DMA RX for n bytes. Non-blocking.
 * Call recv_dma_finish() to wait for completion. */
static int lua_driver_spi_recv_dma_start(lua_State *L)
{
    lua_driver_spi_ud_t *ud  = spi_get_ud(L, 1);
    lua_Integer          n   = luaL_checkinteger(L, 2);
    spi_ctx_t           *ctx = &s_ctx[ud->index];

    if (!ctx->dma_allocated) {
        return luaL_error(L, "spi: DMA channels not available");
    }
    if (n <= 0 || n > SPI_XFER_BUF_SIZE) {
        return luaL_error(L, "spi: recv_dma_start count must be 1-%d", SPI_XFER_BUF_SIZE);
    }

    ctx->dma_rx_len = (u32)n;
    spi_dma_rx_arm(ud->index, (u32)n);
    SSI_SetDmaEnable(ud->dev, ENABLE, SPI_BIT_RDMAE);

    return 0;
}

/* ---- spi:recv_dma_finish([timeout_ms]) ----
 * Wait for DMA RX to complete. Returns received bytes as a string. */
static int lua_driver_spi_recv_dma_finish(lua_State *L)
{
    lua_driver_spi_ud_t *ud      = spi_get_ud(L, 1);
    lua_Integer          timeout = luaL_optinteger(L, 2, SPI_DEFAULT_TIMEOUT_MS);
    spi_ctx_t           *ctx     = &s_ctx[ud->index];

    if (rtos_sema_take(ctx->sema_rx, (u32)timeout) != RTK_SUCCESS) {
        SSI_SetDmaEnable(ud->dev, DISABLE, SPI_BIT_RDMAE);
        return luaL_error(L, "spi: recv_dma_finish timeout");
    }

    lua_pushlstring(L, (const char *)s_dma_rx_buf[ud->index], ctx->dma_rx_len);
    return 1;
}

/* ---- spi:recv_it_start(n) ----
 * Enable interrupt-driven RX for n bytes. Non-blocking.
 * Call recv_it_finish() to wait for completion. */
static int lua_driver_spi_recv_it_start(lua_State *L)
{
    lua_driver_spi_ud_t *ud  = spi_get_ud(L, 1);
    lua_Integer          n   = luaL_checkinteger(L, 2);
    spi_ctx_t           *ctx = &s_ctx[ud->index];

    if (n <= 0 || n > SPI_XFER_BUF_SIZE) {
        return luaL_error(L, "spi: recv_it_start count must be 1-%d", SPI_XFER_BUF_SIZE);
    }

    ctx->it_rx_buf   = s_it_rx_buf[ud->index];
    ctx->it_rx_total = (u32)n;
    ctx->it_rx_done  = 0;

    SSI_SetRxFifoLevel(ud->dev, 0);  /* interrupt on >= 1 byte in RX FIFO */
    InterruptEn(SPI_DEV_TABLE[ud->index].IrqNum, 5);
    SSI_INTConfig(ud->dev, SPI_BIT_RXFIM | SPI_BIT_RXOIM | SPI_BIT_RXUIM, ENABLE);

    return 0;
}

/* ---- spi:recv_it_finish([timeout_ms]) ----
 * Wait for interrupt RX to complete. Returns received bytes as a string. */
static int lua_driver_spi_recv_it_finish(lua_State *L)
{
    lua_driver_spi_ud_t *ud      = spi_get_ud(L, 1);
    lua_Integer          timeout = luaL_optinteger(L, 2, SPI_DEFAULT_TIMEOUT_MS);
    spi_ctx_t           *ctx     = &s_ctx[ud->index];

    if (rtos_sema_take(ctx->sema_rx, (u32)timeout) != RTK_SUCCESS) {
        SSI_INTConfig(ud->dev, SPI_BIT_RXFIM | SPI_BIT_RXOIM | SPI_BIT_RXUIM, DISABLE);
        InterruptDis(SPI_DEV_TABLE[ud->index].IrqNum);
        return luaL_error(L, "spi: recv_it_finish timeout");
    }

    lua_pushlstring(L, (const char *)ctx->it_rx_buf, ctx->it_rx_done);
    return 1;
}

/* ---- spi:set_frequency(hz) ---- */
static int lua_driver_spi_set_frequency(lua_State *L)
{
    lua_driver_spi_ud_t *ud = spi_get_ud(L, 1);
    lua_Integer          hz = luaL_checkinteger(L, 2);

    if (hz <= 0) { return luaL_error(L, "spi: frequency must be > 0"); }

    u32 div = SPI_BUS_CLK_HZ / (u32)hz;
    if (div < 2) { div = 2; }
    if (div & 1) { div++; }

    SSI_Cmd(ud->dev, DISABLE);
    SSI_SetBaudDiv(ud->dev, div);
    SSI_Cmd(ud->dev, ENABLE);
    return 0;
}

/* ---- spi:set_format(bits, mode) ---- */
static int lua_driver_spi_set_format(lua_State *L)
{
    lua_driver_spi_ud_t *ud   = spi_get_ud(L, 1);
    lua_Integer          bits = luaL_checkinteger(L, 2);
    lua_Integer          mode = luaL_optinteger(L, 3, 0);

    if (bits < 4 || bits > 16) { return luaL_error(L, "spi: bits must be 4-16"); }
    if (mode < 0 || mode > 3)  { return luaL_error(L, "spi: mode must be 0-3");  }

    SSI_Cmd(ud->dev, DISABLE);
    SSI_SetDataFrameSize(ud->dev, (u32)bits - 1);
    SSI_SetSclkPolarity(ud->dev, ((mode >> 1) & 1) ? SCPOL_INACTIVE_IS_HIGH : SCPOL_INACTIVE_IS_LOW);
    SSI_SetSclkPhase(ud->dev,    (mode & 1) ? SCPH_TOGGLES_AT_START : SCPH_TOGGLES_IN_MIDDLE);
    SSI_Cmd(ud->dev, ENABLE);
    return 0;
}

static void spi_release_dma(u32 idx)
{
    spi_ctx_t   *ctx = &s_ctx[idx];
    SPI_TypeDef *dev = SPI_DEV_TABLE[idx].SPIx;

    if (!ctx->dma_allocated) {
        return;
    }

    /* Disable SPI DMA requests before touching channels */
    SSI_SetDmaEnable(dev, DISABLE, SPI_BIT_TDMAE);
    SSI_SetDmaEnable(dev, DISABLE, SPI_BIT_RDMAE);

    /* TX channel: clear ISR, clean auto-reload, abort (handles CH_EN=1 case),
     * then free.  Use GDMA_Abort (not GDMA_Cmd) per spi_api.c comment:
     * "Disabling GDMA chan may fail by calling GDMA_Cmd() while GDMA chan
     *  is still working." */
    GDMA_ClearINT(0, ctx->tx_ch);
    GDMA_ChCleanAutoReload(0, ctx->tx_ch, CLEAN_RELOAD_SRC_DST);
    GDMA_Abort(0, ctx->tx_ch);
    GDMA_ChnlFree(0, ctx->tx_ch);

    /* RX channel: same sequence */
    GDMA_ClearINT(0, ctx->rx_ch);
    GDMA_ChCleanAutoReload(0, ctx->rx_ch, CLEAN_RELOAD_SRC_DST);
    GDMA_Abort(0, ctx->rx_ch);
    GDMA_ChnlFree(0, ctx->rx_ch);

    ctx->dma_allocated = 0;
}

/* ---- spi:close() ---- */
static int lua_driver_spi_close(lua_State *L)
{
    lua_driver_spi_ud_t *ud = (lua_driver_spi_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_SPI_METATABLE);
    if (!ud->closed) {
        spi_release_dma(ud->index);
        SSI_Cmd(ud->dev, DISABLE);
        InterruptDis(SPI_DEV_TABLE[ud->index].IrqNum);
        RCC_PeriphClockCmd(s_spi_periph[ud->index], s_spi_clk_en[ud->index], DISABLE);
        s_in_use[ud->index] = 0;
        ud->closed = 1;
    }
    return 0;
}

/* ---- __gc ---- */
static int lua_driver_spi_gc(lua_State *L)
{
    lua_driver_spi_ud_t *ud = (lua_driver_spi_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_SPI_METATABLE);
    if (ud && !ud->closed) {
        spi_release_dma(ud->index);
        SSI_Cmd(ud->dev, DISABLE);
        InterruptDis(SPI_DEV_TABLE[ud->index].IrqNum);
        RCC_PeriphClockCmd(s_spi_periph[ud->index], s_spi_clk_en[ud->index], DISABLE);
        s_in_use[ud->index] = 0;
        ud->closed = 1;
    }
    return 0;
}

int luaopen_spi(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_DRIVER_SPI_METATABLE)) {
        lua_pushcfunction(L, lua_driver_spi_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");

        lua_pushcfunction(L, lua_driver_spi_write);
        lua_setfield(L, -2, "write");
        lua_pushcfunction(L, lua_driver_spi_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_driver_spi_transfer);
        lua_setfield(L, -2, "transfer");
        lua_pushcfunction(L, lua_driver_spi_send_dma);
        lua_setfield(L, -2, "send_dma");
        lua_pushcfunction(L, lua_driver_spi_recv_dma_start);
        lua_setfield(L, -2, "recv_dma_start");
        lua_pushcfunction(L, lua_driver_spi_recv_dma_finish);
        lua_setfield(L, -2, "recv_dma_finish");
        lua_pushcfunction(L, lua_driver_spi_recv_it_start);
        lua_setfield(L, -2, "recv_it_start");
        lua_pushcfunction(L, lua_driver_spi_recv_it_finish);
        lua_setfield(L, -2, "recv_it_finish");
        lua_pushcfunction(L, lua_driver_spi_set_frequency);
        lua_setfield(L, -2, "set_frequency");
        lua_pushcfunction(L, lua_driver_spi_set_format);
        lua_setfield(L, -2, "set_format");
        lua_pushcfunction(L, lua_driver_spi_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_driver_spi_new);
    lua_setfield(L, -2, "new");
    return 1;
}

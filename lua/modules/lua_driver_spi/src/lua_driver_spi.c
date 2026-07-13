/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * lua_driver_spi.c — SPI Lua driver for RTL8721F
 *
 * Wraps raw fwlib SSI_* functions directly (no HAL layer).  Provides two
 * constructor functions:
 *   spi.new(idx, sclk, mosi, miso_or_nil, cs [, opts])       → master handle
 *   spi.new_slave(idx, sclk, mosi, miso_or_nil, cs [, opts]) → slave handle
 *
 * opts fields:
 *   cpol    — clock polarity (0/1, default 0)
 *   cpha    — clock phase    (0/1, default 0)
 *   div     — clock divider  (even integer >= 2, default 20 → 5 MHz)
 *   pinmux  — "dedicated" (default) or "full" (fully programmable per-role codes)
 *
 * Both handle types share the same set of methods:
 *   write(data)        — polling TX (discards mirrored RX bytes)
 *   read(len)          — polling RX (sends 0x00 dummy TX for master)
 *   write_intr(data)   — interrupt-driven TX; blocks until done
 *   read_intr(len)     — interrupt-driven RX; blocks until done
 *   write_dma(data)    — DMA TX; blocks until done
 *   read_dma(len)      — DMA RX; blocks until done
 *   close()            — invalidate handle (HW released at GC)
 *
 * Concurrency follows Template A (same pattern as lua_driver_i2c):
 *   - One rtos_mutex per controller, held for the whole transfer.
 *   - One rtos_sema per controller, used for ISR/DMA completion.
 *   - Reference counting: first open inits HW; last GC shuts it down.
 *   - Conflicting re-open (different role/pins/format) is rejected.
 *   - All longjmp-capable calls (luaL_error, lua_newuserdata) happen
 *     BEFORE rtos_mutex_take to guarantee the mutex is always given back.
 */

#include "lua_driver_spi.h"

#include <string.h>

#include "ameba_soc.h"
#include "ameba_gdma.h"
#include "lauxlib.h"
#include "luhw.h"
#include "os_wrapper.h"

/* ---- Constants ----------------------------------------------------------- */

#define LUA_DRIVER_SPI_MASTER_MT        "spi.master"
#define LUA_DRIVER_SPI_SLAVE_MT         "spi.slave"
#define LUA_DRIVER_SPI_NUM_CTRL         2
#define LUA_DRIVER_SPI_LOCK_TIMEOUT_MS  5000
#define LUA_DRIVER_SPI_XFER_TIMEOUT_MS  5000

/* Round n up to the nearest CACHE_LINE_SIZE multiple for DCache operations */
#define SPI_DCACHE_ALIGN(n) \
    (((u32)(n) + (u32)CACHE_LINE_SIZE - 1U) & ~((u32)CACHE_LINE_SIZE - 1U))

/* ---- Per-controller state ------------------------------------------------ */

typedef struct {
    rtos_mutex_t lock;
    rtos_sema_t  sema;
    int          refcnt;
    u8           inited;
    u8           role;       /* SSI_MASTER or SSI_SLAVE */
    u16          sclk_pin;
    u16          mosi_pin;
    u16          miso_pin;
    u16          cs_pin;
    u8           cpol;
    u8           cpha;
    u16          div;
    u8           full_pinmux; /* 1 = fully programmable per-role codes, 0 = dedicated */
    /* ISR volatile state */
    volatile const u8 *isr_tx_ptr; /* NULL → send dummy 0x00 */
    volatile u32       isr_tx_rem;
    volatile u8       *isr_rx_ptr; /* NULL → discard RX bytes */
    volatile u32       isr_rx_rem;
    /* DMA structs (re-armed per transfer) */
    GDMA_InitTypeDef dma_tx_gdma;
    GDMA_InitTypeDef dma_rx_gdma;
} spi_ctrl_t;

static spi_ctrl_t s_ctrl[LUA_DRIVER_SPI_NUM_CTRL];

/* Flag: 1 when a write_dma call also armed an RX DMA channel (master write) */
static volatile u8 s_dma_rx_active[LUA_DRIVER_SPI_NUM_CTRL];

/* ---- Userdata (shared by master and slave handles) ----------------------- */

typedef struct {
    int idx;
    u8  closed;
    u8  counted; /* 1 when this handle holds a refcnt on s_ctrl[idx] */
} lua_driver_spi_ud_t;

/* ---- Pinmux tables ------------------------------------------------------- */

/* Dedicated mode: all four roles share one function code per controller. */
static const u32 s_spi_clk_mux[LUA_DRIVER_SPI_NUM_CTRL]  = { PINMUX_FUNCTION_SPI0, PINMUX_FUNCTION_SPI1 };
static const u32 s_spi_mosi_mux[LUA_DRIVER_SPI_NUM_CTRL] = { PINMUX_FUNCTION_SPI0, PINMUX_FUNCTION_SPI1 };
static const u32 s_spi_miso_mux[LUA_DRIVER_SPI_NUM_CTRL] = { PINMUX_FUNCTION_SPI0, PINMUX_FUNCTION_SPI1 };
static const u32 s_spi_cs_mux[LUA_DRIVER_SPI_NUM_CTRL]   = { PINMUX_FUNCTION_SPI0, PINMUX_FUNCTION_SPI1 };

/* Fully programmable mode: each role gets its own function code. */
static const u32 s_spi_fp_clk_mux[LUA_DRIVER_SPI_NUM_CTRL]  = { PINMUX_FUNCTION_SPI0_CLK,  PINMUX_FUNCTION_SPI1_CLK };
static const u32 s_spi_fp_mosi_mux[LUA_DRIVER_SPI_NUM_CTRL] = { PINMUX_FUNCTION_SPI0_MOSI, PINMUX_FUNCTION_SPI1_MOSI };
static const u32 s_spi_fp_miso_mux[LUA_DRIVER_SPI_NUM_CTRL] = { PINMUX_FUNCTION_SPI0_MISO, PINMUX_FUNCTION_SPI1_MISO };
static const u32 s_spi_fp_cs_mux[LUA_DRIVER_SPI_NUM_CTRL]   = { PINMUX_FUNCTION_SPI0_CS,   PINMUX_FUNCTION_SPI1_CS };
static const u32 s_spi_periph[LUA_DRIVER_SPI_NUM_CTRL] = {
    APBPeriph_SPI0,
    APBPeriph_SPI1,
};
static const u32 s_spi_clk_en[LUA_DRIVER_SPI_NUM_CTRL] = {
    APBPeriph_SPI0_CLOCK,
    APBPeriph_SPI1_CLOCK,
};

/* ========================================================================= */
/* ISR / DMA callbacks                                                        */
/* ========================================================================= */

static u32 spi_irq_handler(void *param)
{
    u32          idx = (u32)(uintptr_t)param;
    spi_ctrl_t  *c   = &s_ctrl[idx];
    SPI_TypeDef *dev = SPI_DEV_TABLE[idx].SPIx;
    u32          isr = SSI_GetIsr(dev);
    u32          sent;

    SSI_SetIsrClean(dev, isr);

    /* TX-empty: load more bytes into FIFO */
    if (isr & SPI_BIT_TXEIS) {
        if (c->isr_tx_rem > 0) {
            sent = SSI_SendData(dev,
                                (u8 *)(uintptr_t)c->isr_tx_ptr,
                                c->isr_tx_rem,
                                c->role);
            if (c->isr_tx_ptr != NULL) {
                c->isr_tx_ptr += sent;
            }
            c->isr_tx_rem -= sent;
        }
        if (c->isr_tx_rem == 0) {
            SSI_INTConfig(dev, SPI_BIT_TXEIM | SPI_BIT_TXOIM, DISABLE);
            /* For slave write: done when TX FIFO drained (no RX expected) */
            if (c->role == SSI_SLAVE && c->isr_rx_rem == 0) {
                rtos_sema_give(c->sema);
            }
        }
    }

    /* RX-full threshold crossed: drain FIFO */
    if (isr & SPI_BIT_RXFIS) {
        if (c->isr_rx_rem > 0) {
            u32 got = SSI_ReceiveData(dev,
                                      (u8 *)(uintptr_t)c->isr_rx_ptr,
                                      c->isr_rx_rem);
            if (c->isr_rx_ptr != NULL) {
                c->isr_rx_ptr += got;
            }
            c->isr_rx_rem -= got;
        }
        if (c->isr_rx_rem == 0) {
            SSI_INTConfig(dev,
                          SPI_BIT_RXFIM | SPI_BIT_RXOIM | SPI_BIT_RXUIM,
                          DISABLE);
            rtos_sema_give(c->sema);
        }
    }

    /* Log FIFO overrun/underrun; SSI_SetIsrClean already cleared the flags */
    if (isr & (SPI_BIT_TXOIS | SPI_BIT_RXOIS | SPI_BIT_RXUIS)) {
        RTK_LOGS(NOTAG, RTK_LOG_WARN, "[spi%u] FIFO error isr=%08x\n", idx, isr);
    }

    return 0;
}

static u32 spi_dma_tx_cb(void *param)
{
    u32          idx = (u32)(uintptr_t)param;
    spi_ctrl_t  *c   = &s_ctrl[idx];
    SPI_TypeDef *dev = SPI_DEV_TABLE[idx].SPIx;

    GDMA_ClearINT(c->dma_tx_gdma.GDMA_Index, c->dma_tx_gdma.GDMA_ChNum);
    GDMA_Cmd(c->dma_tx_gdma.GDMA_Index, c->dma_tx_gdma.GDMA_ChNum, DISABLE);
    GDMA_ChnlFree(c->dma_tx_gdma.GDMA_Index, c->dma_tx_gdma.GDMA_ChNum);
    SSI_SetDmaEnable(dev, DISABLE, SPI_BIT_TDMAE);
    SSI_SlaveErrRecovery(dev);

    /* Give sema only when there is no paired RX DMA (slave write or no-RX) */
    if (!s_dma_rx_active[idx]) {
        rtos_sema_give(c->sema);
    }
    return 0;
}

static u32 spi_dma_rx_cb(void *param)
{
    u32          idx = (u32)(uintptr_t)param;
    spi_ctrl_t  *c   = &s_ctrl[idx];
    SPI_TypeDef *dev = SPI_DEV_TABLE[idx].SPIx;

    GDMA_ClearINT(c->dma_rx_gdma.GDMA_Index, c->dma_rx_gdma.GDMA_ChNum);
    GDMA_Cmd(c->dma_rx_gdma.GDMA_Index, c->dma_rx_gdma.GDMA_ChNum, DISABLE);
    GDMA_ChnlFree(c->dma_rx_gdma.GDMA_Index, c->dma_rx_gdma.GDMA_ChNum);
    SSI_SetDmaEnable(dev, DISABLE, SPI_BIT_RDMAE);
    SSI_SlaveErrRecovery(dev);

    rtos_sema_give(c->sema);
    return 0;
}

/* Abort in-flight DMA channels after a transfer timeout.
 * Mirrors the cleanup done by spi_dma_tx_cb / spi_dma_rx_cb on normal completion.
 * Must be called from task context (not ISR) with the controller lock held. */
static void spi_dma_abort(spi_ctrl_t *c, SPI_TypeDef *dev, int tx_armed, int rx_armed)
{
    if (tx_armed) {
        GDMA_Cmd(c->dma_tx_gdma.GDMA_Index, c->dma_tx_gdma.GDMA_ChNum, DISABLE);
        GDMA_ClearINT(c->dma_tx_gdma.GDMA_Index, c->dma_tx_gdma.GDMA_ChNum);
        GDMA_ChnlFree(c->dma_tx_gdma.GDMA_Index, c->dma_tx_gdma.GDMA_ChNum);
        SSI_SetDmaEnable(dev, DISABLE, SPI_BIT_TDMAE);
    }
    if (rx_armed) {
        GDMA_Cmd(c->dma_rx_gdma.GDMA_Index, c->dma_rx_gdma.GDMA_ChNum, DISABLE);
        GDMA_ClearINT(c->dma_rx_gdma.GDMA_Index, c->dma_rx_gdma.GDMA_ChNum);
        GDMA_ChnlFree(c->dma_rx_gdma.GDMA_Index, c->dma_rx_gdma.GDMA_ChNum);
        SSI_SetDmaEnable(dev, DISABLE, SPI_BIT_RDMAE);
    }
}

/* ========================================================================= */
/* Concurrency helpers                                                        */
/* ========================================================================= */

/* Acquire the per-controller mutex with a bounded timeout.
 * On timeout the mutex is NOT held, so luaL_error is safe here. */
static void spi_lock(lua_State *L, int idx)
{
    if (rtos_mutex_take(s_ctrl[idx].lock,
                        LUA_DRIVER_SPI_LOCK_TIMEOUT_MS) != RTK_SUCCESS) {
        luaL_error(L, "spi%d: controller busy (lock timeout)", idx);
    }
}

/* Release one reference on a controller.  Called from __gc only (Lua task
 * context, never ISR).  Uses RTOS_MAX_DELAY: safe because all I/O paths that
 * hold this lock are bounded by LUA_DRIVER_SPI_XFER_TIMEOUT_MS, so the lock
 * is always given back in finite time.
 * Must not call any longjmp-capable Lua API. */
static void spi_ctrl_release(int idx)
{
    spi_ctrl_t *c = &s_ctrl[idx];

    rtos_mutex_take(c->lock, RTOS_MAX_DELAY);
    if (c->refcnt > 0) {
        c->refcnt--;
    }
    if (c->refcnt == 0 && c->inited) {
        InterruptDis(SPI_DEV_TABLE[idx].IrqNum);
        InterruptUnRegister(SPI_DEV_TABLE[idx].IrqNum);
        SSI_Cmd(SPI_DEV_TABLE[idx].SPIx, DISABLE);
        c->inited = 0;
    }
    rtos_mutex_give(c->lock);
}

/* Open / attach to a controller.  All validation and userdata allocation must
 * have already been done by the caller (CONC-02: no longjmp inside lock). */
static void spi_open_ctrl(lua_State *L, int idx, u8 role,
                           u16 sclk_pin, u16 mosi_pin, u16 miso_pin,
                           u16 cs_pin, u8 cpol, u8 cpha, u16 div,
                           u8 full_pinmux)
{
    spi_ctrl_t *c = &s_ctrl[idx];

    spi_lock(L, idx);   /* may luaL_error before lock is held — that is fine */

    if (c->inited && c->refcnt > 0) {
        /* Controller already live: check for configuration conflict */
        int conflict = (c->role        != role)        ||
                       (c->sclk_pin   != sclk_pin)    ||
                       (c->mosi_pin   != mosi_pin)    ||
                       (c->miso_pin   != miso_pin)    ||
                       (c->cs_pin     != cs_pin)      ||
                       (c->cpol       != cpol)        ||
                       (c->cpha       != cpha)        ||
                       (c->div        != div)         ||
                       (c->full_pinmux != full_pinmux);
        if (conflict) {
            rtos_mutex_give(c->lock);
            luaL_error(L, "spi%d: already open with a different "
                          "role/pins/format", idx);
            return; /* not reached */
        }
        c->refcnt++;
        rtos_mutex_give(c->lock);
        return;
    }

    /* First user: configure hardware from scratch */
    RCC_PeriphClockCmd(s_spi_periph[idx], s_spi_clk_en[idx], ENABLE);

    /* SSI_SetRole must be called before SSI_Init */
    SSI_SetRole(SPI_DEV_TABLE[idx].SPIx, (u32)role);

    /* Pinmux — select dedicated or fully-programmable codes, skip _PNC pins */
    u32 clk_mux  = full_pinmux ? s_spi_fp_clk_mux[idx]  : s_spi_clk_mux[idx];
    u32 mosi_mux = full_pinmux ? s_spi_fp_mosi_mux[idx] : s_spi_mosi_mux[idx];
    u32 miso_mux = full_pinmux ? s_spi_fp_miso_mux[idx] : s_spi_miso_mux[idx];
    u32 cs_mux   = full_pinmux ? s_spi_fp_cs_mux[idx]   : s_spi_cs_mux[idx];
    if (sclk_pin != _PNC) {
        Pinmux_Config((u8)sclk_pin, clk_mux);
    }
    if (mosi_pin != _PNC) {
        Pinmux_Config((u8)mosi_pin, mosi_mux);
    }
    if (cs_pin != _PNC) {
        Pinmux_Config((u8)cs_pin, cs_mux);
        PAD_PullCtrl((u8)cs_pin, GPIO_PuPd_UP);
    }
    if (miso_pin != _PNC) {
        Pinmux_Config((u8)miso_pin, miso_mux);
    }

    /* SSI_Init */
    SSI_InitTypeDef ssi;
    SSI_StructInit(&ssi);
    ssi.SPI_Role          = (u32)role;
    ssi.SPI_DataFrameSize = DFS_8_BITS;
    ssi.SPI_SclkPolarity  = cpol ? SCPOL_INACTIVE_IS_HIGH : SCPOL_INACTIVE_IS_LOW;
    ssi.SPI_SclkPhase     = cpha ? SCPH_TOGGLES_AT_START  : SCPH_TOGGLES_IN_MIDDLE;
    ssi.SPI_ClockDivider  = div;
    SSI_Init(SPI_DEV_TABLE[idx].SPIx, &ssi);
    SSI_Cmd(SPI_DEV_TABLE[idx].SPIx, ENABLE);

    /* Register (but do not yet enable) the controller IRQ */
    InterruptRegister((IRQ_FUN)spi_irq_handler,
                      SPI_DEV_TABLE[idx].IrqNum,
                      (u32)(uintptr_t)idx, 5);
    InterruptEn(SPI_DEV_TABLE[idx].IrqNum, 5);

    c->role     = role;
    c->sclk_pin = sclk_pin;
    c->mosi_pin = mosi_pin;
    c->miso_pin = miso_pin;
    c->cs_pin   = cs_pin;
    c->cpol        = cpol;
    c->cpha        = cpha;
    c->div         = div;
    c->full_pinmux = full_pinmux;
    c->inited      = 1;
    c->refcnt   = 1;

    rtos_mutex_give(c->lock);
}

/* Drain any stale completion signal before starting a new ISR/DMA transfer */
static void spi_drain_sema(spi_ctrl_t *c)
{
    while (rtos_sema_take(c->sema, 0) == RTK_SUCCESS) {}
}

/* ========================================================================= */
/* Userdata helpers                                                           */
/* ========================================================================= */

/* Accept both master and slave metatables at the same stack position. */
static lua_driver_spi_ud_t *spi_get_ud_any(lua_State *L, int stack_idx)
{
    lua_driver_spi_ud_t *ud =
        (lua_driver_spi_ud_t *)luaL_testudata(L, stack_idx,
                                               LUA_DRIVER_SPI_MASTER_MT);
    if (!ud) {
        ud = (lua_driver_spi_ud_t *)luaL_testudata(L, stack_idx,
                                                    LUA_DRIVER_SPI_SLAVE_MT);
    }
    if (!ud) {
        luaL_error(L, "spi: expected spi.master or spi.slave handle");
    }
    if (ud->closed) {
        luaL_error(L, "spi: handle is closed");
    }
    return ud;
}

/* ========================================================================= */
/* Constructors                                                               */
/* ========================================================================= */

/* Common body for spi.new and spi.new_slave */
static int spi_new_common(lua_State *L, u8 role, const char *mt)
{
    lua_Integer idx  = luaL_checkinteger(L, 1);
    u16 sclk = (u16)luhw_check_pin(L, 2);
    u16 mosi = (u16)luhw_check_pin(L, 3);
    /* arg 4: miso pin name or nil/none → _PNC (e.g. write-only devices like ST7789) */
    u16 miso = _PNC;
    if (!lua_isnoneornil(L, 4)) {
        miso = (u16)luhw_check_pin(L, 4);
    }
    u16 cs = (u16)luhw_check_pin(L, 5);

    if (idx < 0 || idx >= LUA_DRIVER_SPI_NUM_CTRL) {
        return luaL_error(L, "spi: idx must be 0 or 1");
    }

    /* Parse opts table (arg 6) */
    u8  cpol       = 0;
    u8  cpha       = 0;
    u16 div        = 20; /* 100 MHz / 20 = 5 MHz */
    u8  full_pinmux = 0; /* 0 = dedicated, 1 = fully programmable */

    if (!lua_isnoneornil(L, 6)) {
        luaL_checktype(L, 6, LUA_TTABLE);
        lua_getfield(L, 6, "cpol");
        if (!lua_isnil(L, -1)) {
            cpol = (u8)luaL_checkinteger(L, -1);
        }
        lua_pop(L, 1);
        lua_getfield(L, 6, "cpha");
        if (!lua_isnil(L, -1)) {
            cpha = (u8)luaL_checkinteger(L, -1);
        }
        lua_pop(L, 1);
        lua_getfield(L, 6, "div");
        if (!lua_isnil(L, -1)) {
            lua_Integer d = luaL_checkinteger(L, -1);
            if (d < 2) {
                lua_pop(L, 1);
                return luaL_error(L, "spi: div must be >= 2");
            }
            div = (u16)d;
        }
        lua_pop(L, 1);
        lua_getfield(L, 6, "pinmux");
        if (!lua_isnil(L, -1)) {
            const char *pm = luaL_checkstring(L, -1);
            if (strcmp(pm, "full") == 0) {
                full_pinmux = 1;
            }
        }
        lua_pop(L, 1);
    }

    /* Ensure div is even (SSI requirement) */
    if (div & 1) {
        div++;
    }

    int i = (int)idx;

    /* Allocate userdata BEFORE taking the lock (CONC-02) */
    lua_driver_spi_ud_t *ud =
        (lua_driver_spi_ud_t *)lua_newuserdata(L, sizeof(*ud));
    ud->idx     = i;
    ud->closed  = 0;
    ud->counted = 0;
    luaL_getmetatable(L, mt);
    lua_setmetatable(L, -2);

    /* Open / reference the controller (may luaL_error on conflict) */
    spi_open_ctrl(L, i, role,
                  sclk, mosi, miso, cs,
                  cpol, cpha, div, full_pinmux);
    ud->counted = 1;

    return 1;
}

static int lua_driver_spi_new(lua_State *L)
{
    return spi_new_common(L, SSI_MASTER, LUA_DRIVER_SPI_MASTER_MT);
}

static int lua_driver_spi_new_slave(lua_State *L)
{
    return spi_new_common(L, SSI_SLAVE, LUA_DRIVER_SPI_SLAVE_MT);
}

/* ========================================================================= */
/* Polling transfer methods                                                   */
/* ========================================================================= */

/* write(data) — send bytes.
 * Master: TX and RX are simultaneous; mirrored RX bytes are discarded.
 * Slave:  preload TX FIFO only; master drives the clock later to pull
 *         the bytes out.  No RX wait (slave never generates the clock). */
static int lua_driver_spi_write(lua_State *L)
{
    lua_driver_spi_ud_t *ud  = spi_get_ud_any(L, 1);
    spi_ctrl_t          *c   = &s_ctrl[ud->idx];
    SPI_TypeDef         *dev = SPI_DEV_TABLE[ud->idx].SPIx;
    size_t               len = 0;
    const char          *data;
    u32                  elapsed;
    u32                  i;

    if (lua_type(L, 2) == LUA_TSTRING) {
        data = lua_tolstring(L, 2, &len);
    } else {
        return luaL_error(L, "spi write: expected string");
    }

    if (len == 0) {
        return 0;
    }

    spi_lock(L, ud->idx);

    if (c->role == SSI_SLAVE) {
        /* Slave: preload TX FIFO, then return immediately. */
        for (i = 0; i < (u32)len; i++) {
            elapsed = 0;
            while (!SSI_Writeable(dev)) {
                rtos_time_delay_ms(1);
                if (++elapsed > LUA_DRIVER_SPI_XFER_TIMEOUT_MS) {
                    SSI_SlaveErrRecovery(dev);
                    rtos_mutex_give(c->lock);
                    return luaL_error(L, "spi write: TX FIFO full (slave preload timeout)");
                }
            }
            SSI_WriteData(dev, (u8)data[i]);
        }
    } else {
        /* Master: TX and RX simultaneous; drain each mirrored RX byte. */
        for (i = 0; i < (u32)len; i++) {
            elapsed = 0;
            while (!SSI_Writeable(dev)) {
                rtos_time_delay_ms(1);
                if (++elapsed > LUA_DRIVER_SPI_XFER_TIMEOUT_MS) {
                    SSI_SlaveErrRecovery(dev);
                    rtos_mutex_give(c->lock);
                    return luaL_error(L, "spi write: TX timeout");
                }
            }
            SSI_WriteData(dev, (u8)data[i]);

            elapsed = 0;
            while (!SSI_Readable(dev)) {
                rtos_time_delay_ms(1);
                if (++elapsed > LUA_DRIVER_SPI_XFER_TIMEOUT_MS) {
                    SSI_SlaveErrRecovery(dev);
                    rtos_mutex_give(c->lock);
                    return luaL_error(L, "spi write: RX drain timeout");
                }
            }
            SSI_ReadData(dev); /* discard mirrored byte */
        }
    }

    rtos_mutex_give(c->lock);
    return 0;
}

/* read(len) — receive len bytes.
 * Master: writes 0x00 dummy bytes to generate the SPI clock, then reads RX.
 * Slave:  just reads from RX FIFO (master drives the clock; no TX write
 *         to avoid polluting the TX FIFO with stale 0x00 bytes). */
static int lua_driver_spi_read(lua_State *L)
{
    lua_driver_spi_ud_t *ud  = spi_get_ud_any(L, 1);
    spi_ctrl_t          *c   = &s_ctrl[ud->idx];
    SPI_TypeDef         *dev = SPI_DEV_TABLE[ud->idx].SPIx;
    lua_Integer          n   = luaL_checkinteger(L, 2);
    u32                  elapsed;
    u32                  i;

    if (n <= 0) {
        return luaL_error(L, "spi read: len must be >= 1");
    }

    /* Allocate result buffer on Lua stack as a userdata (avoids C stack) */
    u8 *buf = (u8 *)lua_newuserdata(L, (size_t)n);

    spi_lock(L, ud->idx);

    if (c->role == SSI_SLAVE) {
        /* Slave: master already drove the clock; just drain RX FIFO. */
        for (i = 0; i < (u32)n; i++) {
            elapsed = 0;
            while (!SSI_Readable(dev)) {
                rtos_time_delay_ms(1);
                if (++elapsed > LUA_DRIVER_SPI_XFER_TIMEOUT_MS) {
                    SSI_SlaveErrRecovery(dev);
                    rtos_mutex_give(c->lock);
                    return luaL_error(L, "spi read: RX timeout (slave)");
                }
            }
            buf[i] = (u8)SSI_ReadData(dev);
        }
    } else {
        /* Master: write dummy 0x00 to generate clock, then read RX. */
        for (i = 0; i < (u32)n; i++) {
            elapsed = 0;
            while (!SSI_Writeable(dev)) {
                rtos_time_delay_ms(1);
                if (++elapsed > LUA_DRIVER_SPI_XFER_TIMEOUT_MS) {
                    SSI_SlaveErrRecovery(dev);
                    rtos_mutex_give(c->lock);
                    return luaL_error(L, "spi read: TX timeout");
                }
            }
            SSI_WriteData(dev, 0x00); /* dummy TX to generate clock */

            elapsed = 0;
            while (!SSI_Readable(dev)) {
                rtos_time_delay_ms(1);
                if (++elapsed > LUA_DRIVER_SPI_XFER_TIMEOUT_MS) {
                    SSI_SlaveErrRecovery(dev);
                    rtos_mutex_give(c->lock);
                    return luaL_error(L, "spi read: RX timeout");
                }
            }
            buf[i] = (u8)SSI_ReadData(dev);
        }
    }

    rtos_mutex_give(c->lock);
    lua_pushlstring(L, (const char *)buf, (size_t)n);
    return 1;
}

/* ========================================================================= */
/* Interrupt-driven methods                                                   */
/* ========================================================================= */

/* write_intr(data) — ISR-driven TX.
 * Master: blocks until all TX bytes are sent AND all mirrored RX bytes are
 *         drained (isr_rx_rem tracks this).
 * Slave:  preloads TX FIFO; blocks until TX FIFO is drained by the master. */
static int lua_driver_spi_write_intr(lua_State *L)
{
    lua_driver_spi_ud_t *ud   = spi_get_ud_any(L, 1);
    spi_ctrl_t          *c    = &s_ctrl[ud->idx];
    SPI_TypeDef         *dev  = SPI_DEV_TABLE[ud->idx].SPIx;
    size_t               len  = 0;
    const char          *data;

    if (lua_type(L, 2) == LUA_TSTRING) {
        data = lua_tolstring(L, 2, &len);
    } else {
        return luaL_error(L, "spi write_intr: expected string");
    }

    if (len == 0) {
        return 0;
    }

    spi_lock(L, ud->idx);
    spi_drain_sema(c);

    c->isr_tx_ptr = (const u8 *)data;
    c->isr_tx_rem = (u32)len;

    if (c->role == SSI_MASTER) {
        c->isr_rx_ptr = NULL;           /* discard mirrored RX */
        c->isr_rx_rem = (u32)len;
    } else {
        c->isr_rx_ptr = NULL;
        c->isr_rx_rem = 0;              /* slave write: no RX expected */
    }

    SSI_SetRxFifoLevel(dev, 0); /* fire at >= 1 byte */

    if (c->role == SSI_MASTER) {
        SSI_INTConfig(dev,
                      SPI_BIT_TXEIM | SPI_BIT_TXOIM |
                      SPI_BIT_RXFIM | SPI_BIT_RXOIM | SPI_BIT_RXUIM,
                      ENABLE);
    } else {
        SSI_INTConfig(dev, SPI_BIT_TXEIM | SPI_BIT_TXOIM, ENABLE);
    }

    if (rtos_sema_take(c->sema, LUA_DRIVER_SPI_XFER_TIMEOUT_MS) != RTK_SUCCESS) {
        SSI_INTConfig(dev,
                      SPI_BIT_TXEIM | SPI_BIT_TXOIM |
                      SPI_BIT_RXFIM | SPI_BIT_RXOIM | SPI_BIT_RXUIM,
                      DISABLE);
        rtos_mutex_give(c->lock);
        return luaL_error(L, "spi write_intr: timeout");
    }

    rtos_mutex_give(c->lock);
    return 0;
}

/* read_intr(len) — ISR-driven RX.
 * Master: also triggers TX of dummy 0x00 bytes to generate the SPI clock.
 * Slave:  just waits for incoming bytes from the master. */
static int lua_driver_spi_read_intr(lua_State *L)
{
    lua_driver_spi_ud_t *ud  = spi_get_ud_any(L, 1);
    lua_Integer          n   = luaL_checkinteger(L, 2);
    spi_ctrl_t          *c   = &s_ctrl[ud->idx];
    SPI_TypeDef         *dev = SPI_DEV_TABLE[ud->idx].SPIx;

    if (n <= 0) {
        return luaL_error(L, "spi read_intr: len must be >= 1");
    }

    spi_lock(L, ud->idx);
    spi_drain_sema(c);

    u8 *isr_rx_buf = rtos_mem_malloc((u32)n);
    if (!isr_rx_buf) {
        rtos_mutex_give(c->lock);
        return luaL_error(L, "spi read_intr: malloc failed");
    }

    c->isr_rx_ptr = isr_rx_buf;
    c->isr_rx_rem = (u32)n;

    if (c->role == SSI_MASTER) {
        c->isr_tx_ptr = NULL;   /* send dummy 0x00 */
        c->isr_tx_rem = (u32)n;
    } else {
        c->isr_tx_ptr = NULL;
        c->isr_tx_rem = 0;      /* slave: master provides the clocks */
    }

    SSI_SetRxFifoLevel(dev, 0);

    if (c->role == SSI_MASTER) {
        SSI_INTConfig(dev,
                      SPI_BIT_TXEIM | SPI_BIT_TXOIM |
                      SPI_BIT_RXFIM | SPI_BIT_RXOIM | SPI_BIT_RXUIM,
                      ENABLE);
    } else {
        SSI_INTConfig(dev,
                      SPI_BIT_RXFIM | SPI_BIT_RXOIM | SPI_BIT_RXUIM,
                      ENABLE);
    }

    if (rtos_sema_take(c->sema, LUA_DRIVER_SPI_XFER_TIMEOUT_MS) != RTK_SUCCESS) {
        SSI_INTConfig(dev,
                      SPI_BIT_TXEIM | SPI_BIT_TXOIM |
                      SPI_BIT_RXFIM | SPI_BIT_RXOIM | SPI_BIT_RXUIM,
                      DISABLE);
        rtos_mutex_give(c->lock);
        rtos_mem_free(isr_rx_buf);
        return luaL_error(L, "spi read_intr: timeout");
    }

    rtos_mutex_give(c->lock);
    lua_pushlstring(L, (const char *)isr_rx_buf, (size_t)n);
    rtos_mem_free(isr_rx_buf);
    return 1;
}

/* ========================================================================= */
/* DMA transfer methods                                                       */
/* ========================================================================= */

/* Allocate a DMA-capable buffer with 64-byte address alignment.
 * Requests (size + 63) bytes from the heap so that the aligned pointer always
 * falls within the allocation.  The original (unaligned) pointer is stored in
 * *raw_out and must be passed to rtos_mem_free().
 * Returns the 64-byte-aligned pointer, or NULL on allocation failure. */
static u8 *spi_dma_alloc(size_t size, u8 **raw_out)
{
    u8 *raw = rtos_mem_malloc((u32)(size + 63U));
    if (!raw) {
        return NULL;
    }
    *raw_out = raw;
    return (u8 *)(((uintptr_t)raw + 63U) & ~(uintptr_t)63U);
}

/* write_dma(data) — DMA-driven TX.
 * Master: arms TX DMA and RX DMA (to drain the RX FIFO filled by the SPI HW).
 *         Waits for the RX DMA completion interrupt.
 * Slave:  arms TX DMA only (master drives clocks to pull data).
 *         Waits for TX DMA completion interrupt. */
static int lua_driver_spi_write_dma(lua_State *L)
{
    lua_driver_spi_ud_t *ud   = spi_get_ud_any(L, 1);
    spi_ctrl_t          *c    = &s_ctrl[ud->idx];
    SPI_TypeDef         *dev  = SPI_DEV_TABLE[ud->idx].SPIx;
    size_t               len  = 0;
    const char          *data;

    if (lua_type(L, 2) == LUA_TSTRING) {
        data = lua_tolstring(L, 2, &len);
    } else {
        return luaL_error(L, "spi write_dma: expected string");
    }

    if (len == 0) {
        return 0;
    }

    spi_lock(L, ud->idx);
    spi_drain_sema(c);

    /* Allocate aligned TX buffer from heap; flush CPU writes to SRAM for DMA */
    u8 *tx_raw = NULL;
    u8 *tx_buf = spi_dma_alloc(len, &tx_raw);
    if (!tx_buf) {
        rtos_mutex_give(c->lock);
        return luaL_error(L, "spi write_dma: malloc failed");
    }
    memcpy(tx_buf, data, len);
    DCache_Clean((u32)tx_buf, SPI_DCACHE_ALIGN(len));

    /* Master also needs a sink buffer to absorb the mirrored RX bytes */
    u8 *rx_raw = NULL;
    u8 *rx_buf = NULL;
    if (c->role == SSI_MASTER) {
        rx_buf = spi_dma_alloc(len, &rx_raw);
        if (!rx_buf) {
            rtos_mutex_give(c->lock);
            rtos_mem_free(tx_raw);
            return luaL_error(L, "spi write_dma: malloc failed (rx)");
        }
        /* RX DMA must be set up before TX DMA so no RX bytes are lost */
        s_dma_rx_active[ud->idx] = 1;
        SSI_RXGDMA_Init((u8)ud->idx, &c->dma_rx_gdma, (void *)(uintptr_t)ud->idx,
                        (IRQ_FUN)spi_dma_rx_cb,
                        rx_buf, (u32)len);
        SSI_SetDmaEnable(dev, ENABLE, SPI_BIT_RDMAE);
        SSI_TXGDMA_Init((u32)ud->idx, &c->dma_tx_gdma, (void *)(uintptr_t)ud->idx,
                        (IRQ_FUN)spi_dma_tx_cb,
                        tx_buf, (u32)len);
        SSI_SetDmaEnable(dev, ENABLE, SPI_BIT_TDMAE);
    } else {
        /* Slave: TX DMA only; no RX DMA */
        s_dma_rx_active[ud->idx] = 0;
        SSI_TXGDMA_Init((u32)ud->idx, &c->dma_tx_gdma, (void *)(uintptr_t)ud->idx,
                        (IRQ_FUN)spi_dma_tx_cb,
                        tx_buf, (u32)len);
        SSI_SetDmaEnable(dev, ENABLE, SPI_BIT_TDMAE);
    }

    if (rtos_sema_take(c->sema, LUA_DRIVER_SPI_XFER_TIMEOUT_MS) != RTK_SUCCESS) {
        /* TX always armed; RX armed only for master (s_dma_rx_active tracks this) */
        spi_dma_abort(c, dev, 1, s_dma_rx_active[ud->idx]);
        s_dma_rx_active[ud->idx] = 0;
        rtos_mutex_give(c->lock);
        rtos_mem_free(tx_raw);
        if (rx_raw) {
            rtos_mem_free(rx_raw);
        }
        return luaL_error(L, "spi write_dma: timeout");
    }

    rtos_mutex_give(c->lock);
    rtos_mem_free(tx_raw);
    if (rx_raw) {
        rtos_mem_free(rx_raw);
    }
    return 0;
}

/* read_dma(len) — DMA-driven RX.
 * Master: arms RX DMA and TX DMA (dummy 0x00 source) to generate the SPI clock.
 *         Waits for RX DMA completion.
 * Slave:  arms RX DMA only; waits for master to drive clocks. */
static int lua_driver_spi_read_dma(lua_State *L)
{
    lua_driver_spi_ud_t *ud  = spi_get_ud_any(L, 1);
    lua_Integer          n   = luaL_checkinteger(L, 2);
    spi_ctrl_t          *c   = &s_ctrl[ud->idx];
    SPI_TypeDef         *dev = SPI_DEV_TABLE[ud->idx].SPIx;

    if (n <= 0) {
        return luaL_error(L, "spi read_dma: len must be >= 1");
    }

    spi_lock(L, ud->idx);
    spi_drain_sema(c);

    /* Allocate aligned RX buffer; DMA writes directly to SRAM */
    u8 *rx_raw = NULL;
    u8 *rx_buf = spi_dma_alloc((size_t)n, &rx_raw);
    if (!rx_buf) {
        rtos_mutex_give(c->lock);
        return luaL_error(L, "spi read_dma: malloc failed");
    }

    u8 *tx_raw = NULL;
    u8 *tx_buf = NULL;
    s_dma_rx_active[ud->idx] = 1;

    if (c->role == SSI_MASTER) {
        tx_buf = spi_dma_alloc((size_t)n, &tx_raw);
        if (!tx_buf) {
            rtos_mutex_give(c->lock);
            rtos_mem_free(rx_raw);
            return luaL_error(L, "spi read_dma: malloc failed (tx)");
        }
        memset(tx_buf, 0x00, (size_t)n);
        DCache_Clean((u32)tx_buf, SPI_DCACHE_ALIGN((u32)n));
        SSI_RXGDMA_Init((u8)ud->idx, &c->dma_rx_gdma, (void *)(uintptr_t)ud->idx,
                        (IRQ_FUN)spi_dma_rx_cb,
                        rx_buf, (u32)n);
        SSI_SetDmaEnable(dev, ENABLE, SPI_BIT_RDMAE);
        SSI_TXGDMA_Init((u32)ud->idx, &c->dma_tx_gdma, (void *)(uintptr_t)ud->idx,
                        (IRQ_FUN)spi_dma_tx_cb,
                        tx_buf, (u32)n);
        SSI_SetDmaEnable(dev, ENABLE, SPI_BIT_TDMAE);
    } else {
        /* Slave: RX DMA only */
        SSI_RXGDMA_Init((u8)ud->idx, &c->dma_rx_gdma, (void *)(uintptr_t)ud->idx,
                        (IRQ_FUN)spi_dma_rx_cb,
                        rx_buf, (u32)n);
        SSI_SetDmaEnable(dev, ENABLE, SPI_BIT_RDMAE);
    }

    if (rtos_sema_take(c->sema, LUA_DRIVER_SPI_XFER_TIMEOUT_MS) != RTK_SUCCESS) {
        /* RX always armed; TX armed only for master */
        spi_dma_abort(c, dev, (c->role == SSI_MASTER), 1);
        s_dma_rx_active[ud->idx] = 0;
        rtos_mutex_give(c->lock);
        rtos_mem_free(rx_raw);
        if (tx_raw) {
            rtos_mem_free(tx_raw);
        }
        return luaL_error(L, "spi read_dma: timeout");
    }

    rtos_mutex_give(c->lock);
    /* Invalidate CPU cache for RX region so the CPU reads DMA-written SRAM data */
    DCache_Invalidate((u32)rx_buf, SPI_DCACHE_ALIGN((u32)n));
    lua_pushlstring(L, (const char *)rx_buf, (size_t)n);
    rtos_mem_free(rx_raw);
    if (tx_raw) {
        rtos_mem_free(tx_raw);
    }
    return 1;
}

/* ========================================================================= */
/* close / GC                                                                 */
/* ========================================================================= */

/* close() — invalidate the handle; hardware released at __gc */
static int lua_driver_spi_close(lua_State *L)
{
    /* Accept either metatable */
    lua_driver_spi_ud_t *ud =
        (lua_driver_spi_ud_t *)luaL_testudata(L, 1, LUA_DRIVER_SPI_MASTER_MT);
    if (!ud) {
        ud = (lua_driver_spi_ud_t *)luaL_testudata(L, 1,
                                                    LUA_DRIVER_SPI_SLAVE_MT);
    }
    if (ud && !ud->closed) {
        ud->closed = 1;
    }
    return 0;
}

static int lua_driver_spi_gc(lua_State *L)
{
    /* Try both metatables */
    lua_driver_spi_ud_t *ud =
        (lua_driver_spi_ud_t *)luaL_testudata(L, 1, LUA_DRIVER_SPI_MASTER_MT);
    if (!ud) {
        ud = (lua_driver_spi_ud_t *)luaL_testudata(L, 1,
                                                    LUA_DRIVER_SPI_SLAVE_MT);
    }
    if (ud && ud->counted) {
        spi_ctrl_release(ud->idx);
        ud->counted = 0;
    }
    return 0;
}

/* ========================================================================= */
/* Driver init / provision                                                    */
/* ========================================================================= */

void lua_driver_spi_init(void)
{
    int i;
    for (i = 0; i < LUA_DRIVER_SPI_NUM_CTRL; i++) {
        if (s_ctrl[i].lock == NULL) {
            rtos_mutex_create(&s_ctrl[i].lock);
        }
        if (s_ctrl[i].sema == NULL) {
            rtos_sema_create_binary(&s_ctrl[i].sema);
        }
        s_ctrl[i].refcnt  = 0;
        s_ctrl[i].inited  = 0;
        s_dma_rx_active[i] = 0;
    }
}

/* ========================================================================= */
/* Module open                                                                */
/* ========================================================================= */

/* Register methods into a metatable already on top of the stack.
 * The __gc and __index fields are set by the caller. */
static void spi_register_methods(lua_State *L)
{
    lua_pushcfunction(L, lua_driver_spi_write);
    lua_setfield(L, -2, "write");
    lua_pushcfunction(L, lua_driver_spi_read);
    lua_setfield(L, -2, "read");
    lua_pushcfunction(L, lua_driver_spi_write_intr);
    lua_setfield(L, -2, "write_intr");
    lua_pushcfunction(L, lua_driver_spi_read_intr);
    lua_setfield(L, -2, "read_intr");
    lua_pushcfunction(L, lua_driver_spi_write_dma);
    lua_setfield(L, -2, "write_dma");
    lua_pushcfunction(L, lua_driver_spi_read_dma);
    lua_setfield(L, -2, "read_dma");
    lua_pushcfunction(L, lua_driver_spi_close);
    lua_setfield(L, -2, "close");
}

int luaopen_spi(lua_State *L)
{
    /* spi.master metatable */
    if (luaL_newmetatable(L, LUA_DRIVER_SPI_MASTER_MT)) {
        lua_pushcfunction(L, lua_driver_spi_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        spi_register_methods(L);
    }
    lua_pop(L, 1);

    /* spi.slave metatable */
    if (luaL_newmetatable(L, LUA_DRIVER_SPI_SLAVE_MT)) {
        lua_pushcfunction(L, lua_driver_spi_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        spi_register_methods(L);
    }
    lua_pop(L, 1);

    /* Module table */
    lua_newtable(L);
    lua_pushcfunction(L, lua_driver_spi_new);
    lua_setfield(L, -2, "new");
    lua_pushcfunction(L, lua_driver_spi_new_slave);
    lua_setfield(L, -2, "new_slave");
    return 1;
}

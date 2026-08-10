/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * lua_module_led_strip.c — WS2812 / WS2812B addressable RGB strip via SPI DMA.
 *
 * Only MOSI is wired to the strip DIN; SCLK/MISO/CS are configured on the
 * controller but not connected to anything external.
 *
 * Encoding (2.5 MHz SPI, 1 SPI bit = 400 ns, MSB-first):
 *   WS2812 '0' → SPI 0b100  (400 ns high, 800 ns low)
 *   WS2812 '1' → SPI 0b110  (800 ns high, 400 ns low)
 * One WS2812 bit → 3 SPI bits; one colour byte → 3 SPI bytes; one pixel → 9
 * SPI bytes (GRB wire order).
 *
 * DMA buffer layout (zero-initialised at alloc; reset regions never written):
 *   [LED_LEAD_RESET × 0x00] [count × 9 pixel bytes] [LED_TRAIL_RESET × 0x00]
 * The leading zeros re-address the strip to pixel 1 before data; the trailing
 * zeros latch the frame (> 280 µs satisfies both WS2812 and WS2812B).
 *
 * Lua API (require "led_strip"):
 *   local s = led_strip.new({spi=1, mosi="PB_8", count=15 [, pinmux="full"]})
 *   s:set_pixel(i, r, g, b)        -- i 1-based; r/g/b 0..255
 *   s:set_pixel_hsv(i, h, s, v)    -- h 0..359, s/v 0..255
 *   s:fill(r, g, b)
 *   s:fill_hsv(h, s, v)
 *   s:clear()
 *   s:show()                        -- encode + DMA, blocks until done
 *   s:close()
 *   led_strip.stop_requested()      -- true once AT+CLAW=led,off was issued
 *
 * The AT+CLAW=led[,loop|off] test runners live in
 * test/lua_led_strip_test_provision.c (compiled only when CONFIG_CLAW_ENABLE_TESTS).
 */

#include <string.h>
#include <stdio.h>

#include "ameba_soc.h"
#include "ameba_gdma.h"
#include "os_wrapper.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luhw.h"

#ifndef LUAMOD_API
#define LUAMOD_API extern
#endif

#define TAG "led_strip"

/* ---- Timing / encoding --------------------------------------------------- */

#define LED_CLK_DIV        40u  /* 100 MHz / 40 = 2.5 MHz → 1 bit = 400 ns */
#define LED_BYTES_PER_PIX   9u  /* 3 colour bytes × 3 SPI bytes each */
#define LED_LEAD_RESET     32u  /* 32 × 8 × 400 ns ≈ 102 µs */
#define LED_TRAIL_RESET    96u  /* 96 × 8 × 400 ns ≈ 307 µs > WS2812B 280 µs */

#define LED_MT             "led_strip.handle"
#define LED_NUM_CTRL        2
#define LED_LOCK_MS        2000u
#define LED_DMA_MS         1000u

/* ---- Pinmux tables ------------------------------------------------------- */

/* Dedicated: one code per controller covers all roles on any valid pin. */
static const u32 s_ded_mosi[LED_NUM_CTRL] = {
    PINMUX_FUNCTION_SPI0,
    PINMUX_FUNCTION_SPI1,
};
/* Full matrix: per-role code; any GPIO can carry any SPI signal. */
static const u32 s_fp_mosi[LED_NUM_CTRL] = {
    PINMUX_FUNCTION_SPI0_MOSI,
    PINMUX_FUNCTION_SPI1_MOSI,
};
static const u32 s_periph[LED_NUM_CTRL] = {
    APBPeriph_SPI0,
    APBPeriph_SPI1,
};
static const u32 s_clk[LED_NUM_CTRL] = {
    APBPeriph_SPI0_CLOCK,
    APBPeriph_SPI1_CLOCK,
};

/* ---- Per-controller state ------------------------------------------------- */

typedef struct {
    rtos_mutex_t     lock;
    rtos_sema_t      sema;
    int              refcnt;
    u8               inited;
    SPI_TypeDef     *spi_dev;
    GDMA_InitTypeDef gdma_tx;
} led_ctrl_t;

static led_ctrl_t s_ctrl[LED_NUM_CTRL];

/* ---- Handle userdata ----------------------------------------------------- */

typedef struct {
    int   idx;       /* controller index, 0 or 1 */
    int   count;     /* pixel count */
    u8   *grb;       /* count * 3 bytes: G, R, B per pixel */
    u8   *dma_raw;   /* original rtos_mem_zmalloc pointer */
    u8   *dma_buf;   /* cache-line-aligned view of dma_raw */
    u32   dma_size;  /* aligned buffer size */
    u8    closed;
    u8    counted;   /* 1 when this handle holds a refcnt on s_ctrl[idx] */
} led_ud_t;

/* ---- Background-loop control --------------------------------------------- */
/* Non-static: the AT test runners live in test/lua_led_strip_test_provision.c
 * (compiled only when CONFIG_CLAW_ENABLE_TESTS) and drive these flags; the
 * production stop_requested() below reads led_strip_loop_stop. */
volatile int led_strip_loop_stop    = 0;
volatile int led_strip_loop_running = 0;

/* ---- Internal helpers ---------------------------------------------------- */

static inline u32 led_dma_size(int count)
{
    u32 raw = LED_LEAD_RESET + (u32)count * LED_BYTES_PER_PIX + LED_TRAIL_RESET;
    return (raw + (u32)CACHE_LINE_SIZE - 1u) & ~((u32)CACHE_LINE_SIZE - 1u);
}

/* Expand one 8-bit colour value to 3 SPI bytes, MSB first.
 * Each WS2812 bit maps to 3 SPI bits: '1' → 0b110, '0' → 0b100. */
static void led_encode_byte(u8 *dst, u8 val)
{
    u32 acc = 0;
    int i;
    for (i = 0; i < 8; i++) {
        acc <<= 3;
        acc |= (val & 0x80u) ? 0x6u : 0x4u;
        val = (u8)(val << 1);
    }
    dst[0] = (u8)(acc >> 16);
    dst[1] = (u8)(acc >>  8);
    dst[2] = (u8)(acc);
}

/* Encode current GRB buffer into the pixel region of dma_buf.
 * Reset regions (leading and trailing) are always zero and never touched. */
static void led_encode_frame(const led_ud_t *ud)
{
    u8       *dst = ud->dma_buf + LED_LEAD_RESET;
    const u8 *src = ud->grb;
    int i;
    for (i = 0; i < ud->count; i++, src += 3, dst += (int)LED_BYTES_PER_PIX) {
        led_encode_byte(dst,     src[0]); /* G */
        led_encode_byte(dst + 3, src[1]); /* R */
        led_encode_byte(dst + 6, src[2]); /* B */
    }
}

/* HSV to RGB.  h: 0..359, s: 0..255, v: 0..255.  Integer arithmetic only. */
static void led_hsv_to_rgb(u32 h, u8 s, u8 v, u8 *r_out, u8 *g_out, u8 *b_out)
{
    u32 region, rem, p, q, t;
    if (s == 0) {
        *r_out = *g_out = *b_out = v;
        return;
    }
    h %= 360u;
    region = h / 60u;
    rem    = ((h % 60u) * 255u) / 60u;
    p = ((u32)v * (255u - (u32)s)) / 255u;
    q = ((u32)v * (255u - (u32)s * rem / 255u)) / 255u;
    t = ((u32)v * (255u - (u32)s * (255u - rem) / 255u)) / 255u;
    switch (region) {
    case 0:  *r_out = v;     *g_out = (u8)t; *b_out = (u8)p; break;
    case 1:  *r_out = (u8)q; *g_out = v;     *b_out = (u8)p; break;
    case 2:  *r_out = (u8)p; *g_out = v;     *b_out = (u8)t; break;
    case 3:  *r_out = (u8)p; *g_out = (u8)q; *b_out = v;     break;
    case 4:  *r_out = (u8)t; *g_out = (u8)p; *b_out = v;     break;
    default: *r_out = v;     *g_out = (u8)p; *b_out = (u8)q; break;
    }
}

/* ---- DMA ISR ------------------------------------------------------------- */

static u32 led_dma_tx_irq(void *data)
{
    led_ctrl_t       *ctrl = (led_ctrl_t *)data;
    PGDMA_InitTypeDef gd   = &ctrl->gdma_tx;

    GDMA_ClearINT(gd->GDMA_Index, gd->GDMA_ChNum);
    GDMA_Cmd(gd->GDMA_Index, gd->GDMA_ChNum, DISABLE);
    GDMA_ChnlFree(gd->GDMA_Index, gd->GDMA_ChNum);
    SSI_SetDmaEnable(ctrl->spi_dev, DISABLE, SPI_BIT_TDMAE);

    rtos_sema_give(ctrl->sema);
    return 0;
}

/* ---- Userdata check ------------------------------------------------------ */

static led_ud_t *led_check(lua_State *L, int arg)
{
    led_ud_t *ud = (led_ud_t *)luaL_checkudata(L, arg, LED_MT);
    if (ud->closed) {
        luaL_error(L, "led_strip: handle is closed");
    }
    return ud;
}

/* ---- led_strip.new({spi=1, mosi="PB_8", count=15 [, pinmux="full"]}) ----- */

static int led_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    /* ---- Parse parameters (no resources held; luaL_error safe) ----------- */

    lua_getfield(L, 1, "spi");
    int ctrl_idx = lua_isnil(L, -1) ? 1 : (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    if (ctrl_idx < 0 || ctrl_idx >= LED_NUM_CTRL) {
        return luaL_error(L, "led_strip.new: spi must be 0 or 1, got %d", ctrl_idx);
    }

    lua_getfield(L, 1, "mosi");
    if (lua_isnil(L, -1)) {
        return luaL_error(L, "led_strip.new: 'mosi' is required");
    }
    u16 mosi = (u16)(int)luhw_check_pin(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "count");
    if (lua_isnil(L, -1)) {
        return luaL_error(L, "led_strip.new: 'count' is required");
    }
    int count = (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    if (count < 1) {
        return luaL_error(L, "led_strip.new: count must be >= 1, got %d", count);
    }

    lua_getfield(L, 1, "pinmux");
    const char *pmux = lua_isnil(L, -1) ? "dedicated" : luaL_checkstring(L, -1);
    u8 full_pinmux = (strcmp(pmux, "full") == 0) ? 1u : 0u;
    lua_pop(L, 1);

    /* ---- Allocate userdata FIRST (lua_newuserdata may longjmp; no lock) -- */

    led_ud_t *ud = (led_ud_t *)lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    luaL_getmetatable(L, LED_MT);
    lua_setmetatable(L, -2);

    /* ---- Allocate per-handle buffers ------------------------------------- */

    ud->grb = (u8 *)rtos_mem_zmalloc((u32)count * 3u);
    if (!ud->grb) {
        return luaL_error(L, "led_strip.new: out of memory (grb)");
    }

    u32 dma_sz  = led_dma_size(count);
    u8 *dma_raw = (u8 *)rtos_mem_zmalloc(dma_sz + (u32)CACHE_LINE_SIZE - 1u);
    if (!dma_raw) {
        rtos_mem_free(ud->grb);
        ud->grb = NULL;
        return luaL_error(L, "led_strip.new: out of memory (dma)");
    }
    ud->dma_raw  = dma_raw;
    ud->dma_buf  = (u8 *)(((u32)dma_raw + (u32)CACHE_LINE_SIZE - 1u) &
                           ~((u32)CACHE_LINE_SIZE - 1u));
    ud->dma_size = dma_sz;
    ud->count    = count;
    ud->idx      = ctrl_idx;

    /* ---- Acquire controller lock ----------------------------------------- */

    led_ctrl_t *ctrl = &s_ctrl[ctrl_idx];
    if (!ctrl->lock || !ctrl->sema) {
        /* Should have been initialised in luaopen_led_strip; guard anyway. */
        rtos_mem_free(ud->dma_raw); ud->dma_raw = NULL;
        rtos_mem_free(ud->grb);     ud->grb     = NULL;
        return luaL_error(L, "led_strip.new: controller %d OS resources missing",
                          ctrl_idx);
    }
    if (rtos_mutex_take(ctrl->lock, LED_LOCK_MS) != RTK_SUCCESS) {
        return luaL_error(L, "led_strip.new: spi%d busy (lock timeout)", ctrl_idx);
    }

    /* Exclusive: only one handle per controller (one MOSI line). */
    if (ctrl->inited && ctrl->refcnt > 0) {
        rtos_mutex_give(ctrl->lock);
        return luaL_error(L, "led_strip.new: spi%d already in use", ctrl_idx);
    }

    /* ---- Initialise SPI hardware unconditionally ------------------------- */
    /* Always reinit: safe because exclusive access is enforced (refcnt == 0
     * was verified above), and avoids carrying stale state from a previous
     * handle that called close() without disabling the peripheral. */
    {
        SSI_InitTypeDef ssi;

        RCC_PeriphClockCmd(s_periph[ctrl_idx], s_clk[ctrl_idx], ENABLE);

        u32 mosi_mux = full_pinmux ? s_fp_mosi[ctrl_idx] : s_ded_mosi[ctrl_idx];
        Pinmux_Config((u8)mosi, mosi_mux);
        RTK_LOGI(TAG, "spi%d MOSI=0x%x pinmux=%s\n", ctrl_idx, (int)mosi,
                 full_pinmux ? "full-matrix" : "dedicated");

        ctrl->spi_dev = SPI_DEV_TABLE[ctrl_idx].SPIx;
        SSI_SetRole(ctrl->spi_dev, SSI_MASTER);
        SSI_StructInit(&ssi);
        ssi.SPI_Role          = SSI_MASTER;
        ssi.SPI_ClockDivider  = LED_CLK_DIV;
        SSI_Init(ctrl->spi_dev, &ssi);

        ctrl->inited = 1;
    }
    ctrl->refcnt++;
    ud->counted = 1;

    rtos_mutex_give(ctrl->lock);
    return 1; /* the handle userdata */
}

/* ---- set_pixel ----------------------------------------------------------- */

static int led_set_pixel(lua_State *L)
{
    led_ud_t   *ud = led_check(L, 1);
    lua_Integer  i = luaL_checkinteger(L, 2);
    u8 r = (u8)(luaL_checkinteger(L, 3) & 0xFF);
    u8 g = (u8)(luaL_checkinteger(L, 4) & 0xFF);
    u8 b = (u8)(luaL_checkinteger(L, 5) & 0xFF);

    if (i < 1 || i > ud->count) {
        return luaL_error(L, "led_strip.set_pixel: index %d out of 1..%d",
                          (int)i, ud->count);
    }
    int o = (int)(i - 1) * 3;
    ud->grb[o + 0] = g;
    ud->grb[o + 1] = r;
    ud->grb[o + 2] = b;
    return 0;
}

/* ---- set_pixel_hsv ------------------------------------------------------- */

static int led_set_pixel_hsv(lua_State *L)
{
    led_ud_t   *ud = led_check(L, 1);
    lua_Integer  i = luaL_checkinteger(L, 2);
    u32 h = (u32)luaL_checkinteger(L, 3);
    u8  s = (u8)(luaL_checkinteger(L, 4) & 0xFF);
    u8  v = (u8)(luaL_checkinteger(L, 5) & 0xFF);

    if (i < 1 || i > ud->count) {
        return luaL_error(L, "led_strip.set_pixel_hsv: index %d out of 1..%d",
                          (int)i, ud->count);
    }
    u8 r, g, b;
    led_hsv_to_rgb(h, s, v, &r, &g, &b);
    int o = (int)(i - 1) * 3;
    ud->grb[o + 0] = g;
    ud->grb[o + 1] = r;
    ud->grb[o + 2] = b;
    return 0;
}

/* ---- fill ---------------------------------------------------------------- */

static int led_fill(lua_State *L)
{
    led_ud_t *ud = led_check(L, 1);
    u8 r = (u8)(luaL_checkinteger(L, 2) & 0xFF);
    u8 g = (u8)(luaL_checkinteger(L, 3) & 0xFF);
    u8 b = (u8)(luaL_checkinteger(L, 4) & 0xFF);
    int k;
    for (k = 0; k < ud->count; k++) {
        ud->grb[k * 3 + 0] = g;
        ud->grb[k * 3 + 1] = r;
        ud->grb[k * 3 + 2] = b;
    }
    return 0;
}

/* ---- fill_hsv ------------------------------------------------------------- */

static int led_fill_hsv(lua_State *L)
{
    led_ud_t *ud = led_check(L, 1);
    u32 h = (u32)luaL_checkinteger(L, 2);
    u8  s = (u8)(luaL_checkinteger(L, 3) & 0xFF);
    u8  v = (u8)(luaL_checkinteger(L, 4) & 0xFF);
    u8  r, g, b;
    int k;
    led_hsv_to_rgb(h, s, v, &r, &g, &b);
    for (k = 0; k < ud->count; k++) {
        ud->grb[k * 3 + 0] = g;
        ud->grb[k * 3 + 1] = r;
        ud->grb[k * 3 + 2] = b;
    }
    return 0;
}

/* ---- clear --------------------------------------------------------------- */

static int led_clear(lua_State *L)
{
    led_ud_t *ud = led_check(L, 1);
    memset(ud->grb, 0, (size_t)ud->count * 3u);
    return 0;
}

/* ---- show() — encode pixel data then fire one DMA transfer --------------- */

static int led_show(lua_State *L)
{
    led_ud_t   *ud   = led_check(L, 1);
    led_ctrl_t *ctrl = &s_ctrl[ud->idx];

    if (rtos_mutex_take(ctrl->lock, LED_LOCK_MS) != RTK_SUCCESS) {
        return luaL_error(L, "led_strip.show: spi%d busy (lock timeout)", ud->idx);
    }

    led_encode_frame(ud);
    DCache_Clean((u32)ud->dma_buf, ud->dma_size);

    SSI_SetDmaEnable(ctrl->spi_dev, ENABLE, SPI_BIT_TDMAE);
    SSI_TXGDMA_Init((u32)ud->idx, &ctrl->gdma_tx, ctrl,
                    (IRQ_FUN)led_dma_tx_irq,
                    ud->dma_buf, ud->dma_size);

    int rc = rtos_sema_take(ctrl->sema, LED_DMA_MS);

    if (rc != RTK_SUCCESS) {
        /* DMA timed out — abort and clean up. */
        GDMA_Cmd(ctrl->gdma_tx.GDMA_Index, ctrl->gdma_tx.GDMA_ChNum, DISABLE);
        GDMA_ClearINT(ctrl->gdma_tx.GDMA_Index, ctrl->gdma_tx.GDMA_ChNum);
        GDMA_ChnlFree(ctrl->gdma_tx.GDMA_Index, ctrl->gdma_tx.GDMA_ChNum);
        SSI_SetDmaEnable(ctrl->spi_dev, DISABLE, SPI_BIT_TDMAE);
        rtos_mutex_give(ctrl->lock);
        return luaL_error(L, "led_strip.show: DMA timeout on spi%d", ud->idx);
    }

    rtos_mutex_give(ctrl->lock);
    return 0;
}

/* ---- close / __gc -------------------------------------------------------- */

static void led_release(led_ud_t *ud)
{
    if (ud->closed) {
        return;
    }
    ud->closed = 1;

    if (ud->counted) {
        led_ctrl_t *ctrl = &s_ctrl[ud->idx];
        rtos_mutex_take(ctrl->lock, RTOS_MAX_DELAY);
        ctrl->refcnt--;
        /* Do NOT call SSI_Cmd(DISABLE) or RCC_PeriphClockCmd(DISABLE) here:
         * the SPI TX FIFO may still be draining the trailing reset bytes, and
         * disabling the peripheral clock while it is active triggers an
         * imprecise bus fault in Secure ROM.  new() always reinitialises the
         * controller unconditionally, so stale hardware state is never reused. */
        if (ctrl->refcnt == 0) {
            ctrl->inited = 0;
        }
        rtos_mutex_give(ctrl->lock);
        ud->counted = 0;
    }

    if (ud->grb) {
        rtos_mem_free(ud->grb);
        ud->grb = NULL;
    }
    if (ud->dma_raw) {
        rtos_mem_free(ud->dma_raw);
        ud->dma_raw = NULL;
        ud->dma_buf = NULL;
    }
}

static int led_close(lua_State *L)
{
    led_ud_t *ud = (led_ud_t *)luaL_checkudata(L, 1, LED_MT);
    led_release(ud);
    return 0;
}

static int led_gc(lua_State *L)
{
    led_ud_t *ud = (led_ud_t *)luaL_testudata(L, 1, LED_MT);
    if (ud) {
        led_release(ud);
    }
    return 0;
}

/* ---- stop_requested() ---------------------------------------------------- */

static int led_stop_requested(lua_State *L)
{
    lua_pushboolean(L, led_strip_loop_stop);
    return 1;
}

/* ---- luaopen_led_strip ---------------------------------------------------- */

static const luaL_Reg led_methods[] = {
    { "set_pixel",     led_set_pixel     },
    { "set_pixel_hsv", led_set_pixel_hsv },
    { "fill",          led_fill          },
    { "fill_hsv",      led_fill_hsv      },
    { "clear",         led_clear         },
    { "show",          led_show          },
    { "close",         led_close         },
    { NULL, NULL }
};

LUAMOD_API int luaopen_led_strip(lua_State *L)
{
    int i;
    for (i = 0; i < LED_NUM_CTRL; i++) {
        if (!s_ctrl[i].lock) {
            rtos_mutex_create(&s_ctrl[i].lock);
        }
        if (!s_ctrl[i].sema) {
            rtos_sema_create_binary(&s_ctrl[i].sema);
        }
    }

    if (luaL_newmetatable(L, LED_MT)) {
        lua_pushcfunction(L, led_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        luaL_setfuncs(L, led_methods, 0);
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, led_new);
    lua_setfield(L, -2, "new");
    lua_pushcfunction(L, led_stop_requested);
    lua_setfield(L, -2, "stop_requested");
    return 1;
}

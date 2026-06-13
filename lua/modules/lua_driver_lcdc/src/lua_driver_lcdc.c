/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * lua_driver_lcdc.c — LCDC controller Lua driver for RTL8721F.
 *
 * Exposes the lcdc module to Lua:
 *
 * Common:
 *   lcdc.pinmux(cfg)         — configure LCD signal pins (call before init)
 *   lcdc.deinit()            — disable LCDC and release resources
 *   lcdc.enable(bool)        — enable / disable LCDC output
 *   lcdc.update()            — trigger shadow reload (DMA address update)
 *   lcdc.set_framebuf(addr)  — update DMA base address and reload
 *   lcdc.get_framebuf_addr() — read current DMA base address
 *   lcdc.get_cur_pos()       — current scan position (x, y)
 *   lcdc.get_int_status()    — raw interrupt status bits
 *   lcdc.clear_int(mask)     — clear interrupt bits
 *   lcdc.set_underflow_mode(mode[,errdata]) — DMA underflow output mode
 *   lcdc.set_line_int_pos(line)    — set line interrupt trigger position
 *   lcdc.int_config(mask, enable)  — enable/disable interrupt sources
 *   lcdc.get_info()          — {initialized, fb_addr, width, height, INT_*}
 *
 * RGB / SRGB interface:
 *   lcdc.rgb_init(cfg)            — initialize RGB or SRGB interface
 *   lcdc.rgb_get_sync_status()    — {hs, vs} sync status
 *
 * MCU (8080 parallel) interface:
 *   lcdc.mcu_init(cfg)            — initialize MCU interface
 *   lcdc.mcu_io_write_cmd(cmd)    — IO-mode: send command byte
 *   lcdc.mcu_io_write_data(data)  — IO-mode: send data byte
 *   lcdc.mcu_io_read()            — IO-mode: read one byte
 *   lcdc.mcu_dma_start(addr, trigger_mode, burst) — start DMA transfer
 *   lcdc.mcu_dma_trigger()        — push one frame (manual trigger mode)
 *   lcdc.mcu_set_pre_cmd(cmd)     — set command sent before each DMA frame
 *   lcdc.mcu_reset_pre_cmd()      — clear the pre-frame command
 *   lcdc.mcu_get_run_status()     — DMA run status bitmask
 *
 * Note: fill_color(r,g,b), fill_rect(x,y,w,h,r,g,b), and set_pixel(x,y,r,g,b)
 * are NOT members of this module. They are plain Lua globals injected by
 * lua_lcdc_test_provision.c via lua_setglobal() before each test script runs.
 *
 * Pin muxing and board-specific GPIO (backlight, display-on, reset) are the
 * caller's responsibility; use lcdc.pinmux() for LCDC signal pins and
 * the gpio module for any auxiliary GPIOs.
 */

#include "lua_driver_lcdc.h"

#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include "ameba_soc.h"
#include "lauxlib.h"

/* ---- Module state ---- */
static int      s_initialized = 0;
static uint32_t s_fb_addr     = 0;
static uint32_t s_fb_width    = 0;
static uint32_t s_fb_height   = 0;
static int      s_mcu_mode    = 0;   /* 0=RGB, 1=MCU (8080 parallel) */
static int      s_fb_bpp      = 2;   /* bytes/pixel: 2=RGB565, 3=RGB888, 4=ARGB */

/* Return bytes-per-pixel for a given LCDC input format constant. */
static int fmt_to_bpp(uint32_t fmt)
{
    if (fmt == LCDC_INPUT_FORMAT_RGB888 || fmt == LCDC_INPUT_FORMAT_BGR888) {
        return 3;
    }
    if (fmt == LCDC_INPUT_FORMAT_ARGB8888 || fmt == LCDC_INPUT_FORMAT_ABGR8888) {
        return 4;
    }
    return 2;
}

/* ---- IRQ handler: clear all pending interrupts ---- */
static void lcdc_irq_handler(void)
{
    uint32_t ints = LCDC_GetINTStatus(LCDC);
    LCDC_ClearINT(LCDC, ints);
}

/* ---- Table field helpers ---- */

static uint32_t table_get_uint(lua_State *L, int table_idx,
                                const char *key, uint32_t def)
{
    uint32_t val = def;
    lua_getfield(L, table_idx, key);
    if (!lua_isnil(L, -1)) {
        val = (uint32_t)luaL_checkinteger(L, -1);
    }
    lua_pop(L, 1);
    return val;
}

static uint32_t table_require_uint(lua_State *L, int table_idx, const char *key)
{
    lua_getfield(L, table_idx, key);
    if (lua_isnil(L, -1)) {
        luaL_error(L, "lcdc: required field '%s' is missing", key);
    }
    uint32_t val = (uint32_t)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    return val;
}

static const char *table_get_str(lua_State *L, int table_idx,
                                  const char *key, const char *def)
{
    const char *val = def;
    lua_getfield(L, table_idx, key);
    if (!lua_isnil(L, -1)) {
        val = luaL_checkstring(L, -1);
    }
    lua_pop(L, 1);
    return val;
}

/* ---- Lua API ---- */

/* lcdc.pinmux(cfg) → true
 *
 * Configure LCDC signal pins. All fields are optional; only provided pins
 * are configured.
 *
 * cfg fields (pin numbers, e.g. 0x2F for PB_15):
 *   d0..d23  — RGB data bus bits 0–23 (PINMUX_FUNCTION_LCD_D0..D23)
 *   hsync    — horizontal sync     (PINMUX_FUNCTION_LCD_RGB_HSYNC)
 *   vsync    — vertical sync       (PINMUX_FUNCTION_LCD_RGB_VSYNC)
 *   dclk     — pixel clock         (PINMUX_FUNCTION_LCD_RGB_DCLK)
 *   de       — data enable         (PINMUX_FUNCTION_LCD_RGB_DE)
 *
 * Board-specific GPIOs (backlight, display-on) are not part of the LCDC
 * pinmux; configure them separately via the gpio module.
 */
static int lua_lcdc_pinmux(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    /* Data lines D0..D23; PINMUX_FUNCTION_LCD_D0 = 45, D1 = 46, ..., D23 = 68 */
    for (int i = 0; i <= 23; i++) {
        char field[5];
        snprintf(field, sizeof(field), "d%d", i);
        lua_getfield(L, 1, field);
        if (!lua_isnil(L, -1)) {
            uint32_t pin = (uint32_t)luaL_checkinteger(L, -1);
            Pinmux_Config(pin, PINMUX_FUNCTION_LCD_D0 + (uint32_t)i);
        }
        lua_pop(L, 1);
    }

    /* Sync/control signals — RGB and MCU share the same pinmux call */
    static const struct { const char *name; uint32_t func; } signals[] = {
        /* RGB signals */
        { "hsync", PINMUX_FUNCTION_LCD_RGB_HSYNC },
        { "vsync", PINMUX_FUNCTION_LCD_RGB_VSYNC },
        { "dclk",  PINMUX_FUNCTION_LCD_RGB_DCLK  },
        { "de",    PINMUX_FUNCTION_LCD_RGB_DE     },
        /* MCU (8080) control signals */
        { "wr",    PINMUX_FUNCTION_LCD_MCU_WR    },
        { "cs",    PINMUX_FUNCTION_LCD_MCU_CSX   },
        { "rd",    PINMUX_FUNCTION_LCD_MCU_RD    },
        { "te",    PINMUX_FUNCTION_LCD_MCU_TE    },
        { "dcx",   PINMUX_FUNCTION_LCD_MCU_DCX   },
    };
    for (int i = 0; i < (int)(sizeof(signals) / sizeof(signals[0])); i++) {
        lua_getfield(L, 1, signals[i].name);
        if (!lua_isnil(L, -1)) {
            uint32_t pin = (uint32_t)luaL_checkinteger(L, -1);
            Pinmux_Config(pin, signals[i].func);
        }
        lua_pop(L, 1);
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* lcdc.rgb_init(cfg) → true
 *
 * Initialise LCDC in RGB parallel mode (continuous DMA refresh).
 *
 * Required fields (no defaults — panel/screen specific):
 *   fb_addr       — framebuffer base address (allocated by the caller)
 *   width         — panel width  (pixels)
 *   height        — panel height (pixels)
 *   vsw           — vertical sync pulse width  (lines)
 *   vbp           — vertical back porch        (lines)
 *   vfp           — vertical front porch       (lines)
 *   hsw           — horizontal sync width      (DCLK)
 *   hbp           — horizontal back porch      (DCLK)
 *   hfp           — horizontal front porch     (DCLK)
 *   refresh_freq  — frame refresh rate         (Hz)
 *
 * Optional fields (sensible hardware defaults in parentheses):
 *   input_fmt   ("rgb565")   — fb pixel format: rgb565/bgr565/rgb888/bgr888/
 *                              argb8888/abgr8888/argb1555/argb4444/rgb666
 *   output_fmt  ("bgr888")   — panel output:    rgb888/bgr888/rgb565/bgr565/
 *                              rgb666/bgr666
 *   if_width    ("24bit")    — RGB bus width:   "6bit","8bit","16bit","18bit","24bit"
 *   en_pol      (1)          — DE polarity:     1=high-active, 0=low-active
 *   hs_pol      (0)          — HSYNC polarity:  0=low-sync, 1=high-sync
 *   vs_pol      (0)          — VSYNC polarity:  0=low-sync, 1=high-sync
 *   dclk_edge   (1)          — DCLK active edge:1=falling, 0=rising
 *   dma_burst   (1)          — DMA burst size:  0=64B, 1=128B(2×64), 2=256B(4×64)
 */
static int lua_lcdc_rgb_init(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    uint32_t fb_addr = table_require_uint(L, 1, "fb_addr");
    uint32_t width   = table_require_uint(L, 1, "width");
    uint32_t height  = table_require_uint(L, 1, "height");
    uint32_t vsw    = table_require_uint(L, 1, "vsw");
    uint32_t vbp    = table_require_uint(L, 1, "vbp");
    uint32_t vfp    = table_require_uint(L, 1, "vfp");
    uint32_t hsw    = table_require_uint(L, 1, "hsw");
    uint32_t hbp    = table_require_uint(L, 1, "hbp");
    uint32_t hfp    = table_require_uint(L, 1, "hfp");
    uint32_t freq   = table_require_uint(L, 1, "refresh_freq");

    const char *in_fmt_str   = table_get_str(L, 1, "input_fmt",  "rgb565");
    const char *out_fmt_str  = table_get_str(L, 1, "output_fmt", "bgr888");
    const char *if_width_str = table_get_str(L, 1, "if_width",   "24bit");

    uint32_t en_pol    = table_get_uint(L, 1, "en_pol",    LCDC_RGB_EN_PUL_HIGH_LEV_ACTIVE);
    uint32_t hs_pol    = table_get_uint(L, 1, "hs_pol",    LCDC_RGB_HS_PUL_LOW_LEV_SYNC);
    uint32_t vs_pol    = table_get_uint(L, 1, "vs_pol",    LCDC_RGB_VS_PUL_LOW_LEV_SYNC);
    uint32_t dclk_edge = table_get_uint(L, 1, "dclk_edge", LCDC_RGB_DCLK_FALLING_EDGE_FETCH);
    uint32_t dma_burst = table_get_uint(L, 1, "dma_burst", LCDC_DMA_BURSTSIZE_2X64BYTES);

    if (width == 0 || height == 0) {
        return luaL_error(L, "lcdc: width and height must be positive");
    }
    if (dma_burst > 2) {
        return luaL_error(L, "lcdc: dma_burst must be 0, 1, or 2");
    }

    /* Map input format string → constant */
    uint32_t input_fmt;
    if (strcmp(in_fmt_str, "rgb565") == 0) {
        input_fmt = LCDC_INPUT_FORMAT_RGB565;
    } else if (strcmp(in_fmt_str, "bgr565") == 0) {
        input_fmt = LCDC_INPUT_FORMAT_BGR565;
    } else if (strcmp(in_fmt_str, "rgb888") == 0) {
        input_fmt = LCDC_INPUT_FORMAT_RGB888;
    } else if (strcmp(in_fmt_str, "bgr888") == 0) {
        input_fmt = LCDC_INPUT_FORMAT_BGR888;
    } else if (strcmp(in_fmt_str, "argb8888") == 0) {
        input_fmt = LCDC_INPUT_FORMAT_ARGB8888;
    } else if (strcmp(in_fmt_str, "abgr8888") == 0) {
        input_fmt = LCDC_INPUT_FORMAT_ABGR8888;
    } else if (strcmp(in_fmt_str, "argb1555") == 0) {
        input_fmt = LCDC_INPUT_FORMAT_ARGB1555;
    } else if (strcmp(in_fmt_str, "argb4444") == 0) {
        input_fmt = LCDC_INPUT_FORMAT_ARGB4444;
    } else if (strcmp(in_fmt_str, "rgb666") == 0) {
        input_fmt = LCDC_INPUT_FORMAT_RGB666;
    } else {
        return luaL_error(L, "lcdc: unsupported input_fmt '%s'", in_fmt_str);
    }

    /* Map output format string → constant */
    uint32_t output_fmt;
    if (strcmp(out_fmt_str, "rgb888") == 0) {
        output_fmt = LCDC_OUTPUT_FORMAT_RGB888;
    } else if (strcmp(out_fmt_str, "bgr888") == 0) {
        output_fmt = LCDC_OUTPUT_FORMAT_BGR888;
    } else if (strcmp(out_fmt_str, "rgb565") == 0) {
        output_fmt = LCDC_OUTPUT_FORMAT_RGB565;
    } else if (strcmp(out_fmt_str, "bgr565") == 0) {
        output_fmt = LCDC_OUTPUT_FORMAT_BGR565;
    } else if (strcmp(out_fmt_str, "rgb666") == 0) {
        output_fmt = LCDC_OUTPUT_FORMAT_RGB666;
    } else if (strcmp(out_fmt_str, "bgr666") == 0) {
        output_fmt = LCDC_OUTPUT_FORMAT_BGR666;
    } else {
        return luaL_error(L, "lcdc: unsupported output_fmt '%s'", out_fmt_str);
    }

    /* Map if_width string → constant */
    uint32_t if_width;
    if (strcmp(if_width_str, "24bit") == 0 || strcmp(if_width_str, "24") == 0) {
        if_width = LCDC_RGB_IF_24_BIT;
    } else if (strcmp(if_width_str, "18bit") == 0 || strcmp(if_width_str, "18") == 0) {
        if_width = LCDC_RGB_IF_18_BIT;
    } else if (strcmp(if_width_str, "16bit") == 0 || strcmp(if_width_str, "16") == 0) {
        if_width = LCDC_RGB_IF_16_BIT;
    } else if (strcmp(if_width_str, "8bit") == 0 || strcmp(if_width_str, "8") == 0) {
        if_width = LCDC_RGB_IF_8_BIT;
    } else if (strcmp(if_width_str, "6bit") == 0 || strcmp(if_width_str, "6") == 0) {
        if_width = LCDC_RGB_IF_6_BIT;
    } else {
        return luaL_error(L, "lcdc: unsupported if_width '%s' "
                          "(use '6bit','8bit','16bit','18bit','24bit')", if_width_str);
    }

    /* Disable if already running */
    if (s_initialized) {
        LCDC_Cmd(LCDC, DISABLE);
        LCDC_INTConfig(LCDC,
            LCDC_BIT_LCD_FRD_INTEN | LCDC_BIT_DMA_UN_INTEN |
            LCDC_BIT_LCD_LIN_INTEN | LCDC_BIT_FRM_START_INTEN, DISABLE);
        InterruptDis(LCDC_IRQ);
        LCDC_DeInit(LCDC);
        s_initialized = 0;
    }

    /* Clock */
    LCDC_RccEnable();

    /* IRQ */
    InterruptRegister((IRQ_FUN)lcdc_irq_handler, LCDC_IRQ, (u32)LCDC, INT_PRI_MIDDLE);
    InterruptEn(LCDC_IRQ, INT_PRI_MIDDLE);

    /* LCDC RGB init */
    LCDC_RGBInitTypeDef rgb;
    LCDC_Cmd(LCDC, DISABLE);
    LCDC_RGBStructInit(&rgb);

    rgb.Panel_RgbTiming.RgbVsw = vsw;
    rgb.Panel_RgbTiming.RgbVbp = vbp;
    rgb.Panel_RgbTiming.RgbVfp = vfp;
    rgb.Panel_RgbTiming.RgbHsw = hsw;
    rgb.Panel_RgbTiming.RgbHbp = hbp;
    rgb.Panel_RgbTiming.RgbHfp = hfp;

    rgb.Panel_Init.IfWidth        = if_width;
    rgb.Panel_Init.ImgWidth       = width;
    rgb.Panel_Init.ImgHeight      = height;
    rgb.Panel_Init.InputFormat    = input_fmt;
    rgb.Panel_Init.OutputFormat   = output_fmt;
    rgb.Panel_Init.RGBRefreshFreq = freq;

    rgb.Panel_RgbTiming.Flags.RgbEnPolar      = (u8)en_pol;
    rgb.Panel_RgbTiming.Flags.RgbDclkActvEdge = (u8)dclk_edge;
    rgb.Panel_RgbTiming.Flags.RgbHsPolar      = (u8)hs_pol;
    rgb.Panel_RgbTiming.Flags.RgbVsPolar      = (u8)vs_pol;

    LCDC_RGBInit(LCDC, &rgb);

    /* DMA config */
    LCDC_DMABurstSizeConfig(LCDC, dma_burst);
    LCDC_DMAImgCfg(LCDC, fb_addr);

    /* Enable frame-done, line, and DMA-underflow interrupts */
    LCDC_LineINTPosConfig(LCDC, height / 2);
    LCDC_INTConfig(LCDC,
        LCDC_BIT_LCD_FRD_INTEN | LCDC_BIT_DMA_UN_INTEN | LCDC_BIT_LCD_LIN_INTEN, ENABLE);

    LCDC_Cmd(LCDC, ENABLE);

    s_fb_addr   = fb_addr;
    s_fb_width  = width;
    s_fb_height = height;
    s_fb_bpp    = fmt_to_bpp(input_fmt);
    s_initialized = 1;
    s_mcu_mode    = 0;

    lua_pushboolean(L, 1);
    return 1;
}

/* lcdc.deinit() → true */
static int lua_lcdc_deinit(lua_State *L)
{
    if (s_initialized) {
        LCDC_Cmd(LCDC, DISABLE);
        LCDC_INTConfig(LCDC,
            LCDC_BIT_LCD_FRD_INTEN | LCDC_BIT_DMA_UN_INTEN |
            LCDC_BIT_LCD_LIN_INTEN | LCDC_BIT_FRM_START_INTEN, DISABLE);
        InterruptDis(LCDC_IRQ);
        LCDC_DeInit(LCDC);
        s_initialized = 0;
        s_mcu_mode  = 0;
        s_fb_bpp    = 2;
        s_fb_addr   = 0;
        s_fb_width  = 0;
        s_fb_height = 0;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* lcdc.enable(bool) → true */
static int lua_lcdc_enable(lua_State *L)
{
    if (!s_initialized) {
        return luaL_error(L, "lcdc: not initialized");
    }
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    int en = lua_toboolean(L, 1);
    LCDC_Cmd(LCDC, en ? ENABLE : DISABLE);
    lua_pushboolean(L, 1);
    return 1;
}


/* lcdc.update() → true  (trigger shadow reload) */
static int lua_lcdc_update(lua_State *L)
{
    if (!s_initialized) {
        return luaL_error(L, "lcdc: not initialized");
    }
    LCDC_ShadowReloadConfig(LCDC);
    lua_pushboolean(L, 1);
    return 1;
}

/* lcdc.set_framebuf(addr) → true */
static int lua_lcdc_set_framebuf(lua_State *L)
{
    if (!s_initialized) {
        return luaL_error(L, "lcdc: not initialized");
    }
    uint32_t addr = (uint32_t)luaL_checkinteger(L, 1);
    LCDC_DMAImgCfg(LCDC, addr);
    LCDC_ShadowReloadConfig(LCDC);
    lua_pushboolean(L, 1);
    return 1;
}

/* lcdc.get_framebuf_addr() → addr */
static int lua_lcdc_get_framebuf_addr(lua_State *L)
{
    if (!s_initialized) {
        return luaL_error(L, "lcdc: not initialized");
    }
    uint32_t img_a = 0, img_b = 0;
    LCDC_GetImgAddr(LCDC, &img_a, &img_b);
    lua_pushinteger(L, (lua_Integer)img_a);
    return 1;
}

/* lcdc.get_cur_pos() → x, y */
static int lua_lcdc_get_cur_pos(lua_State *L)
{
    if (!s_initialized) {
        return luaL_error(L, "lcdc: not initialized");
    }
    uint32_t px = 0, py = 0;
    LCDC_GetCurPosStatus(LCDC, &px, &py);
    lua_pushinteger(L, (lua_Integer)px);
    lua_pushinteger(L, (lua_Integer)py);
    return 2;
}

/* lcdc.get_int_status() → bitmask */
static int lua_lcdc_get_int_status(lua_State *L)
{
    if (!s_initialized) {
        return luaL_error(L, "lcdc: not initialized");
    }
    lua_pushinteger(L, (lua_Integer)LCDC_GetINTStatus(LCDC));
    return 1;
}

/* lcdc.clear_int(mask) → true */
static int lua_lcdc_clear_int(lua_State *L)
{
    if (!s_initialized) {
        return luaL_error(L, "lcdc: not initialized");
    }
    uint32_t mask = (uint32_t)luaL_checkinteger(L, 1);
    LCDC_ClearINT(LCDC, mask);
    lua_pushboolean(L, 1);
    return 1;
}

/* lcdc.rgb_get_sync_status() → {hs=n, vs=n} */
static int lua_lcdc_rgb_get_sync_status(lua_State *L)
{
    if (!s_initialized) {
        return luaL_error(L, "lcdc: not initialized");
    }
    uint32_t hs = 0, vs = 0;
    LCDC_RGBGetSyncStatus(LCDC, &hs, &vs);
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)hs);
    lua_setfield(L, -2, "hs");
    lua_pushinteger(L, (lua_Integer)vs);
    lua_setfield(L, -2, "vs");
    return 1;
}

/* lcdc.set_underflow_mode(mode [, errdata]) → true */
static int lua_lcdc_set_underflow_mode(lua_State *L)
{
    if (!s_initialized) {
        return luaL_error(L, "lcdc: not initialized");
    }
    uint32_t mode     = (uint32_t)luaL_checkinteger(L, 1);
    uint32_t err_data = (uint32_t)luaL_optinteger(L, 2, 0);
    LCDC_DMAUnderFlowOutdata(LCDC, mode, err_data);
    lua_pushboolean(L, 1);
    return 1;
}

/* lcdc.set_line_int_pos(line) → true */
static int lua_lcdc_set_line_int_pos(lua_State *L)
{
    if (!s_initialized) {
        return luaL_error(L, "lcdc: not initialized");
    }
    uint32_t line = (uint32_t)luaL_checkinteger(L, 1);
    LCDC_LineINTPosConfig(LCDC, line);
    lua_pushboolean(L, 1);
    return 1;
}

/* lcdc.int_config(mask, enable) → true */
static int lua_lcdc_int_config(lua_State *L)
{
    if (!s_initialized) {
        return luaL_error(L, "lcdc: not initialized");
    }
    uint32_t mask = (uint32_t)luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    int en = lua_toboolean(L, 2);
    LCDC_INTConfig(LCDC, mask, en ? ENABLE : DISABLE);
    lua_pushboolean(L, 1);
    return 1;
}

/* lcdc.get_info() → table */
static int lua_lcdc_get_info(lua_State *L)
{
    lua_newtable(L);

    lua_pushboolean(L, s_initialized);
    lua_setfield(L, -2, "initialized");

    lua_pushinteger(L, (lua_Integer)s_fb_addr);
    lua_setfield(L, -2, "fb_addr");

    lua_pushinteger(L, (lua_Integer)s_fb_width);
    lua_setfield(L, -2, "width");

    lua_pushinteger(L, (lua_Integer)s_fb_height);
    lua_setfield(L, -2, "height");

    /* Expose interrupt bit constants for convenience */
    lua_pushinteger(L, (lua_Integer)LCDC_BIT_LCD_FRD_INTEN);
    lua_setfield(L, -2, "INT_FRD");

    lua_pushinteger(L, (lua_Integer)LCDC_BIT_LCD_LIN_INTEN);
    lua_setfield(L, -2, "INT_LINE");

    lua_pushinteger(L, (lua_Integer)LCDC_BIT_DMA_UN_INTEN);
    lua_setfield(L, -2, "INT_DMA_UDF");

    lua_pushinteger(L, (lua_Integer)LCDC_BIT_FRM_START_INTEN);
    lua_setfield(L, -2, "INT_FRM_START");

    return 1;
}

/* ====================================================================
 * MCU (8080 parallel) interface APIs
 * ==================================================================== */

/* lcdc.mcu_init(cfg) → true
 *
 * Initialise LCDC in MCU (8080 parallel) mode and enter IO mode ready
 * for panel init via mcu_io_write_cmd / mcu_io_write_data.
 *
 * Required: width, height
 * Optional (panel/board specific):
 *   if_width    ("24bit")  — "8bit","9bit","16bit","18bit","24bit"
 *   input_fmt   ("rgb888") — framebuffer pixel format
 *   output_fmt  ("rgb888") — panel output format
 *   wrpulw      (1)        — WR pulse width (write-clock divider value)
 *   rdactw      (0)        — RD active pulse width (sys clocks)
 *   rdinactw    (0)        — RD inactive pulse width (sys clocks)
 *   wr_pol      (0)        — 0=rising-edge, 1=falling-edge
 *   rd_pol      (0)        — 0=rising-edge, 1=falling-edge
 *   rs_pol      (0)        — 0=low=command, 1=high=command
 *   dma_burst   (2)        — DMA burst: 0=64B, 1=128B, 2=256B
 */
static int lua_lcdc_mcu_init(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    uint32_t width   = table_require_uint(L, 1, "width");
    uint32_t height  = table_require_uint(L, 1, "height");

    const char *if_width_str = table_get_str(L, 1, "if_width",   "24bit");
    const char *in_fmt_str   = table_get_str(L, 1, "input_fmt",  "rgb888");
    const char *out_fmt_str  = table_get_str(L, 1, "output_fmt", "rgb888");

    uint32_t wrpulw    = table_get_uint(L, 1, "wrpulw",    1);
    uint32_t rdactw    = table_get_uint(L, 1, "rdactw",    0);
    uint32_t rdinactw  = table_get_uint(L, 1, "rdinactw",  0);
    uint32_t wr_pol    = table_get_uint(L, 1, "wr_pol",    LCDC_MCU_WR_PUL_RISING_EDGE_FETCH);
    uint32_t rd_pol    = table_get_uint(L, 1, "rd_pol",    LCDC_MCU_RD_PUL_RISING_EDGE_FETCH);
    uint32_t rs_pol    = table_get_uint(L, 1, "rs_pol",    LCDC_MCU_RS_PUL_LOW_LEV_CMD_ADDR);
    uint32_t dma_burst = table_get_uint(L, 1, "dma_burst", 2);

    if (width == 0 || height == 0) {
        return luaL_error(L, "lcdc: width and height must be positive");
    }
    if (dma_burst > 2) {
        return luaL_error(L, "lcdc: dma_burst must be 0, 1, or 2");
    }

    uint32_t if_width;
    if (strcmp(if_width_str, "24bit") == 0 || strcmp(if_width_str, "24") == 0) {
        if_width = LCDC_MCU_IF_24_BIT;
    } else if (strcmp(if_width_str, "16bit") == 0 || strcmp(if_width_str, "16") == 0) {
        if_width = LCDC_MCU_IF_16_BIT;
    } else if (strcmp(if_width_str, "18bit") == 0 || strcmp(if_width_str, "18") == 0) {
        if_width = LCDC_MCU_IF_18_BIT;
    } else if (strcmp(if_width_str, "8bit") == 0 || strcmp(if_width_str, "8") == 0) {
        if_width = LCDC_MCU_IF_8_BIT;
    } else if (strcmp(if_width_str, "9bit") == 0 || strcmp(if_width_str, "9") == 0) {
        if_width = LCDC_MCU_IF_9_BIT;
    } else {
        return luaL_error(L, "lcdc: unsupported mcu if_width '%s' "
                          "(use '8bit','9bit','16bit','18bit','24bit')", if_width_str);
    }

    uint32_t input_fmt;
    if (strcmp(in_fmt_str, "rgb888") == 0) {
        input_fmt = LCDC_INPUT_FORMAT_RGB888;
    } else if (strcmp(in_fmt_str, "bgr888") == 0) {
        input_fmt = LCDC_INPUT_FORMAT_BGR888;
    } else if (strcmp(in_fmt_str, "rgb565") == 0) {
        input_fmt = LCDC_INPUT_FORMAT_RGB565;
    } else if (strcmp(in_fmt_str, "bgr565") == 0) {
        input_fmt = LCDC_INPUT_FORMAT_BGR565;
    } else if (strcmp(in_fmt_str, "argb8888") == 0) {
        input_fmt = LCDC_INPUT_FORMAT_ARGB8888;
    } else if (strcmp(in_fmt_str, "abgr8888") == 0) {
        input_fmt = LCDC_INPUT_FORMAT_ABGR8888;
    } else if (strcmp(in_fmt_str, "argb1555") == 0) {
        input_fmt = LCDC_INPUT_FORMAT_ARGB1555;
    } else if (strcmp(in_fmt_str, "argb4444") == 0) {
        input_fmt = LCDC_INPUT_FORMAT_ARGB4444;
    } else if (strcmp(in_fmt_str, "rgb666") == 0) {
        input_fmt = LCDC_INPUT_FORMAT_RGB666;
    } else {
        return luaL_error(L, "lcdc: unsupported input_fmt '%s'", in_fmt_str);
    }

    uint32_t output_fmt;
    if (strcmp(out_fmt_str, "rgb888") == 0) {
        output_fmt = LCDC_OUTPUT_FORMAT_RGB888;
    } else if (strcmp(out_fmt_str, "bgr888") == 0) {
        output_fmt = LCDC_OUTPUT_FORMAT_BGR888;
    } else if (strcmp(out_fmt_str, "rgb565") == 0) {
        output_fmt = LCDC_OUTPUT_FORMAT_RGB565;
    } else if (strcmp(out_fmt_str, "bgr565") == 0) {
        output_fmt = LCDC_OUTPUT_FORMAT_BGR565;
    } else if (strcmp(out_fmt_str, "rgb666") == 0) {
        output_fmt = LCDC_OUTPUT_FORMAT_RGB666;
    } else if (strcmp(out_fmt_str, "bgr666") == 0) {
        output_fmt = LCDC_OUTPUT_FORMAT_BGR666;
    } else {
        return luaL_error(L, "lcdc: unsupported output_fmt '%s'", out_fmt_str);
    }

    if (s_initialized) {
        LCDC_Cmd(LCDC, DISABLE);
        LCDC_INTConfig(LCDC,
            LCDC_BIT_LCD_FRD_INTEN | LCDC_BIT_DMA_UN_INTEN |
            LCDC_BIT_LCD_LIN_INTEN | LCDC_BIT_FRM_START_INTEN, DISABLE);
        InterruptDis(LCDC_IRQ);
        LCDC_DeInit(LCDC);
        s_initialized = 0;
        s_mcu_mode = 0;
    }

    LCDC_RccEnable();

    InterruptRegister((IRQ_FUN)lcdc_irq_handler, LCDC_IRQ, (u32)LCDC, INT_PRI_MIDDLE);
    InterruptEn(LCDC_IRQ, INT_PRI_MIDDLE);

    LCDC_MCUInitTypeDef mcu;
    LCDC_MCUStructInit(&mcu);
    mcu.Panel_Init.IfWidth      = if_width;
    mcu.Panel_Init.ImgWidth     = width;
    mcu.Panel_Init.ImgHeight    = height;
    mcu.Panel_Init.InputFormat  = input_fmt;
    mcu.Panel_Init.OutputFormat = output_fmt;
    mcu.Panel_McuTiming.McuWrPolar = (u8)wr_pol;
    mcu.Panel_McuTiming.McuRdPolar = (u8)rd_pol;
    mcu.Panel_McuTiming.McuRsPolar = (u8)rs_pol;
    LCDC_MCUInit(LCDC, &mcu);

    LCDC->LCDC_MCU_TIMING_CFG = LCDC_WRPULW(wrpulw) | LCDC_RDACTW(rdactw) | LCDC_RDINACTW(rdinactw);

    LCDC_DMABurstSizeConfig(LCDC, dma_burst);

    LCDC_Cmd(LCDC, ENABLE);
    LCDC_MCUIOMode(LCDC);

    s_fb_addr   = 0;
    s_fb_width  = width;
    s_fb_height = height;
    s_fb_bpp    = fmt_to_bpp(input_fmt);
    s_initialized = 1;
    s_mcu_mode    = 1;

    lua_pushboolean(L, 1);
    return 1;
}

/* lcdc.mcu_io_write_cmd(cmd) → true */
static int lua_lcdc_mcu_io_write_cmd(lua_State *L)
{
    if (!s_initialized || !s_mcu_mode) {
        return luaL_error(L, "lcdc: MCU mode not initialized");
    }
    LCDC_MCUIOWriteCmd(LCDC, (uint32_t)luaL_checkinteger(L, 1));
    lua_pushboolean(L, 1);
    return 1;
}

/* lcdc.mcu_io_write_data(data) → true */
static int lua_lcdc_mcu_io_write_data(lua_State *L)
{
    if (!s_initialized || !s_mcu_mode) {
        return luaL_error(L, "lcdc: MCU mode not initialized");
    }
    LCDC_MCUIOWriteData(LCDC, (uint32_t)luaL_checkinteger(L, 1));
    lua_pushboolean(L, 1);
    return 1;
}

/* lcdc.mcu_io_read() → data */
static int lua_lcdc_mcu_io_read(lua_State *L)
{
    if (!s_initialized || !s_mcu_mode) {
        return luaL_error(L, "lcdc: MCU mode not initialized");
    }
    lua_pushinteger(L, (lua_Integer)LCDC_MCUIOReadData(LCDC));
    return 1;
}

/* lcdc.mcu_dma_start(fb_addr [, trigger_mode [, burst_size]]) → true
 *
 * Switch to DMA mode and start pixel data transfer.
 *   trigger_mode — 0=auto continuous (default), 1=one-shot trigger
 *   burst_size   — 0=64B, 1=128B, 2=256B (default 2)
 *
 * In auto mode the DMA refreshes the panel continuously.
 * In trigger mode, call mcu_dma_trigger() to push one frame. */
static int lua_lcdc_mcu_dma_start(lua_State *L)
{
    if (!s_initialized || !s_mcu_mode) {
        return luaL_error(L, "lcdc: MCU mode not initialized");
    }
    uint32_t fb_addr      = (uint32_t)luaL_checkinteger(L, 1);
    int      trigger_mode = (int)luaL_optinteger(L, 2, 0);
    uint32_t burst        = (uint32_t)luaL_optinteger(L, 3, 2);

    LCDC_Cmd(LCDC, DISABLE);

    Lcdc_McuDmaCfgDef dma_cfg;
    dma_cfg.TriggerDma = trigger_mode ? LCDC_TRIGGER_DMA_MODE : LCDC_AUTO_DMA_MODE;
    dma_cfg.TeMode     = 0;
    dma_cfg.TeDelay    = 0;
    LCDC_MCUDmaMode(LCDC, &dma_cfg);

    LCDC_DMABurstSizeConfig(LCDC, burst);
    LCDC_DMAImgCfg(LCDC, fb_addr);
    s_fb_addr = fb_addr;

    LCDC_Cmd(LCDC, ENABLE);

    if (trigger_mode) {
        LCDC_MCUDMATrigger(LCDC);
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* lcdc.mcu_dma_trigger() → true  (trigger one frame in trigger mode) */
static int lua_lcdc_mcu_dma_trigger(lua_State *L)
{
    if (!s_initialized || !s_mcu_mode) {
        return luaL_error(L, "lcdc: MCU mode not initialized");
    }
    LCDC_MCUDMATrigger(LCDC);
    lua_pushboolean(L, 1);
    return 1;
}

/* lcdc.mcu_set_pre_cmd(cmd [, cmd2 ...]) → true
 * Set up to 16 commands sent automatically by LCDC hardware before each DMA frame.
 * Must be called before mcu_dma_start(). Typically one command: 0x2C (memory write). */
static int lua_lcdc_mcu_set_pre_cmd(lua_State *L)
{
    if (!s_initialized || !s_mcu_mode) {
        return luaL_error(L, "lcdc: MCU mode not initialized");
    }
    int n = lua_gettop(L);
    if (n < 1 || n > 16) {
        return luaL_error(L, "lcdc.mcu_set_pre_cmd: 1-16 command bytes required");
    }
    uint8_t cmds[16];
    for (int i = 0; i < n; i++) {
        cmds[i] = (uint8_t)luaL_checkinteger(L, i + 1);
    }
    LCDC_MCUSetPreCmd(LCDC, cmds, (uint8_t)n);
    lua_pushboolean(L, 1);
    return 1;
}

/* lcdc.mcu_reset_pre_cmd() → true
 * Clear all pre-commands registered by mcu_set_pre_cmd(). */
static int lua_lcdc_mcu_reset_pre_cmd(lua_State *L)
{
    if (!s_initialized || !s_mcu_mode) {
        return luaL_error(L, "lcdc: MCU mode not initialized");
    }
    LCDC_MCUResetPreCmd(LCDC);
    lua_pushboolean(L, 1);
    return 1;
}

/* lcdc.mcu_get_run_status() → status */
static int lua_lcdc_mcu_get_run_status(lua_State *L)
{
    if (!s_initialized || !s_mcu_mode) {
        return luaL_error(L, "lcdc: MCU mode not initialized");
    }
    lua_pushinteger(L, (lua_Integer)LCDC_MCUGetRunStatus(LCDC));
    return 1;
}

/* ---- Module entry ---- */

int luaopen_lcdc(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        /* ---- Shared: work for both RGB and MCU mode ---- */
        {"pinmux",             lua_lcdc_pinmux},
        {"deinit",             lua_lcdc_deinit},
        {"enable",             lua_lcdc_enable},
        {"update",             lua_lcdc_update},           /* DMA shadow reload */
        {"set_framebuf",       lua_lcdc_set_framebuf},
        {"get_framebuf_addr",  lua_lcdc_get_framebuf_addr},
        {"get_cur_pos",        lua_lcdc_get_cur_pos},
        {"get_int_status",     lua_lcdc_get_int_status},
        {"clear_int",          lua_lcdc_clear_int},
        {"set_underflow_mode", lua_lcdc_set_underflow_mode},
        {"set_line_int_pos",   lua_lcdc_set_line_int_pos},
        {"int_config",         lua_lcdc_int_config},
        {"get_info",           lua_lcdc_get_info},
        /* ---- RGB parallel interface ---- */
        {"rgb_init",               lua_lcdc_rgb_init},
        {"rgb_get_sync_status",    lua_lcdc_rgb_get_sync_status},
        /* ---- MCU (8080 parallel) interface ---- */
        {"mcu_init",           lua_lcdc_mcu_init},
        {"mcu_io_write_cmd",   lua_lcdc_mcu_io_write_cmd},
        {"mcu_io_write_data",  lua_lcdc_mcu_io_write_data},
        {"mcu_io_read",        lua_lcdc_mcu_io_read},
        {"mcu_dma_start",      lua_lcdc_mcu_dma_start},
        {"mcu_dma_trigger",    lua_lcdc_mcu_dma_trigger},
        {"mcu_set_pre_cmd",    lua_lcdc_mcu_set_pre_cmd},
        {"mcu_reset_pre_cmd",  lua_lcdc_mcu_reset_pre_cmd},
        {"mcu_get_run_status", lua_lcdc_mcu_get_run_status},
        {NULL, NULL}
    };
    luaL_newlib(L, funcs);
    return 1;
}

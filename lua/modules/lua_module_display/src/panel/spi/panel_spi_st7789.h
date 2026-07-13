/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * panel_spi_st7789.h — SPI TX-only transport layer for the LVGL ST7789 driver.
 *
 * This is the "panel HAL" described in design_spec/display/phase1_panel_hal.md
 * §1/§2: we own the pins, SSI instance and DMA; LVGL owns init timing and frame
 * assembly (CASET/RASET/RAMWR).  We expose exactly two send callbacks to LVGL
 * (st7789_panel_send_cmd / st7789_panel_send_color) plus panel bring-up / teardown helpers.
 *
 * Single owner, single panel → a file-static context (s_panel) is read by the
 * callbacks; no per-display handle threads through LVGL (see phase1 §1 "context
 * 顺序坑").
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "lv_display.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Panel pin + geometry configuration parsed from board.json by display_lua.c. */
typedef struct {
    int      spi_idx;   /* SSI controller index (SPI1 → 1)                    */
    uint16_t clk_pin;   /* PinName encodings (port<<5 | num)                  */
    uint16_t mosi_pin;
    uint16_t cs_pin;    /* driven as a plain GPIO (manual CS, see phase1 §2)  */
    uint16_t dc_pin;
    uint16_t rst_pin;
    uint16_t blk_pin;
    uint16_t width;
    uint16_t height;
    uint8_t  invert;    /* 1 → issue INVON via lv_st7789_set_invert           */
} panel_cfg_t;

/*
 * st7789_panel_init — configure SSI (TMOD_TO / TX-only), the DC/CS/RST/BLK GPIOs,
 * issue a hard reset and turn the backlight on, then latch the config into the
 * file-static s_panel context.  Must be called BEFORE lv_st7789_create (which
 * immediately drives the init command list through st7789_panel_send_cmd).
 *
 * Returns 0 on success, negative on failure (SSI/GPIO error).  On failure the
 * caller must not call lv_st7789_create.
 */
int  st7789_panel_init(const panel_cfg_t *cfg);

/* st7789_panel_deinit — release the DMA sema/state and drop the backlight.  Idempotent. */
void st7789_panel_deinit(void);

/* Backlight control (on/off).  Safe to call after st7789_panel_init. */
void st7789_panel_backlight(int on);

/* The two LVGL transport callbacks (types from lv_lcd_generic_mipi.h). */
void st7789_panel_send_cmd(lv_display_t *disp, const uint8_t *cmd, size_t cmd_size,
                    const uint8_t *param, size_t param_size);
void st7789_panel_send_color(lv_display_t *disp, const uint8_t *cmd, size_t cmd_size,
                      uint8_t *param, size_t param_size);

/*
 * st7789_panel_present_window — fast path: push a contiguous RGB565_SWAPPED
 * pixel block into the panel window (x,y,w,h) in a single CASET/RASET/RAMWR +
 * one TX-DMA, bypassing LVGL's partial flush.  fb must be a contiguous, DMA-
 * capable, 4-byte-aligned block of exactly w*h*2 bytes (row-major, no padding).
 * See design_spec/display/phase2_present_fastpath.md §2 / §5.
 */
void st7789_panel_present_window(uint8_t *fb, uint16_t x, uint16_t y,
                                 uint16_t w, uint16_t h);

/* st7789_panel_present_full — present_window over the whole panel at (0,0). */
void st7789_panel_present_full(uint8_t *fb, uint16_t w, uint16_t h);

#ifdef __cplusplus
}
#endif

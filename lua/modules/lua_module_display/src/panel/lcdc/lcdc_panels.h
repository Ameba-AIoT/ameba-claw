/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * lcdc_panels.h — per-chip electrical constants for the LCDC/RGB display
 * backend (phase3_lcdc_rgb_st7701p.md §5.2, decision 3.5).
 *
 * This header holds ONLY what is fixed by the panel MODEL and never changes
 * with the wiring: RGB timing (porches / pulse widths), sync/DE/DCLK polarity,
 * reset pulse timing, and — for panels that need it — the register-init command
 * sequence run over a 9-bit SPI before the RGB scan-out starts.  These mirror
 * the SDK's per-panel C files (panel_st7701p_rgb.c + panel_st7701p_rgb_spi.inc,
 * panel_jd9165ba.c), which are likewise hard-coded per model.
 *
 * PINS ARE NOT HERE.  Every pin (reset/bl/power + the 9-bit SPI legs + the ~24
 * RGB data lines + hsync/vsync/dclk/de) lives in board.json and is parsed at
 * runtime by display_backend_lcdc.c — exactly like the SPI/ST7789 backend.
 * Change the wiring → edit board.json.  Change the panel MODEL → edit this file.
 *
 * Included by display_backend_lcdc.c ONLY (static const → no multiple-def).
 */
#pragma once

#include <stdint.h>

/* One step of a 9-bit SPI register-init sequence.  `op` selects the meaning of
 * `val`: a command byte (D/C=0), a data byte (D/C=1), or a millisecond delay. */
enum {
    LCDC_SPI_OP_CMD      = 0,   /* val = command byte  → spi_master_write(val)        */
    LCDC_SPI_OP_DATA     = 1,   /* val = data byte     → spi_master_write(val | BIT8) */
    LCDC_SPI_OP_DELAY_MS = 2,   /* val = milliseconds  → rtos_time_delay_ms(val)      */
};

typedef struct {
    uint8_t op;
    uint8_t val;
} lcdc_spi_op_t;

/* All the per-model constants the backend needs.  Timing units follow the LCDC
 * HAL (LCDC_RGBInitTypeDef): H* in DCLKs, V* in lines.  Polarity/edge flags are
 * booleans the backend maps onto the LCDC_RGB_* enums.  Reset is H(h1)→L(l)→H(h2). */
typedef struct {
    const char *chip;            /* matches board.json "chip" (case-insensitive)      */
    uint16_t    w, h;            /* panel resolution (authoritative)                  */

    uint16_t hfp, hbp, hsw;      /* horizontal front/back porch, sync width (DCLKs)   */
    uint16_t vfp, vbp, vsw;      /* vertical   front/back porch, sync width (lines)   */
    uint16_t refresh_freq;       /* RGBRefreshFreq (Hz) — NOT the pixel clock         */

    uint8_t hs_low;              /* 1 = HSYNC low-active   (LCDC_RGB_HS_PUL_LOW_LEV)   */
    uint8_t vs_low;              /* 1 = VSYNC low-active                              */
    uint8_t de_high;             /* 1 = DE high-active     (LCDC_RGB_EN_PUL_HIGH_LEV) */
    uint8_t dclk_falling;        /* 1 = data fetched on DCLK falling edge             */

    uint16_t rst_h1_ms, rst_l_ms, rst_h2_ms;  /* reset pulse: high→low→high (ms)      */

    uint8_t              needs_spi_init;  /* 1 = run spi_ops before RGB scan-out       */
    const lcdc_spi_op_t *spi_ops;
    uint16_t             spi_ops_count;
} lcdc_panel_t;

/* ------------------------------------------------------------------------- *
 * ST7701P 480x480 — 9-bit SPI register init.  Transcribed VERBATIM from
 * mcu_sdk .../panels/panel_st7701p_rgb_spi.inc (spi_write_command → CMD,
 * spi_write_data → DATA, rtos_time_delay_ms(120) → DELAY_MS 120).
 * ------------------------------------------------------------------------- */
static const lcdc_spi_op_t st7701p_spi_ops[] = {
    {LCDC_SPI_OP_CMD, 0xFF}, {LCDC_SPI_OP_DATA, 0x77}, {LCDC_SPI_OP_DATA, 0x01},
    {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x13},
    {LCDC_SPI_OP_CMD, 0xEF}, {LCDC_SPI_OP_DATA, 0x08},
    {LCDC_SPI_OP_CMD, 0xFF}, {LCDC_SPI_OP_DATA, 0x77}, {LCDC_SPI_OP_DATA, 0x01},
    {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x10},
    {LCDC_SPI_OP_CMD, 0xC0}, {LCDC_SPI_OP_DATA, 0x3B}, {LCDC_SPI_OP_DATA, 0x00},
    {LCDC_SPI_OP_CMD, 0xC1}, {LCDC_SPI_OP_DATA, 0x0D}, {LCDC_SPI_OP_DATA, 0x02},
    {LCDC_SPI_OP_CMD, 0xC2}, {LCDC_SPI_OP_DATA, 0x37}, {LCDC_SPI_OP_DATA, 0x08},
    {LCDC_SPI_OP_CMD, 0xC7}, {LCDC_SPI_OP_DATA, 0x00},
    {LCDC_SPI_OP_CMD, 0xCC}, {LCDC_SPI_OP_DATA, 0x18},
    {LCDC_SPI_OP_CMD, 0xB0},
    {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x11}, {LCDC_SPI_OP_DATA, 0x17},
    {LCDC_SPI_OP_DATA, 0x0E}, {LCDC_SPI_OP_DATA, 0x12}, {LCDC_SPI_OP_DATA, 0x06},
    {LCDC_SPI_OP_DATA, 0x06}, {LCDC_SPI_OP_DATA, 0x08}, {LCDC_SPI_OP_DATA, 0x08},
    {LCDC_SPI_OP_DATA, 0x20}, {LCDC_SPI_OP_DATA, 0x04}, {LCDC_SPI_OP_DATA, 0x11},
    {LCDC_SPI_OP_DATA, 0x0F}, {LCDC_SPI_OP_DATA, 0x29}, {LCDC_SPI_OP_DATA, 0x30},
    {LCDC_SPI_OP_DATA, 0x1F},
    {LCDC_SPI_OP_CMD, 0xB1},
    {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x13}, {LCDC_SPI_OP_DATA, 0x18},
    {LCDC_SPI_OP_DATA, 0x0F}, {LCDC_SPI_OP_DATA, 0x12}, {LCDC_SPI_OP_DATA, 0x07},
    {LCDC_SPI_OP_DATA, 0x06}, {LCDC_SPI_OP_DATA, 0x08}, {LCDC_SPI_OP_DATA, 0x07},
    {LCDC_SPI_OP_DATA, 0x21}, {LCDC_SPI_OP_DATA, 0x04}, {LCDC_SPI_OP_DATA, 0x12},
    {LCDC_SPI_OP_DATA, 0x10}, {LCDC_SPI_OP_DATA, 0x29}, {LCDC_SPI_OP_DATA, 0x34},
    {LCDC_SPI_OP_DATA, 0x1F},
    {LCDC_SPI_OP_CMD, 0xFF}, {LCDC_SPI_OP_DATA, 0x77}, {LCDC_SPI_OP_DATA, 0x01},
    {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x11},
    {LCDC_SPI_OP_CMD, 0xB0}, {LCDC_SPI_OP_DATA, 0x60},
    {LCDC_SPI_OP_CMD, 0xB1}, {LCDC_SPI_OP_DATA, 0x32},
    {LCDC_SPI_OP_CMD, 0xB2}, {LCDC_SPI_OP_DATA, 0x8A},
    {LCDC_SPI_OP_CMD, 0xB3}, {LCDC_SPI_OP_DATA, 0x80},
    {LCDC_SPI_OP_CMD, 0xB5}, {LCDC_SPI_OP_DATA, 0x4B},
    {LCDC_SPI_OP_CMD, 0xB7}, {LCDC_SPI_OP_DATA, 0x85},
    {LCDC_SPI_OP_CMD, 0xB8}, {LCDC_SPI_OP_DATA, 0x21},
    {LCDC_SPI_OP_CMD, 0xC0}, {LCDC_SPI_OP_DATA, 0x07},
    {LCDC_SPI_OP_CMD, 0xC1}, {LCDC_SPI_OP_DATA, 0x78},
    {LCDC_SPI_OP_CMD, 0xC2}, {LCDC_SPI_OP_DATA, 0x78},
    {LCDC_SPI_OP_CMD, 0xE0}, {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x1B},
    {LCDC_SPI_OP_DATA, 0x02},
    {LCDC_SPI_OP_CMD, 0xE1},
    {LCDC_SPI_OP_DATA, 0x08}, {LCDC_SPI_OP_DATA, 0xA0}, {LCDC_SPI_OP_DATA, 0x00},
    {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x07}, {LCDC_SPI_OP_DATA, 0xA0},
    {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x00},
    {LCDC_SPI_OP_DATA, 0x44}, {LCDC_SPI_OP_DATA, 0x44},
    {LCDC_SPI_OP_CMD, 0xE2},
    {LCDC_SPI_OP_DATA, 0x11}, {LCDC_SPI_OP_DATA, 0x11}, {LCDC_SPI_OP_DATA, 0x44},
    {LCDC_SPI_OP_DATA, 0x44}, {LCDC_SPI_OP_DATA, 0xED}, {LCDC_SPI_OP_DATA, 0xA0},
    {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0xEC},
    {LCDC_SPI_OP_DATA, 0xA0}, {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x00},
    {LCDC_SPI_OP_CMD, 0xE3},
    {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x11},
    {LCDC_SPI_OP_DATA, 0x11},
    {LCDC_SPI_OP_CMD, 0xE4}, {LCDC_SPI_OP_DATA, 0x44}, {LCDC_SPI_OP_DATA, 0x44},
    {LCDC_SPI_OP_CMD, 0xE5},
    {LCDC_SPI_OP_DATA, 0x0A}, {LCDC_SPI_OP_DATA, 0xE9}, {LCDC_SPI_OP_DATA, 0xD8},
    {LCDC_SPI_OP_DATA, 0xA0}, {LCDC_SPI_OP_DATA, 0x0C}, {LCDC_SPI_OP_DATA, 0xEB},
    {LCDC_SPI_OP_DATA, 0xD8}, {LCDC_SPI_OP_DATA, 0xA0}, {LCDC_SPI_OP_DATA, 0x0E},
    {LCDC_SPI_OP_DATA, 0xED}, {LCDC_SPI_OP_DATA, 0xD8}, {LCDC_SPI_OP_DATA, 0xA0},
    {LCDC_SPI_OP_DATA, 0x10}, {LCDC_SPI_OP_DATA, 0xEF}, {LCDC_SPI_OP_DATA, 0xD8},
    {LCDC_SPI_OP_DATA, 0xA0},
    {LCDC_SPI_OP_CMD, 0xE6},
    {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x11},
    {LCDC_SPI_OP_DATA, 0x11},
    {LCDC_SPI_OP_CMD, 0xE7}, {LCDC_SPI_OP_DATA, 0x44}, {LCDC_SPI_OP_DATA, 0x44},
    {LCDC_SPI_OP_CMD, 0xE8},
    {LCDC_SPI_OP_DATA, 0x09}, {LCDC_SPI_OP_DATA, 0xE8}, {LCDC_SPI_OP_DATA, 0xD8},
    {LCDC_SPI_OP_DATA, 0xA0}, {LCDC_SPI_OP_DATA, 0x0B}, {LCDC_SPI_OP_DATA, 0xEA},
    {LCDC_SPI_OP_DATA, 0xD8}, {LCDC_SPI_OP_DATA, 0xA0}, {LCDC_SPI_OP_DATA, 0x0D},
    {LCDC_SPI_OP_DATA, 0xEC}, {LCDC_SPI_OP_DATA, 0xD8}, {LCDC_SPI_OP_DATA, 0xA0},
    {LCDC_SPI_OP_DATA, 0x0F}, {LCDC_SPI_OP_DATA, 0xEE}, {LCDC_SPI_OP_DATA, 0xD8},
    {LCDC_SPI_OP_DATA, 0xA0},
    {LCDC_SPI_OP_CMD, 0xEB},
    {LCDC_SPI_OP_DATA, 0x02}, {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0xE4},
    {LCDC_SPI_OP_DATA, 0xE4}, {LCDC_SPI_OP_DATA, 0x88}, {LCDC_SPI_OP_DATA, 0x00},
    {LCDC_SPI_OP_DATA, 0x40},
    {LCDC_SPI_OP_CMD, 0xEC}, {LCDC_SPI_OP_DATA, 0x3C}, {LCDC_SPI_OP_DATA, 0x00},
    {LCDC_SPI_OP_CMD, 0xED},
    {LCDC_SPI_OP_DATA, 0xAB}, {LCDC_SPI_OP_DATA, 0x89}, {LCDC_SPI_OP_DATA, 0x76},
    {LCDC_SPI_OP_DATA, 0x54}, {LCDC_SPI_OP_DATA, 0x02}, {LCDC_SPI_OP_DATA, 0xFF},
    {LCDC_SPI_OP_DATA, 0xFF}, {LCDC_SPI_OP_DATA, 0xFF}, {LCDC_SPI_OP_DATA, 0xFF},
    {LCDC_SPI_OP_DATA, 0xFF}, {LCDC_SPI_OP_DATA, 0xFF}, {LCDC_SPI_OP_DATA, 0x20},
    {LCDC_SPI_OP_DATA, 0x45}, {LCDC_SPI_OP_DATA, 0x67},
    {LCDC_SPI_OP_DATA, 0x98}, {LCDC_SPI_OP_DATA, 0xBA},
    {LCDC_SPI_OP_CMD, 0xEF},
    {LCDC_SPI_OP_DATA, 0x08}, {LCDC_SPI_OP_DATA, 0x08}, {LCDC_SPI_OP_DATA, 0x08},
    {LCDC_SPI_OP_DATA, 0x45}, {LCDC_SPI_OP_DATA, 0x3F}, {LCDC_SPI_OP_DATA, 0x54},
    {LCDC_SPI_OP_CMD, 0xFF}, {LCDC_SPI_OP_DATA, 0x77}, {LCDC_SPI_OP_DATA, 0x01},
    {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x13},
    {LCDC_SPI_OP_CMD, 0xE8}, {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x0E},
    {LCDC_SPI_OP_CMD, 0xFF}, {LCDC_SPI_OP_DATA, 0x77}, {LCDC_SPI_OP_DATA, 0x01},
    {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x00},
    {LCDC_SPI_OP_CMD, 0x11},
    {LCDC_SPI_OP_DELAY_MS, 120},
    {LCDC_SPI_OP_CMD, 0xFF}, {LCDC_SPI_OP_DATA, 0x77}, {LCDC_SPI_OP_DATA, 0x01},
    {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x13},
    {LCDC_SPI_OP_CMD, 0xE8}, {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x0C},
    {LCDC_SPI_OP_DELAY_MS, 120},
    {LCDC_SPI_OP_CMD, 0xE8}, {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x00},
    {LCDC_SPI_OP_CMD, 0xFF}, {LCDC_SPI_OP_DATA, 0x77}, {LCDC_SPI_OP_DATA, 0x01},
    {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x00}, {LCDC_SPI_OP_DATA, 0x00},
    {LCDC_SPI_OP_CMD, 0x29},
    {LCDC_SPI_OP_CMD, 0x36}, {LCDC_SPI_OP_DATA, 0x00},
};

/* Per-chip table.  Look up by board.json "chip" (case-insensitive).
 *   ST7701P : 480x480, dclk RISING edge, 9-bit SPI init, reset H4→L30→H120.
 *   JD9165BA: 1024x600, dclk FALLING edge, no SPI init,   reset H10→L10→H120.
 * (jd9165ba entry present for completeness; not yet bench-validated — §7.) */
static const lcdc_panel_t s_lcdc_panels[] = {
    {
        .chip = "ST7701P", .w = 480, .h = 480,
        .hfp = 15, .hbp = 2, .hsw = 2,
        .vfp = 15, .vbp = 12, .vsw = 3,
        .refresh_freq = 60,
        .hs_low = 1, .vs_low = 1, .de_high = 1, .dclk_falling = 0,
        .rst_h1_ms = 4, .rst_l_ms = 30, .rst_h2_ms = 120,
        .needs_spi_init = 1,
        .spi_ops = st7701p_spi_ops,
        .spi_ops_count = (uint16_t)(sizeof(st7701p_spi_ops) / sizeof(st7701p_spi_ops[0])),
    },
    {
        .chip = "JD9165BA", .w = 1024, .h = 600,
        .hfp = 160, .hbp = 160, .hsw = 24,
        .vfp = 12, .vbp = 23, .vsw = 2,
        .refresh_freq = 60,
        .hs_low = 1, .vs_low = 1, .de_high = 1, .dclk_falling = 1,
        .rst_h1_ms = 10, .rst_l_ms = 10, .rst_h2_ms = 120,
        .needs_spi_init = 0,
        .spi_ops = NULL,
        .spi_ops_count = 0,
    },
};

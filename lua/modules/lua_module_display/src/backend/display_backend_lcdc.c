/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * display_backend_lcdc.c — RGB/LCDC framebuffer backend for the "display"
 * module (phase3_lcdc_rgb_st7701p.md).  Implements the display_backend_t vtable
 * on top of the LCDC controller driving an RGB parallel panel.  The common
 * layer (display_lua.c) needs NO changes — see §2.6 of phase2 for the seam.
 *
 * How it differs from the SPI/ST7789 backend (the three §2.6 assumptions):
 *   1. encode():   native RGB565 (NO byte swap); surface.bpp = 2.
 *   2. render_buf: the lv_canvas buffer IS the LCDC scan-out framebuffer, so
 *                  AA primitives and the direct-write fast path both land
 *                  straight in the framebuffer — no per-frame copy.
 *   3. cache:      LCDC scan-out DMA is not cache-coherent, so present_full/
 *                  present_rect DCache_Clean the (dirty) framebuffer region;
 *                  there is no RAMWR/page-flip — the DMA scans fb continuously.
 *
 * Single-buffered bring-up (decision 3): the framebuffer both receives drawing
 * and is scanned out, so heavy AA rendering can momentarily tear.  Acceptable
 * for bring-up; double-buffer + VSYNC flip is the phase3 §6 optimisation.
 *
 * Panel MODEL constants (timing / polarity / reset ms / 9-bit SPI init) come
 * from lcdc_panels.h (per-chip table).  Board WIRING (every pin) comes from
 * board.json, parsed here exactly like the SPI backend's load_panel_cfg().
 */

#include "display_backend.h"
#include "lcdc_panels.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "ameba_soc.h"
#include "spi_api.h"
#include "os_wrapper.h"
#include "ameba_claw_defs.h"

#include "cJSON.h"
#include "claw_cap.h"

#include "lvgl.h"

/* Raw (pre-alignment) allocations + aux pin state, kept for deinit(). */
static struct {
    uint8_t *fb_raw[3];   /* per-buffer raw allocs (PSRAM heap; freed in deinit) */
    uint8_t *dbuf_raw;    /* dummy LVGL draw buffer raw alloc         */
    uint16_t blk_pin;     /* backlight GPIO (PinName)                 */
    uint8_t  has_blk;
    uint16_t power_en_pin;
    uint8_t  has_power_en;
    volatile uint32_t udf_count;  /* BRING-UP: DMA FIFO underflow IRQ counter */
    rtos_sema_t vsync_sema;       /* posted by the frame-done IRQ; present() waits
                                   * on it instead of busy-spinning on VBR       */
} s_lcdc;

/* Parsed board.json wiring for one RGB panel device. */
typedef struct {
    uint16_t reset;
    uint8_t  has_reset;    /* 0 = no reset pin (e.g. T1720A power_en-only panel) */
    uint16_t power_en;     /* optional: drive HIGH at init before scan-out       */
    uint8_t  has_power_en;
    uint16_t blk;
    uint8_t  has_blk;
    uint16_t spi_cs, spi_sclk, spi_mosi;   /* only if panel->needs_spi_init */
    uint16_t hsync, vsync, dclk, de;       /* hsync/vsync may be 0xFFFF (DE-only) */
    uint16_t data[24];
    const lcdc_panel_t *panel;
} lcdc_cfg_t;

/* ========================================================================= */
/* board.json pin parsing (mirrors display_backend_spi.c)                     */
/* ========================================================================= */

/* Parse "PA_0".."PC_31" → PinName-encoded uint16 (port<<5 | num).  0xFFFF on
 * failure.  The fwlib pin macros, mbed PinName, and this encoding all agree
 * (port<<5|num), so the result feeds Pinmux_Config / GPIO / spi_init alike. */
static uint16_t parse_pin(const char *s)
{
    if (!s || (s[0] != 'P' && s[0] != 'p')) {
        return 0xFFFF;
    }
    int port;
    switch (s[1]) {
        case 'A': case 'a': port = 0; break;
        case 'B': case 'b': port = 1; break;
        case 'C': case 'c': port = 2; break;
        default: return 0xFFFF;
    }
    const char *n = (s[2] == '_') ? &s[3] : &s[2];
    char *end;
    long num = strtol(n, &end, 10);
    if (*end != '\0' || num < 0 || num > 31) {
        return 0xFFFF;
    }
    return (uint16_t)((port << 5) | (int)num);
}

/* Parse one optional string pin field; sets *dst = 0xFFFF if absent/invalid. */
static void parse_opt_pin(cJSON *params, const char *key, uint16_t *dst)
{
    cJSON *p = cJSON_GetObjectItem(params, key);
    *dst = (p && cJSON_IsString(p)) ? parse_pin(p->valuestring) : 0xFFFF;
}

/* Parse one required string pin field from `params` into *dst. */
static int parse_req_pin(cJSON *params, const char *key, uint16_t *dst,
                         const char *dev_id, char *err, size_t errlen)
{
    cJSON *p = cJSON_GetObjectItem(params, key);
    if (!p || !cJSON_IsString(p)) {
        snprintf(err, errlen, "device '%s' missing pin '%s'", dev_id, key);
        return -1;
    }
    uint16_t pin = parse_pin(p->valuestring);
    if (pin == 0xFFFF) {
        snprintf(err, errlen, "device '%s' bad pin '%s'=%s", dev_id, key,
                 p->valuestring);
        return -1;
    }
    *dst = pin;
    return 0;
}

/* Fetch device `dev_id` from cap_board_mgr, look up its panel model in the
 * per-chip table, and parse every wiring pin into *cfg.  Returns 0 on success. */
static int load_lcdc_cfg(const char *dev_id, lcdc_cfg_t *cfg,
                         char *err, size_t errlen)
{
    char  input[96];
    char *out = NULL;

    memset(cfg, 0, sizeof(*cfg));
    snprintf(input, sizeof(input), "{\"id\":\"%s\"}", dev_id);

    claw_cap_call_context_t ctx = { 0 };
    ctx.caller = CLAW_CAP_CALLER_INTERNAL;
    (void)claw_cap_call("board_get_device", input, &ctx, &out);
    if (!out) {
        snprintf(err, errlen, "board_get_device returned nothing");
        return -1;
    }

    cJSON *root = cJSON_Parse(out);
    free(out);
    if (!root) {
        snprintf(err, errlen, "board_get_device: bad JSON");
        return -1;
    }

    cJSON *jerr = cJSON_GetObjectItem(root, "error");
    if (jerr && cJSON_IsString(jerr)) {
        snprintf(err, errlen, "%s", jerr->valuestring);
        cJSON_Delete(root);
        return -1;
    }

    /* chip → per-chip panel table (case-insensitive). */
    cJSON *chip = cJSON_GetObjectItem(root, "chip");
    if (!chip || !cJSON_IsString(chip)) {
        snprintf(err, errlen, "device '%s' has no chip", dev_id);
        cJSON_Delete(root);
        return -1;
    }
    for (size_t i = 0; i < sizeof(s_lcdc_panels) / sizeof(s_lcdc_panels[0]); i++) {
        if (strcasecmp(chip->valuestring, s_lcdc_panels[i].chip) == 0) {
            cfg->panel = &s_lcdc_panels[i];
            break;
        }
    }
    if (!cfg->panel) {
        snprintf(err, errlen, "no LCDC panel table entry for chip '%s'",
                 chip->valuestring);
        cJSON_Delete(root);
        return -1;
    }

    cJSON *params = cJSON_GetObjectItem(root, "params");
    if (!params) {
        snprintf(err, errlen, "device '%s' has no params", dev_id);
        cJSON_Delete(root);
        return -1;
    }

    /* reset and hsync/vsync are optional: absent → 0xFFFF (DE-only panels, e.g. T1720A). */
    parse_opt_pin(params, "reset", &cfg->reset);
    cfg->has_reset = (cfg->reset != 0xFFFF);
    parse_opt_pin(params, "hsync", &cfg->hsync);
    parse_opt_pin(params, "vsync", &cfg->vsync);

    /* power_en optional: drive HIGH at init (T1720A uses this instead of reset). */
    parse_opt_pin(params, "power_en", &cfg->power_en);
    cfg->has_power_en = (cfg->power_en != 0xFFFF);

    /* dclk and de are always required. */
    if (parse_req_pin(params, "dclk", &cfg->dclk, dev_id, err, errlen) ||
        parse_req_pin(params, "de",   &cfg->de,   dev_id, err, errlen)) {
        cJSON_Delete(root);
        return -1;
    }

    /* Backlight is optional (some boards tie it high). */
    parse_opt_pin(params, "blk", &cfg->blk);
    cfg->has_blk = (cfg->blk != 0xFFFF);

    /* 9-bit register-init SPI legs, required only for panels that need init. */
    if (cfg->panel->needs_spi_init) {
        if (parse_req_pin(params, "spi_cs",   &cfg->spi_cs,   dev_id, err, errlen) ||
            parse_req_pin(params, "spi_sclk", &cfg->spi_sclk, dev_id, err, errlen) ||
            parse_req_pin(params, "spi_mosi", &cfg->spi_mosi, dev_id, err, errlen)) {
            cJSON_Delete(root);
            return -1;
        }
    }

    /* The 24 RGB data lines D0..D23. */
    cJSON *data = cJSON_GetObjectItem(params, "data");
    if (!data || !cJSON_IsArray(data) || cJSON_GetArraySize(data) != 24) {
        snprintf(err, errlen, "device '%s' needs a 24-element 'data' pin array",
                 dev_id);
        cJSON_Delete(root);
        return -1;
    }
    for (int i = 0; i < 24; i++) {
        cJSON *d = cJSON_GetArrayItem(data, i);
        if (!d || !cJSON_IsString(d)) {
            snprintf(err, errlen, "device '%s' data[%d] not a string", dev_id, i);
            cJSON_Delete(root);
            return -1;
        }
        uint16_t pin = parse_pin(d->valuestring);
        if (pin == 0xFFFF) {
            snprintf(err, errlen, "device '%s' bad data[%d]=%s", dev_id, i,
                     d->valuestring);
            cJSON_Delete(root);
            return -1;
        }
        cfg->data[i] = pin;
    }

    cJSON_Delete(root);
    return 0;
}

/* ========================================================================= */
/* helpers                                                                    */
/* ========================================================================= */

/* Allocate `size` bytes 64-byte aligned from PSRAM (TYPE_DRAM, 0x60000000+).
 * Framebuffers are too large for SRAM heap (e.g. 800x480 RGB565 = 768 KB). */
static uint8_t *alloc_aligned(size_t size, uint8_t **raw)
{
    uint8_t *r = rtos_heap_types_malloc((u32)(size + 63U), TYPE_DRAM);
    if (!r) {
        return NULL;
    }
    *raw = r;
    return (uint8_t *)(((uintptr_t)r + 63U) & ~(uintptr_t)63U);
}

/* Configure a pin as a push-pull output and drive it to `level`. */
static void gpio_out_init(uint16_t pin, int level)
{
    GPIO_InitTypeDef init;
    init.GPIO_Pin  = (u32)pin;
    init.GPIO_Mode = GPIO_Mode_OUT;
    init.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(&init);
    GPIO_WriteBit((u32)pin, level ? 1 : 0);
}

/* LCDC IRQ handler.  Two sources are enabled:
 *   - DMA underflow: just counted (bring-up diagnostic), then cleared so a FIFO
 *     underrun doesn't wedge the controller.
 *   - DMA frame-start (LCDC_BIT_FRM_START_INTS): fires at the top of each new
 *     frame, AFTER the vblank in which a pending shadow-reload (page flip) has
 *     already latched — so on this edge VBR is guaranteed clear.  (Frame-DONE
 *     fires at the last active pixel, before the reload point, so it could wake
 *     present() a frame too early.)  We post vsync_sema here so present()'s flip
 *     wait is interrupt-driven — the task sleeps and yields the CPU instead of
 *     busy-spinning on VBR.  rtos_sema_give detects ISR context internally. */
static void lcdc_irq_handler(void)
{
    uint32_t ints = LCDC_GetINTStatus(LCDC);
    if (ints & LCDC_BIT_DMA_UN_INTS) {
        s_lcdc.udf_count++;   /* BRING-UP: count DMA FIFO underflows */
    }
    if ((ints & LCDC_BIT_FRM_START_INTS) && s_lcdc.vsync_sema) {
        rtos_sema_give(s_lcdc.vsync_sema);
    }
    LCDC_ClearINT(LCDC, ints);
}

/* Wait (interrupt-driven, CPU yielded) until a pending shadow-reload has been
 * latched by hardware — i.e. LCDC_BIT_VBR self-clears.  The frame-start IRQ posts
 * vsync_sema once per frame; we sleep on it and re-check VBR, so the CPU is free
 * for other tasks during the wait.  Bounded by FLIP_TIMEOUT_MS so a stalled or
 * disabled controller can never hang present().  No-op if no flip is pending. */
static void lcdc_wait_flip_latched(void)
{
    uint32_t deadline = (uint32_t)rtos_time_get_current_system_time_ms()
                        + CLAW_DISPLAY_LCDC_FLIP_TIMEOUT_MS;
    while (LCDC->LCDC_SHW_RLD_CFG & LCDC_BIT_VBR) {
        int32_t remain = (int32_t)(deadline
                         - (uint32_t)rtos_time_get_current_system_time_ms());
        if (remain <= 0) {
            break;   /* controller stalled — bail rather than hang */
        }
        /* Sleep until the frame-start IRQ wakes us, but cap the block short so
         * correctness never depends on that IRQ actually firing: VBR clears in
         * hardware at vblank regardless, and re-polling it every few ms (while
         * yielding the CPU) is guaranteed to observe it.  The IRQ, when it does
         * fire, just makes the wake snappier. */
        uint32_t wait_ms = (remain < (int32_t)CLAW_DISPLAY_LCDC_FLIP_POLL_MS)
                         ? (uint32_t)remain : CLAW_DISPLAY_LCDC_FLIP_POLL_MS;
        rtos_sema_take(s_lcdc.vsync_sema, wait_ms);
    }
}

/* Run the panel's 9-bit register-init sequence over mbed spi_api, then release
 * the SPI so its pins are free (they are NOT part of the RGB bus). */
static void run_spi_init(const lcdc_cfg_t *cfg)
{
    spi_t spi;
    spi.spi_idx = MBED_SPI1;   /* board wires the init SPI on SPI1 (PA_20/21/22) */

    /* mbed spi_init(obj, mosi, miso, sclk, ssel); miso unused → 0xFFFFFFFF (NC),
     * matching the SDK's panel_spi_init.c which passes the literal. */
    spi_init(&spi, (PinName)cfg->spi_mosi, (PinName)0xFFFFFFFF,
             (PinName)cfg->spi_sclk, (PinName)cfg->spi_cs);
    spi_frequency(&spi, CLAW_DISPLAY_LCDC_SPI_HZ);
    spi_format(&spi, 9, 3, 0);   /* 9-bit frame, SPI mode 3 (matches SDK) */

    const lcdc_spi_op_t *ops = cfg->panel->spi_ops;
    for (uint16_t i = 0; i < cfg->panel->spi_ops_count; i++) {
        switch (ops[i].op) {
            case LCDC_SPI_OP_CMD:
                spi_master_write(&spi, ops[i].val);          /* D/C = 0 */
                break;
            case LCDC_SPI_OP_DATA:
                spi_master_write(&spi, ops[i].val | BIT8);   /* D/C = 1 (9th bit) */
                break;
            case LCDC_SPI_OP_DELAY_MS:
                rtos_time_delay_ms(ops[i].val);
                break;
            default:
                break;
        }
    }

    spi_free(&spi);
}

/* Bring up the LCDC RGB controller against framebuffer `fb` for panel `p`. */
static void lcdc_controller_init(const lcdc_cfg_t *cfg, uint8_t *fb)
{
    const lcdc_panel_t *p = cfg->panel;

    /* Pinmux the 24 data lines + RGB sync/control lines.
     * hsync/vsync may be absent (0xFFFF) on DE-only panels such as T1720A. */
    for (int i = 0; i < 24; i++) {
        Pinmux_Config((u8)cfg->data[i], PINMUX_FUNCTION_LCD_D0 + (uint32_t)i);
    }
    if (cfg->hsync != 0xFFFF) {
        Pinmux_Config((u8)cfg->hsync, PINMUX_FUNCTION_LCD_RGB_HSYNC);
    }
    if (cfg->vsync != 0xFFFF) {
        Pinmux_Config((u8)cfg->vsync, PINMUX_FUNCTION_LCD_RGB_VSYNC);
    }
    Pinmux_Config((u8)cfg->dclk,  PINMUX_FUNCTION_LCD_RGB_DCLK);
    Pinmux_Config((u8)cfg->de,    PINMUX_FUNCTION_LCD_RGB_DE);

    LCDC_RccEnable();

    InterruptRegister((IRQ_FUN)lcdc_irq_handler, LCDC_IRQ, (u32)LCDC, INT_PRI_MIDDLE);
    InterruptEn(LCDC_IRQ, INT_PRI_MIDDLE);

    LCDC_RGBInitTypeDef rgb;
    LCDC_Cmd(LCDC, DISABLE);
    LCDC_RGBStructInit(&rgb);

    rgb.Panel_RgbTiming.RgbVsw = p->vsw;
    rgb.Panel_RgbTiming.RgbVbp = p->vbp;
    rgb.Panel_RgbTiming.RgbVfp = p->vfp;
    rgb.Panel_RgbTiming.RgbHsw = p->hsw;
    rgb.Panel_RgbTiming.RgbHbp = p->hbp;
    rgb.Panel_RgbTiming.RgbHfp = p->hfp;

    rgb.Panel_Init.IfWidth        = LCDC_RGB_IF_24_BIT;
    rgb.Panel_Init.ImgWidth       = p->w;
    rgb.Panel_Init.ImgHeight      = p->h;
    rgb.Panel_Init.InputFormat    = LCDC_INPUT_FORMAT_RGB565;
    rgb.Panel_Init.OutputFormat   = LCDC_OUTPUT_FORMAT_RGB888;
    rgb.Panel_Init.RGBRefreshFreq = p->refresh_freq;

    rgb.Panel_RgbTiming.Flags.RgbEnPolar =
        p->de_high ? LCDC_RGB_EN_PUL_HIGH_LEV_ACTIVE : LCDC_RGB_EN_PUL_LOW_LEV_ACTIVE;
    rgb.Panel_RgbTiming.Flags.RgbHsPolar =
        p->hs_low ? LCDC_RGB_HS_PUL_LOW_LEV_SYNC : LCDC_RGB_HS_PUL_HIGH_LEV_SYNC;
    rgb.Panel_RgbTiming.Flags.RgbVsPolar =
        p->vs_low ? LCDC_RGB_VS_PUL_LOW_LEV_SYNC : LCDC_RGB_VS_PUL_HIGH_LEV_SYNC;
    rgb.Panel_RgbTiming.Flags.RgbDclkActvEdge =
        p->dclk_falling ? LCDC_RGB_DCLK_FALLING_EDGE_FETCH : LCDC_RGB_DCLK_RISING_EDGE_FETCH;

    LCDC_RGBInit(LCDC, &rgb);

    LCDC_DMABurstSizeConfig(LCDC, CLAW_DISPLAY_LCDC_DMA_BURST);
    LCDC_DMAImgCfg(LCDC, (u32)fb);   /* single buffer: DMA base configured once */

    /* Frame-start IRQ drives the interrupt-based flip wait (present_full);
     * underflow IRQ stays on as a starved-DMA bring-up diagnostic. */
    LCDC_INTConfig(LCDC, LCDC_BIT_FRM_START_INTEN | LCDC_BIT_DMA_UN_INTEN, ENABLE);

    /* Order matters, matching the SDK bring-up path (lcdc_rgb.c): the DMA burst
     * size lives in LCDC_SHW_RLD_CFG (a shadow register) and the DMA base is
     * shadowed too, so BOTH must be latched by a ShadowReload BEFORE the scan
     * engine starts.  Enabling first would scan with an un-latched burst size
     * → chronic DMA FIFO underflow → regular vertical striping. */
    LCDC_ShadowReloadConfig(LCDC);
    LCDC_Cmd(LCDC, ENABLE);
}

/* ========================================================================= */
/* vtable                                                                     */
/* ========================================================================= */

/* Exact 16-bit halfword LVGL stores for an opaque colour in a NATIVE RGB565
 * canvas.  The LCDC input path is configured LCDC_INPUT_FORMAT_RGB565, which
 * reads native little-endian RGB565 from PSRAM — so NO byte swap here (unlike
 * the SPI backend, whose panel wants RGB565_SWAPPED wire order).  RGB565 halves
 * the per-frame framebuffer write/scan-out traffic vs XRGB8888, roughly halving
 * the CPU frame-prep time and lifting the vsync-locked framerate. */
static uint32_t lcdc_encode(lv_color_t c)
{
    return lv_color_to_u16(c);
}

/* Inverse of lcdc_encode(): native RGB565 → lv_color_t (5/6/5 → 8/8/8, high
 * bits replicated into the low bits so 0x1F→0xFF).  Used by the AA fringe
 * blend in fill_circles(). */
static lv_color_t lcdc_decode(uint32_t v)
{
    uint8_t r5 = (uint8_t)((v >> 11) & 0x1Fu);
    uint8_t g6 = (uint8_t)((v >> 5) & 0x3Fu);
    uint8_t b5 = (uint8_t)(v & 0x1Fu);
    return lv_color_make((uint8_t)((r5 << 3) | (r5 >> 2)),
                         (uint8_t)((g6 << 2) | (g6 >> 4)),
                         (uint8_t)((b5 << 3) | (b5 >> 2)));
}

static void lcdc_backlight(int on)
{
    if (s_lcdc.has_blk) {
        GPIO_WriteBit((u32)s_lcdc.blk_pin, on ? 1 : 0);
    }
}

/* Double-buffered page-flip (see display_backend.h double-buffer contract).
 * render_buf is the just-drawn BACK buffer; make it the scan-out source at the
 * next vertical blank, then wait for the swap to land so the common layer can
 * safely hand the CPU the now-off-screen buffer.
 *
 * Cache: amebadplus D-cache is write-through (MPU MAIR = NORMAL_WT; ameba_cache.h
 * "clean is not needed"), so CPU writes already reached PSRAM — this clean is
 * belt-and-suspenders against a future write-back MPU change.  The 0xFFFFFFFF
 * sentinel takes the clean-ALL-by-set/way path (~cache-size/32 lines), far
 * cheaper than a ranged clean over the ~0.9 MB frame (28800 lines).  Clean, not
 * CleanInvalidate: the DMA only READS, so nothing needs invalidating.
 *
 * Flip: LCDC_DMAImgCfg writes the SHADOW base; LCDC_ShadowReloadConfig sets VBR
 * so the hardware latches it during the next vblank and self-clears VBR when
 * done.  Spinning until VBR clears vsync-locks present (no tearing); the ms cap
 * only guards against a stalled/disabled controller so we never hang. */
/* Arm a flip = point the DMA shadow base at the just-drawn back buffer and set
 * VBR so hardware latches it at the next vblank.  Cache clean first: writes are
 * write-through today, but this is belt-and-suspenders + cheap (clean-all by
 * set/way).  Common to both buffering modes. */
static void lcdc_arm_flip(display_surface_t *s)
{
    DCache_Clean(0xFFFFFFFF, 0xFFFFFFFF);
    LCDC_DMAImgCfg(LCDC, (u32)(uintptr_t)s->render_buf);
    LCDC_ShadowReloadConfig(LCDC);
}

/* present_full — hand the just-drawn buffer to the scan-out DMA, tear-free.
 *
 * TWO buffering strategies, selected at RUNTIME by `s->buf_count` (defaults to
 * CLAW_DISPLAY_LCDC_BUF_COUNT from lcdc_init(), but a caller that only rotates
 * a SUBSET of the allocated buffers — e.g. `lvgl` mode, which hands LVGL just
 * buffers[0]/[1] and lets LVGL's own FULL-mode ping-pong drive the rotation
 * instead of this backend's round-robin — must override `s->buf_count` to
 * match how many buffers are ACTUALLY rotating, or the fire-and-forget
 * triple-buffer path below races LVGL's redraw against the still-in-flight
 * flip (visible as flicker/tearing on any full-screen redraw):
 *
 *   Triple-buffer (==3): arm this frame's flip and RETURN immediately — do NOT
 *     wait for it.  The wait we DO perform is for the PREVIOUS frame's flip, up
 *     front, and when the CPU is render-bound that has long since latched, so it
 *     is instant.  The common layer then hands the CPU the third buffer (freed
 *     by that previous flip), so it never blocks on the flip it just armed →
 *     frame time = max(render+push, refresh) with no vblank stall.
 *
 *   Double-buffer (==2): arm the flip, then wait for IT to latch before
 *     returning — with only two buffers the next drawing target is the buffer
 *     still being scanned, so we must block until this flip frees it.  Costs one
 *     vblank per frame but one less full framebuffer of PSRAM.
 *
 * Both waits are interrupt-driven (lcdc_wait_flip_latched → frame-start IRQ),
 * so the CPU is yielded to other FreeRTOS tasks rather than busy-spun. */
static void lcdc_present_full(display_surface_t *s)
{
    if (s->buf_count >= 3) {
        lcdc_wait_flip_latched();   /* previous flip; instant when render-bound */
        lcdc_arm_flip(s);           /* this frame's flip — fire and forget       */
    } else {
        lcdc_arm_flip(s);
        lcdc_wait_flip_latched();   /* block until this flip frees the other buf */
    }
}

/* Clean only the dirty rows.  Framebuffer rows are contiguous (stride = w*4),
 * so a rect maps to the byte span [y*stride, (y+h)*stride).  Coords are clamped
 * to the screen; an empty/off-screen rect is a no-op. */
static void lcdc_present_rect(display_surface_t *s, int rx, int ry, int rw, int rh)
{
    (void)rx; (void)rw;   /* full rows are cleaned; x-extent doesn't narrow it */
    int H = (int)s->h;

    if (ry < 0) { rh += ry; ry = 0; }
    if (ry >= H || rh <= 0) {
        return;
    }
    if (ry + rh > H) {
        rh = H - ry;
    }

    size_t stride = (size_t)s->w * (size_t)s->bpp;
    DCache_Clean((u32)(uintptr_t)(s->render_buf + (size_t)ry * stride),
                 (u32)((size_t)rh * stride));
}

/* Read device `dev_id`'s "interface" from cap_board_mgr; claim "lcdc"/"rgb". */
static int lcdc_probe(const char *dev_id)
{
    char  input[96];
    char *out = NULL;

    snprintf(input, sizeof(input), "{\"id\":\"%s\"}", dev_id);

    claw_cap_call_context_t ctx = { 0 };
    ctx.caller = CLAW_CAP_CALLER_INTERNAL;
    (void)claw_cap_call("board_get_device", input, &ctx, &out);
    if (!out) {
        return 0;
    }

    cJSON *root = cJSON_Parse(out);
    free(out);
    if (!root) {
        return 0;
    }

    /* board_get_device inlines the resolved interface as an OBJECT
     * ({"id","type","instance",...}); we key off its "type". */
    int claimed = 0;
    cJSON *iface = cJSON_GetObjectItem(root, "interface");
    if (iface && cJSON_IsObject(iface)) {
        cJSON *type = cJSON_GetObjectItem(iface, "type");
        if (type && cJSON_IsString(type) && type->valuestring) {
            claimed = (strcmp(type->valuestring, "lcdc") == 0) ||
                      (strcmp(type->valuestring, "rgb") == 0);
        }
    }
    cJSON_Delete(root);
    return claimed;
}

/* Teardown: LVGL display (also deletes canvas), LCDC off, free buffers, GPIOs
 * off, zero surface.  Idempotent — safe to call on a partially-built surface. */
static void lcdc_deinit(display_surface_t *s)
{
    if (s->disp) {
        lv_display_delete(s->disp);   /* also deletes screens + canvas */
    }

    LCDC_Cmd(LCDC, DISABLE);
    LCDC_INTConfig(LCDC, LCDC_BIT_FRM_START_INTEN | LCDC_BIT_DMA_UN_INTEN, DISABLE);
    InterruptDis(LCDC_IRQ);
    LCDC_DeInit(LCDC);

    /* Full POR-level reset of the LCDC module.  LCDC_Cmd(DISABLE)/LCDC_DeInit
     * only set INST_DIS + clear IRQs; on this SoC the "instant disable" internal-
     * state reset does NOT self-complete once the controller is starved, so the
     * scan-out FSM stays wedged.  A re-init then enables a controller that never
     * produces vblank → the page-flip's shadow-reload (VBR) never latches and the
     * panel stays blank on the 2nd+ init.  APBPeriph_LCDC's function bit is
     * "1=enable / 0=reset" (sysreg_lsys.h), so gating it here drops the module
     * into reset (and gates its clock); the next init's LCDC_RccEnable() brings
     * it back out of reset POR-clean, exactly like the very first init. */
    RCC_PeriphClockCmd(APBPeriph_LCDC, APBPeriph_LCDC_CLOCK, DISABLE);

    if (s_lcdc.has_blk) {
        GPIO_WriteBit((u32)s_lcdc.blk_pin, 0);
    }
    if (s_lcdc.has_power_en) {
        GPIO_WriteBit((u32)s_lcdc.power_en_pin, 0);
    }
    for (int i = 0; i < 3; i++) {
        if (s_lcdc.fb_raw[i]) {
            rtos_mem_free(s_lcdc.fb_raw[i]);
            s_lcdc.fb_raw[i] = NULL;
        }
    }
    if (s_lcdc.dbuf_raw) {
        rtos_mem_free(s_lcdc.dbuf_raw);
        s_lcdc.dbuf_raw = NULL;
    }
    if (s_lcdc.vsync_sema) {
        rtos_sema_delete(s_lcdc.vsync_sema);
        s_lcdc.vsync_sema = NULL;
    }
    s_lcdc.has_blk      = 0;
    s_lcdc.has_power_en = 0;
    memset(s, 0, sizeof(*s));
}

static int lcdc_init(const char *dev_id, display_surface_t *s,
                     char *err, size_t errlen)
{
    memset(s, 0, sizeof(*s));
    s_lcdc.fb_raw[0] = s_lcdc.fb_raw[1] = s_lcdc.fb_raw[2] = NULL;
    s_lcdc.dbuf_raw = NULL;
    s_lcdc.has_blk  = 0;

    lcdc_cfg_t cfg;
    if (load_lcdc_cfg(dev_id, &cfg, err, errlen) != 0) {
        return -1;   /* load_lcdc_cfg allocated nothing that outlives it */
    }

    const uint16_t W = cfg.panel->w;
    const uint16_t H = cfg.panel->h;
    if (W == 0 || H == 0 || W > CLAW_DISPLAY_LCDC_MAX_W || H > CLAW_DISPLAY_LCDC_MAX_H) {
        snprintf(err, errlen, "panel '%s' geometry %ux%u out of range",
                 cfg.panel->chip, W, H);
        return -1;
    }

    s_lcdc.blk_pin      = cfg.blk;
    s_lcdc.has_blk      = cfg.has_blk;
    s_lcdc.power_en_pin = cfg.power_en;
    s_lcdc.has_power_en = cfg.has_power_en;

    if (cfg.has_blk) {
        gpio_out_init(cfg.blk, 0);
    }

    /* Power-enable (T1720A style): drive HIGH, then let the panel stabilise. */
    if (cfg.has_power_en) {
        gpio_out_init(cfg.power_en, 1);
        rtos_time_delay_ms(10);
    }

    /* Reset sequence: high(h1) → low(l) → high(h2). Skipped when no reset pin. */
    if (cfg.has_reset) {
        gpio_out_init(cfg.reset, 1);
        GPIO_WriteBit((u32)cfg.reset, 1);
        rtos_time_delay_ms(cfg.panel->rst_h1_ms);
        GPIO_WriteBit((u32)cfg.reset, 0);
        rtos_time_delay_ms(cfg.panel->rst_l_ms);
        GPIO_WriteBit((u32)cfg.reset, 1);
        rtos_time_delay_ms(cfg.panel->rst_h2_ms);
    }

    /* Register-init over 9-bit SPI (frees the SPI pins when done). */
    if (cfg.panel->needs_spi_init) {
        run_spi_init(&cfg);
    }

    /* CLAW_DISPLAY_LCDC_BUF_COUNT RGB565 framebuffers (2 B/px), 64-byte aligned,
     * PSRAM-allocated (TYPE_DRAM) via alloc_aligned.  fb_raw[] holds the raw
     * pointer so deinit can free them.  All zeroed + flushed so the panel shows
     * black until the first present(). */
    const uint8_t nbuf = CLAW_DISPLAY_LCDC_BUF_COUNT;
    size_t fb_sz = (size_t)W * H * 2u;
    uint8_t *fb[3] = { NULL, NULL, NULL };
    for (uint8_t i = 0; i < nbuf; i++) {
        fb[i] = alloc_aligned(fb_sz, &s_lcdc.fb_raw[i]);
        if (!fb[i]) {
            snprintf(err, errlen, "out of memory: framebuffer %u (%u B)", i, (unsigned)fb_sz);
            lcdc_deinit(s);
            return -1;
        }
        memset(fb[i], 0, fb_sz);
    }
    DCache_Clean(0xFFFFFFFF, 0xFFFFFFFF);

    /* Diagnostic: show where the framebuffers landed and how much heap is left
     * so a placement/heap regression is visible on the console at bring-up. */
    DiagPrintf("[claw-lcdc] %ux%u x%u fb: %p %p %p  fb_sz=%u  free_heap=%u\n",
               (unsigned)W, (unsigned)H, (unsigned)nbuf,
               fb[0], fb[1], fb[2], (unsigned)fb_sz,
               (unsigned)rtos_mem_get_free_heap_size());

    /* IRQ-driven flip wait needs the semaphore live before the controller (and
     * its frame-done IRQ) start. */
    if (!s_lcdc.vsync_sema && rtos_sema_create_binary(&s_lcdc.vsync_sema) != RTK_SUCCESS) {
        snprintf(err, errlen, "vsync semaphore create failed");
        lcdc_deinit(s);
        return -1;
    }

    /* Bring-up: CPU draws fb[0] (back), DMA scans the LAST buffer (front); all
     * start black, so the panel shows black until the first present() flips the
     * base to fb[0].  The common layer then advances back_idx round-robin. */
    lcdc_controller_init(&cfg, fb[nbuf - 1]);

    /* LVGL display — we never lv_refr_now, but it must own a valid draw buffer.
     * The command-style drawing surface is a full-screen canvas whose buffer IS
     * the framebuffer, so AA primitives + direct writes land straight in fb. */
    lv_display_t *disp = lv_display_create(W, H);
    if (!disp) {
        snprintf(err, errlen, "lv_display_create failed");
        lcdc_deinit(s);
        return -1;
    }
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);

    size_t   dbuf_sz = (size_t)W * CLAW_DISPLAY_LCDC_DUMMY_LINES * 2u;
    uint8_t *dbuf = alloc_aligned(dbuf_sz, &s_lcdc.dbuf_raw);
    if (!dbuf) {
        snprintf(err, errlen, "out of memory: dummy draw buffer");
        lv_display_delete(disp);
        lcdc_deinit(s);
        return -1;
    }
    lv_display_set_buffers(disp, dbuf, NULL, (uint32_t)dbuf_sz,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_obj_t *canvas = lv_canvas_create(lv_screen_active());
    if (!canvas) {
        snprintf(err, errlen, "lv_canvas_create failed");
        lv_display_delete(disp);
        lcdc_deinit(s);
        return -1;
    }
    lv_canvas_set_buffer(canvas, fb[0], W, H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(canvas, 0, 0);
    lv_canvas_fill_bg(canvas, lv_color_hex(0x000000), LV_OPA_COVER);

    s->disp       = disp;
    s->canvas     = canvas;
    s->render_buf = fb[0];    /* == buffers[back_idx] */
    s->scratch    = NULL;
    s->scratch_sz = 0;
    s->w          = W;
    s->h          = H;
    s->bpp        = 2;

    /* Multi-buffer contract: CPU draws fb[0] (back), DMA scans fb[nbuf-1]. */
    for (uint8_t i = 0; i < nbuf; i++) {
        s->buffers[i] = fb[i];
    }
    s->buf_count  = nbuf;
    s->back_idx   = 0;

    /* Panel is scanning a black frame — safe to turn the backlight on now. */
    lcdc_backlight(1);
    return 0;
}

const display_backend_t display_backend_lcdc = {
    .name         = "lcdc-rgb",
    .probe        = lcdc_probe,
    .init         = lcdc_init,
    .deinit       = lcdc_deinit,
    .backlight    = lcdc_backlight,
    .present_full = lcdc_present_full,
    .present_rect = lcdc_present_rect,
    .encode       = lcdc_encode,
    .decode       = lcdc_decode,
};

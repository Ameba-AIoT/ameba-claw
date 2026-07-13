/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * display_backend_spi.c — the SPI/ST7789 backend for the "display" module.
 *
 * Implements the display_backend_t vtable (display_backend.h) on top of the
 * TX-only SPI transport in panel_spi_st7789.c.  This is the "write first, test
 * first" path.  Everything here is SPI-specific: board.json pin parsing, the
 * canvas/draw-buffer allocation ladder, lv_st7789_create wiring, the
 * RGB565_SWAPPED pixel encoding, and the CASET/RASET/RAMWR + TX-DMA present
 * fast path (phase2_present_fastpath.md §1).
 *
 * The common layer (display_lua.c) never sees any of this — see §2.6 of that
 * doc for the seam and the three assumptions an LCDC backend must override.
 */

#include "display_backend.h"
#include "panel_spi_st7789.h"

#include <string.h>
#include <stdio.h>

#include "os_wrapper.h"
#include "ameba_claw_defs.h"

#include "cJSON.h"
#include "claw_cap.h"

#include "lvgl.h"
#include "drivers/display/st7789/lv_st7789.h"

/* Raw (pre-alignment) allocation pointers, kept for free() in deinit. */
static struct {
    uint8_t *canvas_raw;
    uint8_t *draw_raw;
} s_spi;

/* ========================================================================= */
/* board.json pin parsing                                                     */
/* ========================================================================= */

/* Parse a "PA_0".."PC_8" pin name into a PinName-encoded uint16 (port<<5|num).
 * Returns 0xFFFF on failure. */
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

/* Fetch the display device descriptor from cap_board_mgr and fill `cfg`.
 * Returns 0 on success; on failure writes a message into errbuf. */
static int load_panel_cfg(const char *dev_id, panel_cfg_t *cfg,
                          char *errbuf, size_t errlen)
{
    char  input[96];
    char *out = NULL;

    snprintf(input, sizeof(input), "{\"id\":\"%s\"}", dev_id);

    claw_cap_call_context_t ctx = { 0 };
    ctx.caller = CLAW_CAP_CALLER_INTERNAL;
    (void)claw_cap_call("board_get_device", input, &ctx, &out);
    if (!out) {
        snprintf(errbuf, errlen, "board_get_device returned nothing");
        return -1;
    }

    cJSON *root = cJSON_Parse(out);
    free(out);
    if (!root) {
        snprintf(errbuf, errlen, "board_get_device: bad JSON");
        return -1;
    }

    cJSON *err = cJSON_GetObjectItem(root, "error");
    if (err && cJSON_IsString(err)) {
        snprintf(errbuf, errlen, "%s", err->valuestring);
        cJSON_Delete(root);
        return -1;
    }

    cJSON *params = cJSON_GetObjectItem(root, "params");
    if (!params) {
        snprintf(errbuf, errlen, "device '%s' has no params", dev_id);
        cJSON_Delete(root);
        return -1;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->spi_idx = 1;   /* board interface is spi1 */

    struct { const char *key; uint16_t *dst; } pins[] = {
        { "clk",  &cfg->clk_pin },
        { "mosi", &cfg->mosi_pin },
        { "cs",   &cfg->cs_pin },
        { "dc",   &cfg->dc_pin },
        { "rst",  &cfg->rst_pin },
        { "blk",  &cfg->blk_pin },
    };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        cJSON *p = cJSON_GetObjectItem(params, pins[i].key);
        if (!p || !cJSON_IsString(p)) {
            snprintf(errbuf, errlen, "device '%s' missing pin '%s'",
                     dev_id, pins[i].key);
            cJSON_Delete(root);
            return -1;
        }
        uint16_t pin = parse_pin(p->valuestring);
        if (pin == 0xFFFF) {
            snprintf(errbuf, errlen, "device '%s' bad pin '%s'=%s",
                     dev_id, pins[i].key, p->valuestring);
            cJSON_Delete(root);
            return -1;
        }
        *pins[i].dst = pin;
    }

    /* resolution "WxH" and color_format are optional; default to 240x240. */
    cfg->width  = CLAW_DISPLAY_DEFAULT_W;
    cfg->height = CLAW_DISPLAY_DEFAULT_H;
    cJSON *res = cJSON_GetObjectItem(params, "resolution");
    if (res && cJSON_IsString(res)) {
        int rw = 0, rh = 0;
        if (sscanf(res->valuestring, "%dx%d", &rw, &rh) == 2 &&
            rw > 0 && rh > 0 && rw <= 480 && rh <= 480) {
            cfg->width  = (uint16_t)rw;
            cfg->height = (uint16_t)rh;
        }
    }
    cfg->invert = 1;   /* ST7789 panels typically need INVON */
    cJSON *inv = cJSON_GetObjectItem(params, "invert");
    if (inv && cJSON_IsBool(inv)) {
        cfg->invert = cJSON_IsTrue(inv) ? 1 : 0;
    }

    cJSON_Delete(root);
    return 0;
}

/* Allocate `size` bytes 64-byte aligned; stores the raw allocation in *raw. */
static uint8_t *alloc_aligned(size_t size, uint8_t **raw)
{
    uint8_t *r = rtos_mem_malloc((u32)(size + 63U));
    if (!r) {
        return NULL;
    }
    *raw = r;
    return (uint8_t *)(((uintptr_t)r + 63U) & ~(uintptr_t)63U);
}

/* ========================================================================= */
/* vtable                                                                     */
/* ========================================================================= */

/* Exact 16-bit value LVGL stores for an opaque colour in a SWAPPED canvas. */
static uint32_t spi_encode(lv_color_t c)
{
    return lv_color_swap_16(lv_color_to_u16(c));
}

/* Inverse of spi_encode(): un-swap the bytes back to native RGB565, then expand
 * 5/6/5 → 8/8/8 (high bits replicated).  Used by the AA fringe blend in
 * fill_circles(). */
static lv_color_t spi_decode(uint32_t stored)
{
    uint16_t v  = lv_color_swap_16((uint16_t)stored);
    uint8_t  r5 = (uint8_t)((v >> 11) & 0x1Fu);
    uint8_t  g6 = (uint8_t)((v >> 5) & 0x3Fu);
    uint8_t  b5 = (uint8_t)(v & 0x1Fu);
    return lv_color_make((uint8_t)((r5 << 3) | (r5 >> 2)),
                         (uint8_t)((g6 << 2) | (g6 >> 4)),
                         (uint8_t)((b5 << 3) | (b5 >> 2)));
}

static void spi_backlight(int on)
{
    st7789_panel_backlight(on);
}

static void spi_present_full(display_surface_t *s)
{
    st7789_panel_present_full(s->render_buf, s->w, s->h);
}

/* Push only (x,y,w,h): full-width bands are contiguous in render_buf (no copy);
 * strided sub-rects are gathered into the scratch buffer then pushed once; a
 * rect too large for the scratch falls back to a full-frame push.  Coordinates
 * are clamped to the screen; an empty/off-screen rect is a no-op.  (phase2 §5) */
static void spi_present_rect(display_surface_t *s, int rx, int ry, int rw, int rh)
{
    int W = s->w, H = s->h;

    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rw <= 0 || rh <= 0 || rx >= W || ry >= H) {
        return;
    }
    if (rx + rw > W) rw = W - rx;
    if (ry + rh > H) rh = H - ry;

    uint8_t *canvas = s->render_buf;
    size_t   need   = (size_t)rw * (size_t)rh * 2u;

    if (rw == W) {
        st7789_panel_present_window(canvas + (size_t)ry * (size_t)W * 2u,
                                    0, (uint16_t)ry, (uint16_t)W, (uint16_t)rh);
    } else if (s->scratch && need <= s->scratch_sz) {
        uint8_t *scratch = s->scratch;
        for (int r = 0; r < rh; r++) {
            memcpy(scratch + (size_t)r * (size_t)rw * 2u,
                   canvas + (((size_t)(ry + r) * (size_t)W + rx) * 2u),
                   (size_t)rw * 2u);
        }
        st7789_panel_present_window(scratch, (uint16_t)rx, (uint16_t)ry,
                                    (uint16_t)rw, (uint16_t)rh);
    } else {
        st7789_panel_present_full(canvas, (uint16_t)W, (uint16_t)H);
    }
}

static void spi_deinit(display_surface_t *s)
{
    if (s->disp) {
        lv_display_delete(s->disp);   /* also deletes screens + canvas */
    }
    if (s_spi.draw_raw) {
        rtos_mem_free(s_spi.draw_raw);
        s_spi.draw_raw = NULL;
    }
    if (s_spi.canvas_raw) {
        rtos_mem_free(s_spi.canvas_raw);
        s_spi.canvas_raw = NULL;
    }
    st7789_panel_deinit();
    memset(s, 0, sizeof(*s));
}

static int spi_init(const char *dev_id, display_surface_t *s,
                    char *err, size_t errlen)
{
    memset(s, 0, sizeof(*s));
    s_spi.canvas_raw = NULL;
    s_spi.draw_raw   = NULL;

    panel_cfg_t cfg;
    if (load_panel_cfg(dev_id, &cfg, err, errlen) != 0) {
        return -1;   /* load_panel_cfg allocated nothing that outlives it */
    }

    if (st7789_panel_init(&cfg) != 0) {
        snprintf(err, errlen, "st7789_panel_init failed (SPI/GPIO)");
        return -1;
    }

    /* LVGL runs the ST7789 init command list NOW through st7789_panel_send_cmd. */
    lv_display_t *disp = lv_st7789_create(cfg.width, cfg.height, LV_LCD_FLAG_NONE,
                                          st7789_panel_send_cmd, st7789_panel_send_color);
    if (!disp) {
        st7789_panel_deinit();
        snprintf(err, errlen, "lv_st7789_create failed");
        return -1;
    }
    if (cfg.invert) {
        lv_st7789_set_invert(disp, true);
    }
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);

    /* Full-screen canvas buffer (mandatory 112KB for 240x240 RGB565). */
    size_t   cbuf_sz  = (size_t)cfg.width * cfg.height * 2u;
    uint8_t *cbuf = alloc_aligned(cbuf_sz, &s_spi.canvas_raw);
    if (!cbuf) {
        lv_display_delete(disp);
        st7789_panel_deinit();
        snprintf(err, errlen, "out of memory: canvas buffer");
        return -1;
    }

    /* Partial draw buffer ladder: start at 1/START_DIV, shrink to floor. */
    uint8_t *dbuf = NULL;
    size_t   dbuf_sz = 0;
    for (int div = CLAW_DISPLAY_DRAWBUF_START_DIV;
         div <= CLAW_DISPLAY_DRAWBUF_FLOOR_DIV; div *= 2) {
        int rows = cfg.height / div;
        if (rows < 1) rows = 1;
        dbuf_sz = (size_t)cfg.width * rows * 2u;
        dbuf = alloc_aligned(dbuf_sz, &s_spi.draw_raw);
        if (dbuf) break;
    }
    if (!dbuf) {
        rtos_mem_free(s_spi.canvas_raw);
        s_spi.canvas_raw = NULL;
        lv_display_delete(disp);
        st7789_panel_deinit();
        snprintf(err, errlen, "out of memory: draw buffer");
        return -1;
    }

    lv_display_set_buffers(disp, dbuf, NULL, (uint32_t)dbuf_sz,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* Canvas widget = the command-style drawing surface. */
    lv_obj_t *canvas = lv_canvas_create(lv_screen_active());
    if (!canvas) {
        rtos_mem_free(s_spi.draw_raw);
        s_spi.draw_raw = NULL;
        rtos_mem_free(s_spi.canvas_raw);
        s_spi.canvas_raw = NULL;
        lv_display_delete(disp);
        st7789_panel_deinit();
        snprintf(err, errlen, "lv_canvas_create failed");
        return -1;
    }
    lv_canvas_set_buffer(canvas, cbuf, cfg.width, cfg.height,
                         LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_obj_set_pos(canvas, 0, 0);
    lv_canvas_fill_bg(canvas, lv_color_hex(0x000000), LV_OPA_COVER);

    s->disp       = disp;
    s->canvas     = canvas;
    s->render_buf = cbuf;
    s->scratch    = dbuf;
    s->scratch_sz = dbuf_sz;
    s->w          = cfg.width;
    s->h          = cfg.height;
    s->bpp        = 2;

    /* Single-buffered: the synchronous CASET/RAMWR + TX-DMA present pushes the
     * whole buffer to the panel (no continuous scan-out, so no CPU/DMA tearing).
     * The common layer's swap is a no-op at buf_count==1.  Future speedup: an
     * ASYNC present + buf_count=2 would let the CPU render frame N+1 while the
     * SPI DMA transmits frame N — reusing the exact same swap policy. */
    s->buffers[0] = cbuf;
    s->buffers[1] = NULL;
    s->buf_count  = 1;
    s->back_idx   = 0;
    return 0;
}

/* Read device `dev_id`'s "interface" field from cap_board_mgr and report
 * whether it names an SPI transport.  This backend claims interfaces that
 * start with "spi" (e.g. "spi1").  Any error / missing field → decline (0). */
static int spi_probe(const char *dev_id)
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
            claimed = (strncmp(type->valuestring, "spi", 3) == 0);
        }
    }
    cJSON_Delete(root);
    return claimed;
}

const display_backend_t display_backend_spi = {
    .name         = "spi-st7789",
    .probe        = spi_probe,
    .init         = spi_init,
    .deinit       = spi_deinit,
    .backlight    = spi_backlight,
    .present_full = spi_present_full,
    .present_rect = spi_present_rect,
    .encode       = spi_encode,
    .decode       = spi_decode,
};

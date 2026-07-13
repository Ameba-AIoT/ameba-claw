/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * display_lua.c — the "display" Lua module: a command-style 2D canvas.
 *
 * This is the backend-AGNOSTIC common layer.  Everything panel-specific lives
 * behind the display_backend_t vtable (display_backend.h): SPI/ST7789 in
 * display_backend_spi.c (implemented), RGB/LCDC in display_backend_lcdc.c
 * (stub).  Porting to LCDC touches only the backend — see
 * design_spec/display/phase2_present_fastpath.md §2.6.
 *
 * Design: design_spec/display/{phase1_panel_hal.md, phase2_present_fastpath.md,
 * display_api_and_effort.md}.
 *   - The drawing surface is a full-screen lv_canvas widget the backend sets up.
 *     Anti-aliased primitives (circle/arc/line/triangle/text/rounded corners)
 *     map onto lv_draw_* into a canvas layer.
 *   - Opaque solid primitives (pixel/rect/clear/draw_points) take a direct-write
 *     fast path: they write the backend-encoded pixel straight into render_buf,
 *     which is BIT-IDENTICAL to LVGL's opaque fill (no mask ⇒ no AA) while
 *     skipping the whole draw-task machinery.  See phase2 §1.7.
 *   - present() = finish layer + backend->present_full (SPI: one full-screen
 *     CASET/RASET/RAMWR + TX-DMA, phase2 §1; synchronous — returns == on-screen).
 *   - Colours are 0xRRGGBB (24-bit); the backend's encode() turns them into the
 *     native render_buf pixel value.
 *   - Single owner (先到先得, no preemption); a __gc sentinel guarantees the
 *     panel is released even if the Lua job dies without calling deinit().
 */

#include "lua_module_display.h"
#include "display_backend.h"
#include "display_ownership.h"
#include "ameba_claw_defs.h"
#include "jpeg_hw.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "lauxlib.h"
#include "os_wrapper.h"

#include "lvgl.h"

/* ---- module state -------------------------------------------------------- */

#define DISPLAY_SENTINEL_MT "display.sentinel"

static struct {
    rtos_mutex_t              lock;
    disp_owner_mode_t         mode;        /* NONE / DISPLAY / LVGL              */
    uint32_t                  token;       /* owner token (monotonic; ABA-safe)*/
    uint32_t                  next_token;
    const display_backend_t  *be;          /* active panel backend             */
    display_surface_t         surf;        /* backend-filled drawing surface   */
    lv_layer_t                layer;
    uint8_t                   frame_active;
    uint8_t                   clip_active;
    lv_area_t                 clip;
    uint32_t                  last_present_ms;  /* wall time of last present()  */
    uint32_t                  last_render_ms;   /* flush_layer() AA raster cost */
} s_disp;

static bool s_lv_ready;   /* lv_init done once, never torn down (see phase1 §4) */
static disp_lvgl_teardown_fn s_lvgl_teardown_fn;   /* registered by lua_module_lvgl */

/* LVGL tick source.  We bypass the SDK's lv_port (framebuffer/VBlank model, see
 * phase1 §0), so we must feed LVGL a millisecond tick ourselves — otherwise the
 * delays in the ST7789 init command list (SLPOUT +120ms) spin forever. */
static uint32_t disp_tick_cb(void)
{
    return (uint32_t)rtos_time_get_current_system_time_ms();
}

/* ========================================================================= */
/* Ownership arbitration                                                      */
/* ========================================================================= */

/* Try to acquire the panel for DISP_OWNER_DISPLAY.  Returns a non-zero owner
 * token on success, 0 if the panel is already owned. */
static uint32_t own_acquire(void)
{
    return disp_own_acquire(DISP_OWNER_DISPLAY);
}

/* Tear down every display-mode resource.  Called with s_disp.lock held. */
static void own_teardown_locked(void)
{
    if (s_disp.mode == DISP_OWNER_LVGL) {
        /* lua_module_lvgl owns its own teardown (widget tree / indev / lv_timer
         * task); display_lua.c must not touch LVGL internals it never created.
         * Registered once by lua_module_lvgl_init() — see display_ownership.h. */
        if (s_lvgl_teardown_fn) {
            s_lvgl_teardown_fn();
        }
        return;
    }
    if (s_disp.frame_active && s_disp.surf.canvas) {
        /* best-effort: close the dangling layer so LVGL frees its tasks */
        lv_canvas_finish_layer(s_disp.surf.canvas, &s_disp.layer);
    }
    s_disp.frame_active = 0;
    if (s_disp.be) {
        s_disp.be->deinit(&s_disp.surf);   /* frees buffers + display + panel */
        s_disp.be = NULL;
    }
    s_disp.clip_active = 0;
}

/* Idempotent release: only the token holder tears down. */
static void own_release(uint32_t token)
{
    rtos_mutex_take(s_disp.lock, RTOS_MAX_DELAY);
    if (s_disp.mode != DISP_OWNER_NONE && s_disp.token == token) {
        own_teardown_locked();
        s_disp.mode = DISP_OWNER_NONE;
    }
    rtos_mutex_give(s_disp.lock);
}

/* ========================================================================= */
/* Shared arbiter API (display_ownership.h) — consumed by lua_module_lvgl    */
/* ========================================================================= */

uint32_t disp_own_acquire(disp_owner_mode_t mode)
{
    uint32_t tok = 0;
    rtos_mutex_take(s_disp.lock, RTOS_MAX_DELAY);
    if (s_disp.mode == DISP_OWNER_NONE) {
        s_disp.mode  = mode;
        s_disp.token = ++s_disp.next_token;
        tok = s_disp.token;
    }
    rtos_mutex_give(s_disp.lock);
    return tok;
}

void disp_own_release(uint32_t token)
{
    own_release(token);
}

disp_owner_mode_t disp_own_current_mode(void)
{
    disp_owner_mode_t m;
    rtos_mutex_take(s_disp.lock, RTOS_MAX_DELAY);
    m = s_disp.mode;
    rtos_mutex_give(s_disp.lock);
    return m;
}

void disp_lv_ensure_init(void)
{
    /* lv_init is one-way (creates the swdraw thread + LVGL singleton): once
     * ready it stays ready across failures of later steps, and across both
     * modes — see phase1 §4 / phase5 §5. */
    if (!s_lv_ready) {
        lv_init();
        lv_tick_set_cb(disp_tick_cb);
        s_lv_ready = true;
    }
}

void disp_set_lvgl_teardown_fn(disp_lvgl_teardown_fn fn)
{
    s_lvgl_teardown_fn = fn;
}

/* ---- __gc sentinel: guarantees release on Lua job death ------------------ */

static int sentinel_gc(lua_State *L)
{
    uint32_t *tok = (uint32_t *)luaL_checkudata(L, 1, DISPLAY_SENTINEL_MT);
    if (tok && *tok) {
        own_release(*tok);
    }
    return 0;
}

/* Push a sentinel userdata holding `token` and anchor it in the registry so it
 * is not GC'd before the job actually ends.  (Attach order: newuserdata →
 * setmetatable → luaL_ref, per phase1 §5.) */
static void sentinel_create(lua_State *L, uint32_t token)
{
    uint32_t *tok = (uint32_t *)lua_newuserdata(L, sizeof(uint32_t));
    *tok = token;
    luaL_getmetatable(L, DISPLAY_SENTINEL_MT);
    lua_setmetatable(L, -2);
    luaL_ref(L, LUA_REGISTRYINDEX);   /* keep a reference alive until job end */
}

/* ========================================================================= */
/* Argument helpers                                                           */
/* ========================================================================= */

static int check_int(lua_State *L, int idx)
{
    if (lua_type(L, idx) != LUA_TNUMBER) {
        return (int)luaL_error(L, "arg %d: expected number", idx);
    }
    /* Accept floats and floor them to whole pixels.  Physics/animation math is
     * naturally floating point; auto-flooring here spares callers a math.floor()
     * on every coordinate — the #1 cause of "expected integer" script crashes. */
    lua_Number v = lua_tonumber(L, idx);
    int i = (int)v;               /* truncates toward zero */
    if (v < 0 && (lua_Number)i != v) {
        i--;                      /* make it a true floor for negatives */
    }
    return i;
}

static lv_color_t check_color(lua_State *L, int idx)
{
    if (lua_type(L, idx) != LUA_TNUMBER || !lua_isinteger(L, idx)) {
        luaL_error(L, "arg %d: colour must be an integer 0xRRGGBB", idx);
    }
    uint32_t rgb = (uint32_t)lua_tointeger(L, idx) & 0xFFFFFFu;
    return lv_color_hex(rgb);
}

/* Ensure a display session is live; raises a Lua error otherwise. */
static void require_ready(lua_State *L)
{
    if (s_disp.mode != DISP_OWNER_DISPLAY || !s_disp.be || !s_disp.surf.canvas) {
        luaL_error(L, "display not initialised — call display.init(id) first");
    }
}

/* Lazily open a canvas layer for the current frame. */
static lv_layer_t *ensure_layer(lua_State *L)
{
    require_ready(L);
    if (!s_disp.frame_active) {
        lv_canvas_init_layer(s_disp.surf.canvas, &s_disp.layer);
        s_disp.frame_active = 1;
        if (s_disp.clip_active) {
            s_disp.layer._clip_area = s_disp.clip;
        }
    }
    return &s_disp.layer;
}

static void area_set_wh(lv_area_t *a, int x, int y, int w, int h)
{
    a->x1 = x;
    a->y1 = y;
    a->x2 = x + (w > 0 ? w - 1 : 0);
    a->y2 = y + (h > 0 ? h - 1 : 0);
}

/* ========================================================================= */
/* Direct-write fast path (opaque primitives) — phase2 §1.7                   */
/* ========================================================================= */
/*
 * For an opaque (mask-free, OPA_COVER) fill the LVGL software renderer just
 * stores the backend-encoded pixel into every covered slot of render_buf — no
 * anti-aliasing, no blending.  So for solid pixels / rects / clear we write
 * those values straight into render_buf and get a BIT-IDENTICAL result while
 * skipping the entire draw-task machinery.  AA shapes still go through LVGL.
 *
 * The write width is dispatched on surface.bpp so the same scheduling code
 * serves a 2-byte (RGB565) SPI backend and a future 4-byte LCDC backend without
 * edits — only the backend's encode()/bpp differ.
 */

/* Fill `n` u16s at `d` with `v`, 32-bit stores for the bulk.  Handles a
 * possibly-unaligned start (canvas is 64B aligned; odd x offsets are not). */
static void mem_fill16(uint16_t *d, size_t n, uint16_t v)
{
    if (n == 0) {
        return;
    }
    if (((uintptr_t)d & 3u) != 0u) {   /* align up to a 4-byte boundary */
        *d++ = v;
        n--;
    }
    uint32_t  v2  = ((uint32_t)v << 16) | (uint32_t)v;
    uint32_t *d32 = (uint32_t *)d;
    size_t    n2  = n >> 1;
    while (n2--) {
        *d32++ = v2;
    }
    if ((n & 1u) != 0u) {
        *(uint16_t *)d32 = v;
    }
}

/* Fill `n` consecutive pixels at byte pointer `dst` with encoded value `enc`. */
static void fill_span(uint8_t *dst, size_t n, uint32_t enc, uint8_t bpp)
{
    if (bpp == 2) {
        mem_fill16((uint16_t *)dst, n, (uint16_t)enc);
    } else {   /* bpp == 4 */
        uint32_t *d = (uint32_t *)dst;
        while (n--) {
            *d++ = enc;
        }
    }
}

/* Store one pixel at (x,y) into render_buf. */
static inline void put_px(uint8_t *base, int W, int x, int y, uint32_t enc, uint8_t bpp)
{
    if (bpp == 2) {
        ((uint16_t *)base)[(size_t)y * (size_t)W + (size_t)x] = (uint16_t)enc;
    } else {
        ((uint32_t *)base)[(size_t)y * (size_t)W + (size_t)x] = enc;
    }
}

/* Integer square root (floor) of a 64-bit value — the AA rasterisers are pure
 * fixed-point because this SoC's M33 has NO hardware FPU (softfloat sqrtf/÷ on
 * the per-pixel hot path would be far too slow; LVGL itself is all-integer).
 * Classic bit-by-bit restoring algorithm, ~one iteration per 2 result bits. */
static uint32_t isqrt64(uint64_t v)
{
    uint64_t x   = 0;
    uint64_t bit = (uint64_t)1 << 62;
    while (bit > v) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (v >= x + bit) {
            v -= x + bit;
            x  = (x >> 1) + bit;
        } else {
            x >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)x;
}

/* Read one native pixel from render_buf at (x,y). */
static inline uint32_t read_px(const uint8_t *base, int W, int x, int y, uint8_t bpp)
{
    if (bpp == 2) {
        return ((const uint16_t *)base)[(size_t)y * (size_t)W + (size_t)x];
    }
    return ((const uint32_t *)base)[(size_t)y * (size_t)W + (size_t)x];
}

/* Alpha-blend src over the stored pixel at (x,y): out = (src*a + dst*(256-a))>>8
 * per 8-bit channel.  Coverage `a` is in 0..256 (256 == fully opaque) so the mix
 * is a shift, never a divide — no FPU, no /255.  Read-modify-write via the
 * backend's decode()/encode(), so it stays format/byte-order agnostic.  Used
 * ONLY on AA fringe pixels (a few per row) — opaque interiors go through the
 * plain span-fill, so this cost never touches the bulk. */
static void blend_px(uint8_t *base, int W, int x, int y,
                     lv_color_t src, uint32_t src_enc, uint32_t a, uint8_t bpp)
{
    if (a == 0) {
        return;
    }
    if (a >= 256) {
        put_px(base, W, x, y, src_enc, bpp);
        return;
    }
    lv_color_t dst = s_disp.be->decode(read_px(base, W, x, y, bpp));
    uint32_t   ia  = 256u - a;
    lv_color_t out;
    out.red   = (uint8_t)(((uint32_t)src.red   * a + (uint32_t)dst.red   * ia) >> 8);
    out.green = (uint8_t)(((uint32_t)src.green * a + (uint32_t)dst.green * ia) >> 8);
    out.blue  = (uint8_t)(((uint32_t)src.blue  * a + (uint32_t)dst.blue  * ia) >> 8);
    put_px(base, W, x, y, s_disp.be->encode(out), bpp);
}

/* Flush any pending vector layer so subsequent direct writes composite in the
 * correct z-order: everything queued so far renders into render_buf first, then
 * the direct write lands on top.  The next vector draw lazily reopens a layer. */
static void flush_layer(void)
{
    if (s_disp.frame_active) {
        lv_canvas_finish_layer(s_disp.surf.canvas, &s_disp.layer);
        s_disp.frame_active = 0;
    }
}

/* Double-buffer swap (backend-agnostic policy — see display_backend.h): after a
 * present, the buffer just shown is now the front; hand the CPU the OTHER
 * buffer for the next frame and re-point the canvas + direct-write target at it.
 * No-op for single-buffered backends (buf_count < 2).  The backend's
 * present_full() has already made render_buf the on-screen frame AND waited for
 * the hand-off to be safe (LCDC: vblank latch), so the toggled buffer is free.
 *
 * NOTE: with page-flip there is no in-place partial update — the new back holds
 * an older frame's pixels, so callers must fully redraw each frame (clear + draw).
 * Incremental/partial-update drawing needs a single-buffered backend. */
static void swap_backbuffer(void)
{
    display_surface_t *s = &s_disp.surf;
    if (s->buf_count < 2) {
        return;
    }
    /* Round-robin to the next buffer (== the classic toggle for buf_count==2).
     * present_full() guarantees whichever buffer this lands on is free: with
     * triple-buffering it is the third buffer, released by the flip present()
     * already waited on, so the CPU can draw it without touching the just-armed
     * flip. */
    s->back_idx  = (uint8_t)((s->back_idx + 1u) % s->buf_count);
    s->render_buf = s->buffers[s->back_idx];

    lv_color_format_t cf = (s->bpp == 2) ? LV_COLOR_FORMAT_RGB565
                         : (s->bpp == 3) ? LV_COLOR_FORMAT_RGB888
                         :                 LV_COLOR_FORMAT_XRGB8888;
    lv_canvas_set_buffer(s->canvas, s->render_buf, s->w, s->h, cf);
}

/* Effective clip box for a direct write: the screen intersected with the active
 * clip rect — matching how LVGL clips a draw task to the layer's clip area. */
static void clip_bounds(int *x0, int *y0, int *x1, int *y1)
{
    *x0 = 0; *y0 = 0;
    *x1 = (int)s_disp.surf.w - 1; *y1 = (int)s_disp.surf.h - 1;
    if (s_disp.clip_active) {
        if (s_disp.clip.x1 > *x0) *x0 = s_disp.clip.x1;
        if (s_disp.clip.y1 > *y0) *y0 = s_disp.clip.y1;
        if (s_disp.clip.x2 < *x1) *x1 = s_disp.clip.x2;
        if (s_disp.clip.y2 < *y1) *y1 = s_disp.clip.y2;
    }
}

/* Direct solid fill of the rect (x,y,w,h), clamped to the clip box.  Bit-
 * identical to an opaque lv_draw_rect (no radius/border).  Caller must already
 * have flushed the vector layer. */
static void fill_rect_direct(int x, int y, int w, int h, uint32_t enc)
{
    int cx0, cy0, cx1, cy1;
    clip_bounds(&cx0, &cy0, &cx1, &cy1);
    int rx0 = x, ry0 = y, rx1 = x + w - 1, ry1 = y + h - 1;
    if (rx0 < cx0) rx0 = cx0;
    if (ry0 < cy0) ry0 = cy0;
    if (rx1 > cx1) rx1 = cx1;
    if (ry1 > cy1) ry1 = cy1;
    if (rx0 > rx1 || ry0 > ry1) {
        return;
    }
    uint8_t *base = s_disp.surf.render_buf;
    int      W    = (int)s_disp.surf.w;
    uint8_t  bpp  = s_disp.surf.bpp;
    size_t   rw   = (size_t)(rx1 - rx0 + 1);
    if (rx0 == 0 && rx1 == W - 1) {
        /* Full-width band: rows are contiguous — one fill. */
        fill_span(base + (size_t)ry0 * (size_t)W * bpp,
                  rw * (size_t)(ry1 - ry0 + 1), enc, bpp);
    } else {
        for (int yy = ry0; yy <= ry1; yy++) {
            fill_span(base + ((size_t)yy * (size_t)W + (size_t)rx0) * bpp,
                      rw, enc, bpp);
        }
    }
}

/* Anti-aliased solid circle written straight into render_buf — the batch
 * primitive behind fill_circles().  Keeps the fast-path win (no draw-task alloc,
 * no descriptor init, no layer) yet gets smooth edges: the O(r²) interior is a
 * plain opaque scanline span-fill (as cheap as the old non-AA path), and ONLY
 * the ~O(r) boundary annulus does per-pixel coverage blending.
 *
 * Coverage model (analytic, uniform quality all around the rim — unlike a
 * per-row horizontal-only fringe, which under-AAs the near-horizontal top and
 * bottom).  A pixel centre at distance d from (cx,cy) is fully inside when
 * d <= r-0.5, fully outside when d >= r+0.5, and partial in between with
 * alpha = clamp(r + 0.5 - d, 0, 1).  Per row we opaque-fill |dx| <= xi
 * (xi = floor(sqrt((r-0.5)²-dy²))) and blend the fringe out to
 * xo = floor(sqrt((r+0.5)²-dy²)).  Caller must have flushed the vector layer so
 * z-order is correct (prior AA under, later draws on top).
 *
 * All fixed-point (no FPU): the opaque band uses squared-radius compares, and
 * the fringe alpha is (r+0.5-d) in Q8 via isqrt64 — coverage a = thr_q8 - d·256
 * clamped to 0..256, d·256 = isqrt64((dx²+dy²)<<16). */
static void fill_circle_direct(int cx, int cy, int r, lv_color_t src)
{
    if (r < 0) {
        return;
    }
    uint8_t  bpp     = s_disp.surf.bpp;
    uint8_t *base    = s_disp.surf.render_buf;
    int      W       = (int)s_disp.surf.w;
    uint32_t src_enc = s_disp.be->encode(src);
    int      cx0, cy0, cx1, cy1;
    clip_bounds(&cx0, &cy0, &cx1, &cy1);
    if (r == 0) {   /* degenerate: a single opaque pixel (matches old behaviour) */
        if (cx >= cx0 && cx <= cx1 && cy >= cy0 && cy <= cy1) {
            put_px(base, W, cx, cy, src_enc, bpp);
        }
        return;
    }
    int rr_out = r * r + r;      /* floor((r+0.5)²) = r²+r (drop the .25) */
    int rr_in  = r * r - r;      /* floor((r-0.5)²) = r²-r                */
    int thr_q8 = r * 256 + 128;  /* (r+0.5)·256, the Q8 coverage origin   */
    for (int dy = -(r + 1); dy <= r + 1; dy++) {
        int yy = cy + dy;
        if (yy < cy0 || yy > cy1) {
            continue;
        }
        int dy2     = dy * dy;
        int rem_out = rr_out - dy2;
        if (rem_out < 0) {
            continue;   /* row lies entirely outside the circle */
        }
        int xo = (int)isqrt64((uint64_t)rem_out);
        int rem_in = rr_in - dy2;
        int xi = (rem_in >= 0) ? (int)isqrt64((uint64_t)rem_in) : -1;
        /* Opaque interior span [cx-xi, cx+xi]. */
        if (xi >= 0) {
            int rx0 = cx - xi, rx1 = cx + xi;
            if (rx0 < cx0) rx0 = cx0;
            if (rx1 > cx1) rx1 = cx1;
            if (rx0 <= rx1) {
                fill_span(base + ((size_t)yy * (size_t)W + (size_t)rx0) * bpp,
                          (size_t)(rx1 - rx0 + 1), src_enc, bpp);
            }
        }
        /* Fringe: integer |dx| in (xi, xo], both sides, coverage-blended.  The
         * +1 guards the isqrt floor; a<=0 pixels are skipped inside. */
        for (int dx = xi + 1; dx <= xo + 1; dx++) {
            uint32_t d2   = (uint32_t)(dx * dx + dy2);
            uint32_t d_q8 = isqrt64((uint64_t)d2 << 16);   /* d·256 */
            int      a    = thr_q8 - (int)d_q8;            /* (r+0.5-d)·256 */
            if (a <= 0) {
                continue;
            }
            if (a > 256) {
                a = 256;
            }
            int xr = cx + dx, xl = cx - dx;
            if (xr >= cx0 && xr <= cx1) {
                blend_px(base, W, xr, yy, src, src_enc, (uint32_t)a, bpp);
            }
            if (dx != 0 && xl >= cx0 && xl <= cx1) {
                blend_px(base, W, xl, yy, src, src_enc, (uint32_t)a, bpp);
            }
        }
    }
}

/* Floor half-width, at row offset `ey`, of an axis-aligned ellipse whose radii
 * in HALF-pixel units are Rx2/2 and Ry2/2 (i.e. Rx2 = 2·rx±k).  Returns -1 when
 * the row is past the ellipse's y-extent.  All integer (no FPU):
 *     x = Rx2·sqrt(Ry2² − 4·ey²) / (2·Ry2).
 * Used to bound the opaque interior (inner ellipse) and the fringe scan (outer
 * ellipse) of the AA ellipse rasterisers. */
static int ellipse_hw(int Rx2, int Ry2, int ey)
{
    long long t = (long long)Ry2 * Ry2 - 4LL * ey * ey;
    if (t <= 0) {
        return -1;
    }
    return (int)(((long long)Rx2 * isqrt64((uint64_t)t)) / (2LL * Ry2));
}

/* Anti-aliased axis-aligned solid ellipse written straight into render_buf —
 * the batch primitive behind fill_ellipses().  Same fast-path structure as
 * fill_circle_direct (opaque scanline interior, coverage-blended rim only); an
 * ellipse with rx==ry reduces to exactly the circle case.  All fixed-point
 * (no FPU): with A = dx²ry² + dy²rx², B = dx²ry⁴ + dy²rx⁴, P = rx·ry, the signed
 * pixel distance to the rim is sd = √A·(√A − P)/√B (√ via isqrt64), computed in
 * Q8 so coverage a = 128 − sd·256 clamped to 0..256.
 *
 * Coverage uses the first-order implicit-distance estimate for the ellipse
 * F(ex,ey) = (ex/rx)² + (ey/ry)² = 1.  With q = sqrt((ex/rx)²+(ey/ry)²) (==1 on
 * the boundary) the signed pixel distance to the rim is
 *     sd ≈ (q - 1)·q / sqrt((ex/rx²)² + (ey/ry²)²),
 * and coverage = clamp(0.5 - sd, 0, 1) — 1 well inside, 0 well outside, a smooth
 * ramp across the ~1px rim.  (For rx==ry this simplifies to r+0.5-d, i.e. the
 * fill_circle_direct formula, so the two primitives are visually consistent.)
 * Per row we opaque-fill the inner ellipse (rx-0.5, ry-0.5) and blend the band
 * out to the outer ellipse (rx+0.5, ry+0.5); the band bound is approximate but
 * only decides which pixels are evaluated — the accurate formula supplies the
 * alpha, so a slightly wide band just costs a few cov==0/1 pixels.  Caller must
 * have flushed the vector layer so z-order is correct. */
static void fill_ellipse_direct(int cx, int cy, int rx, int ry, lv_color_t src)
{
    if (rx < 0 || ry < 0) {
        return;
    }
    uint8_t  bpp     = s_disp.surf.bpp;
    uint8_t *base    = s_disp.surf.render_buf;
    int      W       = (int)s_disp.surf.w;
    uint32_t src_enc = s_disp.be->encode(src);
    int      cx0, cy0, cx1, cy1;
    clip_bounds(&cx0, &cy0, &cx1, &cy1);

    if (rx == 0 && ry == 0) {   /* degenerate: single opaque pixel */
        if (cx >= cx0 && cx <= cx1 && cy >= cy0 && cy <= cy1) {
            put_px(base, W, cx, cy, src_enc, bpp);
        }
        return;
    }
    if (rx == 0 || ry == 0) {   /* degenerate axis: a 1px sliver, opaque (no AA) */
        int x0 = cx - rx, x1 = cx + rx, y0 = cy - ry, y1 = cy + ry;
        if (x0 < cx0) x0 = cx0;
        if (x1 > cx1) x1 = cx1;
        if (y0 < cy0) y0 = cy0;
        if (y1 > cy1) y1 = cy1;
        for (int yy = y0; yy <= y1 && x0 <= x1; yy++) {
            fill_span(base + ((size_t)yy * (size_t)W + (size_t)x0) * bpp,
                      (size_t)(x1 - x0 + 1), src_enc, bpp);
        }
        return;
    }

    long long rx2 = (long long)rx * rx;
    long long ry2 = (long long)ry * ry;
    long long rx4 = rx2 * rx2, ry4 = ry2 * ry2;
    long long P   = (long long)rx * ry;

    for (int ey = -(ry + 1); ey <= ry + 1; ey++) {
        int yy = cy + ey;
        if (yy < cy0 || yy > cy1) {
            continue;
        }
        int xo = ellipse_hw(2 * rx + 1, 2 * ry + 1, ey);   /* outer band bound */
        if (xo < 0) {
            continue;   /* row lies past the outer ellipse's y-extent */
        }
        /* Inner opaque half-width (fully-covered pixels on this row). */
        int xi = (rx >= 1 && ry >= 1) ? ellipse_hw(2 * rx - 1, 2 * ry - 1, ey) : -1;
        if (xi >= 0) {
            int rxx0 = cx - xi, rxx1 = cx + xi;
            if (rxx0 < cx0) rxx0 = cx0;
            if (rxx1 > cx1) rxx1 = cx1;
            if (rxx0 <= rxx1) {
                fill_span(base + ((size_t)yy * (size_t)W + (size_t)rxx0) * bpp,
                          (size_t)(rxx1 - rxx0 + 1), src_enc, bpp);
            }
        }
        /* Fringe band [xi+1, xo+1] on both sides, coverage-blended (Q8). */
        long long ey2 = (long long)ey * ey;
        for (int dx = xi + 1; dx <= xo + 1; dx++) {
            long long dx2 = (long long)dx * dx;
            uint32_t  sA  = isqrt64((uint64_t)(dx2 * ry2 + ey2 * rx2));   /* √A */
            uint32_t  sB  = isqrt64((uint64_t)(dx2 * ry4 + ey2 * rx4));   /* √B */
            if (sB == 0) {
                continue;   /* centre pixel: no defined rim distance */
            }
            long long sd_q8 = (long long)sA * ((long long)sA - P) * 256 / (long long)sB;
            int       a     = 128 - (int)sd_q8;   /* (0.5 - sd)·256 */
            if (a <= 0) {
                continue;
            }
            if (a > 256) {
                a = 256;
            }
            int xr = cx + dx, xl = cx - dx;
            if (xr >= cx0 && xr <= cx1) {
                blend_px(base, W, xr, yy, src, src_enc, (uint32_t)a, bpp);
            }
            if (dx != 0 && xl >= cx0 && xl <= cx1) {
                blend_px(base, W, xl, yy, src, src_enc, (uint32_t)a, bpp);
            }
        }
    }
}

/* Anti-aliased axis-aligned ellipse OUTLINE, stroke width `width` px, written
 * straight into render_buf (the ellipse analogue of draw_circle, which has no
 * LVGL ellipse primitive to lean on).  Reuses the same signed-distance estimate
 * as fill_ellipse_direct: sd = √A·(√A − P)/√B  (A = dx²ry² + dy²rx²,
 * B = dx²ry⁴ + dy²rx⁴, P = rx·ry; all integer via isqrt64).  A stroke centred on
 * the boundary covers pixels with |sd| <= width/2, anti-aliased at both edges:
 * coverage a = (0.5 + width/2 − |sd|)·256 clamped to 0..256 (width 1 → the
 * classic 1px AA curve).  Only the ring band is scanned — the interior hole
 * (inside rx−w/2, ry−w/2) is skipped.  Caller must have flushed the vector
 * layer so z-order is correct. */
static void draw_ellipse_direct(int cx, int cy, int rx, int ry, lv_color_t src, int width)
{
    if (rx < 0 || ry < 0) {
        return;
    }
    if (width < 1) {
        width = 1;
    }
    uint8_t  bpp     = s_disp.surf.bpp;
    uint8_t *base    = s_disp.surf.render_buf;
    int      W       = (int)s_disp.surf.w;
    uint32_t src_enc = s_disp.be->encode(src);
    int      cx0, cy0, cx1, cy1;
    clip_bounds(&cx0, &cy0, &cx1, &cy1);

    long long rx2 = (long long)rx * rx;
    long long ry2 = (long long)ry * ry;
    long long rx4 = rx2 * rx2, ry4 = ry2 * ry2;
    long long P   = (long long)rx * ry;
    int       thr = 128 + width * 128;   /* (0.5 + width/2)·256 */
    int       eylim = ry + width / 2 + 2;

    for (int ey = -eylim; ey <= eylim; ey++) {
        int yy = cy + ey;
        if (yy < cy0 || yy > cy1) {
            continue;
        }
        int xo = ellipse_hw(2 * rx + width + 1, 2 * ry + width + 1, ey);   /* outer edge */
        if (xo < 0) {
            continue;
        }
        /* Skip the fully-interior hole (inside rx−w/2−0.5). */
        int hx2 = 2 * rx - width - 1, hy2 = 2 * ry - width - 1;
        int xh  = (hx2 > 0 && hy2 > 0) ? ellipse_hw(hx2, hy2, ey) : -1;
        long long ey2 = (long long)ey * ey;
        for (int dx = xh + 1; dx <= xo + 1; dx++) {
            long long dx2 = (long long)dx * dx;
            uint32_t  sB  = isqrt64((uint64_t)(dx2 * ry4 + ey2 * rx4));   /* √B */
            if (sB == 0) {
                continue;   /* centre: not on any outline */
            }
            uint32_t  sA    = isqrt64((uint64_t)(dx2 * ry2 + ey2 * rx2)); /* √A */
            long long sd_q8 = (long long)sA * ((long long)sA - P) * 256 / (long long)sB;
            int       ad    = (int)(sd_q8 < 0 ? -sd_q8 : sd_q8);         /* |sd|·256 */
            int       a     = thr - ad;
            if (a <= 0) {
                continue;
            }
            if (a > 256) {
                a = 256;
            }
            int xr = cx + dx, xl = cx - dx;
            if (xr >= cx0 && xr <= cx1) {
                blend_px(base, W, xr, yy, src, src_enc, (uint32_t)a, bpp);
            }
            if (dx != 0 && xl >= cx0 && xl <= cx1) {
                blend_px(base, W, xl, yy, src, src_enc, (uint32_t)a, bpp);
            }
        }
    }
}

/* Opaque solid rect, dispatched to preserve z-order WITHOUT an extra flush:
 *   - No layer open  → direct write (the fast path; typical "solid background").
 *   - Layer open      → enqueue an opaque, radius-0, border-less lv_draw_rect
 *     onto the SAME layer.  This is BIT-IDENTICAL to the direct write (phase2
 *     §1.7: opaque + no mask ⇒ LVGL just stores encode(c)), yet it composites in
 *     script order at the single present()-time finish, so interleaving solid
 *     fills with AA shapes no longer forces one flush per solid primitive.
 * Crucially the layer-open branch does NOT touch render_buf directly, so it
 * never races the swdraw thread — it only appends to the task queue, exactly
 * like the AA primitives.  (See phase2 §2.6 guardrails.) */
static void solid_fill(int x, int y, int w, int h, lv_color_t c)
{
    if (s_disp.frame_active) {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_opa   = LV_OPA_COVER;
        d.bg_color = c;
        lv_area_t a;
        area_set_wh(&a, x, y, w, h);
        lv_draw_rect(&s_disp.layer, &d, &a);
    } else {
        fill_rect_direct(x, y, w, h, s_disp.be->encode(c));
    }
}

/* ========================================================================= */
/* Lifecycle                                                                  */
/* ========================================================================= */

static int l_init(lua_State *L)
{
    const char *dev_id = luaL_checkstring(L, 1);

    /* Acquire ownership first so a second init() fails fast without touching a
     * live owner's resources. */
    uint32_t token = own_acquire();
    if (token == 0) {
        lua_pushnil(L);
        lua_pushstring(L, "display already in use by another session");
        return 2;
    }

    disp_lv_ensure_init();

    /* Select the backend by asking each to probe() the device descriptor's
     * `interface` field; first to claim it wins (SPI: "spi*", LCDC: "lcdc"/
     * "rgb").  Keeps all board/cJSON knowledge in the backends. */
    static const display_backend_t *const backends[] = {
        &display_backend_spi,
        &display_backend_lcdc,
    };
    const display_backend_t *be = NULL;
    for (size_t i = 0; i < sizeof(backends) / sizeof(backends[0]); i++) {
        if (backends[i]->probe && backends[i]->probe(dev_id)) {
            be = backends[i];
            break;
        }
    }
    if (!be) {
        own_release(token);
        lua_pushnil(L);
        lua_pushfstring(L, "no display backend for device '%s'", dev_id);
        return 2;
    }

    char errbuf[96];
    if (be->init(dev_id, &s_disp.surf, errbuf, sizeof(errbuf)) != 0) {
        own_release(token);   /* be still NULL → teardown skips backend deinit */
        lua_pushnil(L);
        lua_pushstring(L, errbuf);
        return 2;
    }
    s_disp.be           = be;
    s_disp.frame_active = 0;
    s_disp.clip_active  = 0;

    sentinel_create(L, token);   /* __gc fallback release */

    lua_pushboolean(L, 1);
    return 1;
}

static int l_deinit(lua_State *L)
{
    (void)L;
    /* Guard against releasing a token this module never acquired: if the
     * arbiter currently belongs to the `lvgl` mode, display.deinit() must be
     * a no-op (idempotent releases elsewhere in this file assume the token
     * they hold really is theirs). */
    if (s_disp.mode == DISP_OWNER_DISPLAY) {
        own_release(s_disp.token);
    }
    return 0;
}

static int l_backlight(lua_State *L)
{
    require_ready(L);
    int on;
    if (lua_type(L, 1) == LUA_TBOOLEAN) {
        on = lua_toboolean(L, 1);
    } else {
        on = check_int(L, 1) != 0;
    }
    s_disp.be->backlight(on);
    return 0;
}

/* ========================================================================= */
/* Frame lifecycle                                                            */
/* ========================================================================= */

static int l_begin_frame(lua_State *L)
{
    require_ready(L);
    int clear = 1;
    uint32_t color = 0x000000;
    if (lua_type(L, 1) == LUA_TTABLE) {
        lua_getfield(L, 1, "clear");
        if (!lua_isnil(L, -1)) clear = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 1, "color");
        if (lua_isinteger(L, -1)) color = (uint32_t)lua_tointeger(L, -1) & 0xFFFFFFu;
        lua_pop(L, 1);
    }
    if (clear) {
        /* Solid full-screen clear (bit-identical to LVGL; no flush — solid_fill
         * direct-writes when idle, else enqueues in z-order). */
        solid_fill(0, 0, s_disp.surf.w, s_disp.surf.h, lv_color_hex(color));
    }
    return 0;
}

static int l_present(lua_State *L)
{
    require_ready(L);
    /* Split timing (profiling): flush_layer() is where all queued AA draw tasks
     * are actually rasterised into render_buf (blocking) — usually the real
     * bottleneck; present_full() is the DMA push + vsync wait (≈15ms ceiling).
     * Measuring them separately tells CPU-bound (render) from vsync-bound (push).
     * Exposed as render_ms() and frame_ms()/fps() respectively. */
    uint32_t tr = (uint32_t)rtos_time_get_current_system_time_ms();
    flush_layer();
    uint32_t t0 = (uint32_t)rtos_time_get_current_system_time_ms();
    s_disp.last_render_ms = t0 - tr;
    /* Fast path (phase2 §1): render_buf is already the final full frame — the
     * backend pushes it in one framing + DMA, bypassing LVGL's partial flush. */
    s_disp.be->present_full(&s_disp.surf);
    swap_backbuffer();   /* double-buffered backends: hand CPU the off-screen buf */
    s_disp.last_present_ms = (uint32_t)rtos_time_get_current_system_time_ms() - t0;
    return 0;
}

static int l_present_full(lua_State *L)
{
    return l_present(L);
}

/* d.present_rect(x,y,w,h) — push only the given rectangle (phase2 §5).  The
 * backend clamps to the screen and gathers/pushes as appropriate. */
static int l_present_rect(lua_State *L)
{
    require_ready(L);
    int rx = check_int(L, 1), ry = check_int(L, 2);
    int rw = check_int(L, 3), rh = check_int(L, 4);

    flush_layer();
    uint32_t t0 = (uint32_t)rtos_time_get_current_system_time_ms();
    if (s_disp.surf.buf_count >= 2) {
        /* Page-flipped backends cannot partial-update a single scanned buffer:
         * present the whole (fully-redrawn) back buffer and swap.  The rect is
         * advisory only here — see swap_backbuffer()'s full-redraw note. */
        s_disp.be->present_full(&s_disp.surf);
        swap_backbuffer();
    } else {
        s_disp.be->present_rect(&s_disp.surf, rx, ry, rw, rh);
    }
    s_disp.last_present_ms = (uint32_t)rtos_time_get_current_system_time_ms() - t0;
    return 0;
}

/* d.frame_ms() -> milliseconds the last present() push took (DMA + vsync). */
static int l_frame_ms(lua_State *L)
{
    lua_pushinteger(L, s_disp.last_present_ms);
    return 1;
}

/* d.render_ms() -> milliseconds the last flush_layer() AA rasterisation took.
 * This is the CPU-side cost of turning all queued draw tasks into pixels; if it
 * dwarfs frame_ms() the frame is CPU/render-bound, not vsync-bound. */
static int l_render_ms(lua_State *L)
{
    lua_pushinteger(L, s_disp.last_render_ms);
    return 1;
}

/* d.fps() -> frames per second derived from the last present().  0 before the
 * first present or if a present was instant. */
static int l_fps(lua_State *L)
{
    if (s_disp.last_present_ms > 0) {
        lua_pushinteger(L, 1000 / s_disp.last_present_ms);
    } else {
        lua_pushinteger(L, 0);
    }
    return 1;
}

static int l_end_frame(lua_State *L)
{
    (void)L;
    return 0;   /* reserved no-op */
}

/* ========================================================================= */
/* Drawing primitives                                                         */
/* ========================================================================= */

static int l_clear(lua_State *L)
{
    require_ready(L);
    lv_color_t c = check_color(L, 1);
    solid_fill(0, 0, s_disp.surf.w, s_disp.surf.h, c);
    return 0;
}

static int l_fill_rect(lua_State *L)
{
    require_ready(L);
    int x = check_int(L, 1), y = check_int(L, 2);
    int w = check_int(L, 3), h = check_int(L, 4);
    lv_color_t c = check_color(L, 5);
    solid_fill(x, y, w, h, c);
    return 0;
}

static int l_draw_rect(lua_State *L)
{
    lv_layer_t *layer = ensure_layer(L);
    int x = check_int(L, 1), y = check_int(L, 2);
    int w = check_int(L, 3), h = check_int(L, 4);
    lv_color_t c = check_color(L, 5);
    int thick = lua_isinteger(L, 6) ? (int)lua_tointeger(L, 6) : 1;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_TRANSP;
    d.border_opa = LV_OPA_COVER;
    d.border_color = c;
    d.border_width = thick;
    lv_area_t a;
    area_set_wh(&a, x, y, w, h);
    lv_draw_rect(layer, &d, &a);
    return 0;
}

static int l_fill_round_rect(lua_State *L)
{
    require_ready(L);
    int x = check_int(L, 1), y = check_int(L, 2);
    int w = check_int(L, 3), h = check_int(L, 4);
    int r = check_int(L, 5);
    lv_color_t c = check_color(L, 6);
    if (r <= 0) {
        /* No corner rounding → a plain opaque rect: solid_fill (direct or
         * z-order enqueue, no flush). */
        solid_fill(x, y, w, h, c);
        return 0;
    }
    lv_layer_t *layer = ensure_layer(L);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER;
    d.bg_color = c;
    d.radius = r;
    lv_area_t a;
    area_set_wh(&a, x, y, w, h);
    lv_draw_rect(layer, &d, &a);
    return 0;
}

static int l_draw_round_rect(lua_State *L)
{
    lv_layer_t *layer = ensure_layer(L);
    int x = check_int(L, 1), y = check_int(L, 2);
    int w = check_int(L, 3), h = check_int(L, 4);
    int r = check_int(L, 5);
    lv_color_t c = check_color(L, 6);
    int thick = lua_isinteger(L, 7) ? (int)lua_tointeger(L, 7) : 1;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_TRANSP;
    d.radius = r;
    d.border_opa = LV_OPA_COVER;
    d.border_color = c;
    d.border_width = thick;
    lv_area_t a;
    area_set_wh(&a, x, y, w, h);
    lv_draw_rect(layer, &d, &a);
    return 0;
}

static int l_draw_pixel(lua_State *L)
{
    require_ready(L);
    int x = check_int(L, 1), y = check_int(L, 2);
    lv_color_t c = check_color(L, 3);
    flush_layer();
    int x0, y0, x1, y1;
    clip_bounds(&x0, &y0, &x1, &y1);
    if (x < x0 || x > x1 || y < y0 || y > y1) {
        return 0;
    }
    put_px(s_disp.surf.render_buf, (int)s_disp.surf.w, x, y,
           s_disp.be->encode(c), s_disp.surf.bpp);
    return 0;
}

/* draw_points(tbl [, color]) — plot many single pixels in one call, writing
 * straight into render_buf (bit-identical to N draw_pixel calls but with one
 * Lua↔C crossing and one layer flush for the whole batch).  `tbl` is a flat
 * array; with a `color` arg it is {x1,y1, x2,y2, …} (uniform colour), otherwise
 * {x1,y1,c1, x2,y2,c2, …} (per-point 0xRRGGBB).  Off-screen points are skipped. */
static int l_draw_points(lua_State *L)
{
    require_ready(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    int      uniform = (lua_type(L, 2) == LUA_TNUMBER);
    uint32_t ucol    = 0;
    if (uniform) {
        ucol = s_disp.be->encode(lv_color_hex((uint32_t)lua_tointeger(L, 2) & 0xFFFFFFu));
    }
    int stride = uniform ? 2 : 3;

    flush_layer();
    int x0, y0, x1, y1;
    clip_bounds(&x0, &y0, &x1, &y1);
    uint8_t     *base = s_disp.surf.render_buf;
    int          W    = (int)s_disp.surf.w;
    uint8_t      bpp  = s_disp.surf.bpp;
    lua_Integer  n    = (lua_Integer)luaL_len(L, 1);

    for (lua_Integer i = 1; i + (stride - 1) <= n; i += stride) {
        lua_rawgeti(L, 1, i);
        int px = (int)lua_tonumber(L, -1);
        lua_rawgeti(L, 1, i + 1);
        int py = (int)lua_tonumber(L, -1);
        uint32_t col;
        if (uniform) {
            col = ucol;
            lua_pop(L, 2);
        } else {
            lua_rawgeti(L, 1, i + 2);
            col = s_disp.be->encode(lv_color_hex((uint32_t)lua_tointeger(L, -1) & 0xFFFFFFu));
            lua_pop(L, 3);
        }
        if (px < x0 || px > x1 || py < y0 || py > y1) {
            continue;
        }
        put_px(base, W, px, py, col, bpp);
    }
    return 0;
}

static int l_draw_line(lua_State *L)
{
    lv_layer_t *layer = ensure_layer(L);
    int x0 = check_int(L, 1), y0 = check_int(L, 2);
    int x1 = check_int(L, 3), y1 = check_int(L, 4);
    lv_color_t c = check_color(L, 5);
    int width = lua_isinteger(L, 6) ? (int)lua_tointeger(L, 6) : 1;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = c;
    d.width = width;
    d.p1.x = x0; d.p1.y = y0;
    d.p2.x = x1; d.p2.y = y1;
    lv_draw_line(layer, &d);
    return 0;
}

/* fill_circles(tbl) — draw one OR many anti-aliased solid circles in a single
 * Lua↔C crossing via the direct-write fast path.  `tbl` is a flat array
 * {cx1,cy1,r1,color1, cx2,cy2,r2,color2, …} (4 numbers per circle, color is
 * 0xRRGGBB).  This is the ONLY circle-fill primitive: one circle or a thousand
 * take the same call.  A single flush_layer() for the whole batch composites
 * prior queued AA under the circles; later AA draws land on top.
 *
 * Versus routing each circle through lv_draw_rect(radius=CIRCLE): same smooth
 * edges (see fill_circle_direct's coverage model), but it removes N draw-task
 * allocs + descriptor inits + N-1 Lua crossings — the interior is a bare
 * span-fill and only the rim is blended, so cost scales with area+perimeter,
 * not with LVGL's per-task overhead. */
static int l_fill_circles(lua_State *L)
{
    require_ready(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    flush_layer();   /* composite everything queued so far, once, under the batch */
    lua_Integer n = (lua_Integer)luaL_len(L, 1);
    for (lua_Integer i = 1; i + 3 <= n; i += 4) {
        lua_rawgeti(L, 1, i);
        int cx = (int)lua_tonumber(L, -1);
        lua_rawgeti(L, 1, i + 1);
        int cy = (int)lua_tonumber(L, -1);
        lua_rawgeti(L, 1, i + 2);
        int r = (int)lua_tonumber(L, -1);
        lua_rawgeti(L, 1, i + 3);
        lv_color_t c = lv_color_hex((uint32_t)lua_tointeger(L, -1) & 0xFFFFFFu);
        lua_pop(L, 4);
        fill_circle_direct(cx, cy, r, c);
    }
    return 0;
}

/* fill_ellipses(tbl) — draw one OR many anti-aliased axis-aligned solid
 * ellipses in a single Lua↔C crossing, the ellipse analogue of fill_circles().
 * `tbl` is a flat array {cx1,cy1,rx1,ry1,color1, …} — **5 values per ellipse**
 * (centre, x-radius, y-radius, 0xRRGGBB colour).  This is the only filled-
 * ellipse primitive: one ellipse or a thousand take the same call.  Same
 * fast-path win + coverage-blended AA edges as fill_circles (an rx==ry ellipse
 * renders identically to the matching circle).  One flush_layer() for the whole
 * batch: prior queued AA composites under, later draws land on top. */
static int l_fill_ellipses(lua_State *L)
{
    require_ready(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    flush_layer();   /* composite everything queued so far, once, under the batch */
    lua_Integer n = (lua_Integer)luaL_len(L, 1);
    for (lua_Integer i = 1; i + 4 <= n; i += 5) {
        lua_rawgeti(L, 1, i);
        int cx = (int)lua_tonumber(L, -1);
        lua_rawgeti(L, 1, i + 1);
        int cy = (int)lua_tonumber(L, -1);
        lua_rawgeti(L, 1, i + 2);
        int rx = (int)lua_tonumber(L, -1);
        lua_rawgeti(L, 1, i + 3);
        int ry = (int)lua_tonumber(L, -1);
        lua_rawgeti(L, 1, i + 4);
        lv_color_t c = lv_color_hex((uint32_t)lua_tointeger(L, -1) & 0xFFFFFFu);
        lua_pop(L, 5);
        fill_ellipse_direct(cx, cy, rx, ry, c);
    }
    return 0;
}

static int l_draw_circle(lua_State *L)
{
    lv_layer_t *layer = ensure_layer(L);
    int cx = check_int(L, 1), cy = check_int(L, 2), r = check_int(L, 3);
    lv_color_t c = check_color(L, 4);
    int width = lua_isinteger(L, 5) ? (int)lua_tointeger(L, 5) : 1;
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = c;
    d.width = width;
    d.center.x = cx; d.center.y = cy;
    d.radius = (uint16_t)r;
    d.start_angle = 0;
    d.end_angle = 360;
    lv_draw_arc(layer, &d);
    return 0;
}

/* draw_ellipse(cx, cy, rx, ry, color[, width]) — AA ellipse outline via the
 * direct-write stroke path (no LVGL ellipse primitive exists).  Like the other
 * direct writers it flush_layer()s first so queued AA composites underneath and
 * the stroke lands on top in script order. */
static int l_draw_ellipse(lua_State *L)
{
    require_ready(L);
    int cx = check_int(L, 1), cy = check_int(L, 2);
    int rx = check_int(L, 3), ry = check_int(L, 4);
    lv_color_t c = check_color(L, 5);
    int width = lua_isinteger(L, 6) ? (int)lua_tointeger(L, 6) : 1;
    flush_layer();
    draw_ellipse_direct(cx, cy, rx, ry, c, width);
    return 0;
}

static int l_draw_arc(lua_State *L)
{
    lv_layer_t *layer = ensure_layer(L);
    int cx = check_int(L, 1), cy = check_int(L, 2), r = check_int(L, 3);
    lua_Number a0 = luaL_checknumber(L, 4);
    lua_Number a1 = luaL_checknumber(L, 5);
    lv_color_t c = check_color(L, 6);
    int width = lua_isinteger(L, 7) ? (int)lua_tointeger(L, 7) : 1;
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = c;
    d.width = width;
    d.center.x = cx; d.center.y = cy;
    d.radius = (uint16_t)r;
    d.start_angle = (lv_value_precise_t)a0;
    d.end_angle = (lv_value_precise_t)a1;
    lv_draw_arc(layer, &d);
    return 0;
}

static int l_fill_arc(lua_State *L)
{
    lv_layer_t *layer = ensure_layer(L);
    int cx = check_int(L, 1), cy = check_int(L, 2);
    int r_out = check_int(L, 3), r_in = check_int(L, 4);
    lua_Number a0 = luaL_checknumber(L, 5);
    lua_Number a1 = luaL_checknumber(L, 6);
    lv_color_t c = check_color(L, 7);
    int width = r_out - r_in;
    if (width < 1) width = 1;
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = c;
    d.width = width;
    d.center.x = cx; d.center.y = cy;
    d.radius = (uint16_t)((r_out + r_in) / 2);
    d.start_angle = (lv_value_precise_t)a0;
    d.end_angle = (lv_value_precise_t)a1;
    lv_draw_arc(layer, &d);
    return 0;
}

static int l_fill_triangle(lua_State *L)
{
    lv_layer_t *layer = ensure_layer(L);
    int x0 = check_int(L, 1), y0 = check_int(L, 2);
    int x1 = check_int(L, 3), y1 = check_int(L, 4);
    int x2 = check_int(L, 5), y2 = check_int(L, 6);
    lv_color_t c = check_color(L, 7);
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.color = c;
    d.opa = LV_OPA_COVER;
    d.p[0].x = x0; d.p[0].y = y0;
    d.p[1].x = x1; d.p[1].y = y1;
    d.p[2].x = x2; d.p[2].y = y2;
    lv_draw_triangle(layer, &d);
    return 0;
}

static int l_draw_triangle(lua_State *L)
{
    /* outline = three lines */
    lv_layer_t *layer = ensure_layer(L);
    int x0 = check_int(L, 1), y0 = check_int(L, 2);
    int x1 = check_int(L, 3), y1 = check_int(L, 4);
    int x2 = check_int(L, 5), y2 = check_int(L, 6);
    lv_color_t c = check_color(L, 7);
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = c;
    d.width = 1;
    d.p1.x = x0; d.p1.y = y0; d.p2.x = x1; d.p2.y = y1; lv_draw_line(layer, &d);
    d.p1.x = x1; d.p1.y = y1; d.p2.x = x2; d.p2.y = y2; lv_draw_line(layer, &d);
    d.p1.x = x2; d.p1.y = y2; d.p2.x = x0; d.p2.y = y0; lv_draw_line(layer, &d);
    return 0;
}

/* ---- text ---------------------------------------------------------------- */

static const lv_font_t *font_for_size(int px)
{
    if (px >= 26) return &lv_font_montserrat_26;
    if (px >= 24) return &lv_font_montserrat_24;
    if (px >= 20) return &lv_font_montserrat_20;
    return &lv_font_montserrat_14;
}

static int l_draw_text(lua_State *L)
{
    lv_layer_t *layer = ensure_layer(L);
    int x = check_int(L, 1), y = check_int(L, 2);
    const char *str = luaL_checkstring(L, 3);
    lv_color_t fg = check_color(L, 4);
    int has_bg = lua_isinteger(L, 5);
    int font_px = lua_isinteger(L, 6) ? (int)lua_tointeger(L, 6) : 14;
    const lv_font_t *font = font_for_size(font_px);

    lv_point_t sz;
    lv_text_get_size(&sz, str, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

    if (has_bg) {
        lv_draw_rect_dsc_t bd;
        lv_draw_rect_dsc_init(&bd);
        bd.bg_opa = LV_OPA_COVER;
        bd.bg_color = lv_color_hex((uint32_t)lua_tointeger(L, 5) & 0xFFFFFFu);
        lv_area_t ba;
        area_set_wh(&ba, x, y, sz.x, sz.y);
        lv_draw_rect(layer, &bd, &ba);
    }

    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = str;
    d.color = fg;
    d.font = font;
    lv_area_t a;
    area_set_wh(&a, x, y, sz.x, sz.y);
    lv_draw_label(layer, &d, &a);
    return 0;
}

static int l_draw_text_aligned(lua_State *L)
{
    lv_layer_t *layer = ensure_layer(L);
    int x = check_int(L, 1), y = check_int(L, 2);
    int w = check_int(L, 3), h = check_int(L, 4);
    const char *str = luaL_checkstring(L, 5);
    lv_color_t fg = check_color(L, 6);
    const char *al = luaL_optstring(L, 7, "left");
    lv_text_align_t align = LV_TEXT_ALIGN_LEFT;
    if (strcmp(al, "center") == 0) align = LV_TEXT_ALIGN_CENTER;
    else if (strcmp(al, "right") == 0) align = LV_TEXT_ALIGN_RIGHT;

    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = str;
    d.color = fg;
    d.font = font_for_size(14);
    d.align = align;
    lv_area_t a;
    area_set_wh(&a, x, y, w, h);
    lv_draw_label(layer, &d, &a);
    return 0;
}

static int l_measure_text(lua_State *L)
{
    const char *str = luaL_checkstring(L, 1);
    int font_px = lua_isinteger(L, 2) ? (int)lua_tointeger(L, 2) : 14;
    lv_point_t sz;
    lv_text_get_size(&sz, str, font_for_size(font_px), 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lua_pushinteger(L, sz.x);
    lua_pushinteger(L, sz.y);
    return 2;
}

/* ---- clip ---------------------------------------------------------------- */

static int l_set_clip_rect(lua_State *L)
{
    require_ready(L);
    int x = check_int(L, 1), y = check_int(L, 2);
    int w = check_int(L, 3), h = check_int(L, 4);
    area_set_wh(&s_disp.clip, x, y, w, h);
    /* Clamp to the screen: LVGL requires layer._clip_area ⊆ buf_area, and an
     * out-of-bounds clip would let an enqueued opaque fill (solid_fill's layer
     * branch) or an AA task write past render_buf. */
    if (s_disp.clip.x1 < 0) s_disp.clip.x1 = 0;
    if (s_disp.clip.y1 < 0) s_disp.clip.y1 = 0;
    if (s_disp.clip.x2 > (int32_t)s_disp.surf.w - 1) s_disp.clip.x2 = (int32_t)s_disp.surf.w - 1;
    if (s_disp.clip.y2 > (int32_t)s_disp.surf.h - 1) s_disp.clip.y2 = (int32_t)s_disp.surf.h - 1;
    s_disp.clip_active = 1;
    if (s_disp.frame_active) {
        s_disp.layer._clip_area = s_disp.clip;
    }
    return 0;
}

static int l_clear_clip_rect(lua_State *L)
{
    require_ready(L);
    s_disp.clip_active = 0;
    if (s_disp.frame_active) {
        area_set_wh(&s_disp.layer._clip_area, 0, 0, s_disp.surf.w, s_disp.surf.h);
    }
    return 0;
}

/* ---- pixel blit ---------------------------------------------------------- */

/* draw_pixels(x, y, w, h, buf, format) — blit a raw RGB565 block into the
 * render buffer.  format "rgb565" = big-endian (matches our swapped canvas,
 * copied verbatim); "rgb565le" = little-endian (byte-swapped on the way in).
 * Writes directly to render_buf; composited at the next present().
 * NOTE: assumes a 2-byte (RGB565) surface — the raw-blit contract is defined in
 * terms of RGB565 bytes. */
static int l_draw_pixels(lua_State *L)
{
    require_ready(L);
    int x = check_int(L, 1), y = check_int(L, 2);
    int w = check_int(L, 3), h = check_int(L, 4);
    size_t blen = 0;
    const char *buf = luaL_checklstring(L, 5, &blen);
    const char *fmt = luaL_optstring(L, 6, "rgb565");
    int swap = (strcmp(fmt, "rgb565le") == 0);

    if (w <= 0 || h <= 0) return 0;
    if (blen < (size_t)w * h * 2u) {
        return luaL_error(L, "draw_pixels: buffer too small");
    }
    /* finish any pending vector draws so ordering is well-defined */
    flush_layer();
    uint16_t *dst = (uint16_t *)s_disp.surf.render_buf;
    const uint16_t *src = (const uint16_t *)buf;
    int W = (int)s_disp.surf.w, H = (int)s_disp.surf.h;
    for (int row = 0; row < h; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= H) continue;
        for (int col = 0; col < w; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= W) continue;
            uint16_t px = src[row * w + col];
            if (swap) {
                px = (uint16_t)((px >> 8) | (px << 8));
            }
            dst[dy * W + dx] = px;
        }
    }
    return 0;
}

/* draw_image(x, y, path [, w, h]) — hardware-decode a JPEG file and blit it
 * into the canvas at (x,y).  The RTL8721F post-processor scales the picture to
 * (w,h) DURING decode, so a large photo is never held in RAM at full resolution
 * — see jpeg_hw.c.  Sizing: omit both to keep the source size; give both to
 * force an exact (possibly distorting) size; give only w OR only h and the other
 * is derived from the source aspect ratio (fit-to-width / fit-to-height).
 *
 * Output is native RGB565 (LV_COLOR_FORMAT_RGB565), which matches the LCDC
 * canvas 1:1.  On the SPI/ST7789 backend the canvas is RGB565_SWAPPED, so we
 * byte-swap on the way in (correctness over speed; image use targets LCDC).
 *
 * Like draw_pixels(), this writes straight into render_buf: it flushes any
 * queued vector draws first, then composites at the next present().  Draw the
 * image FIRST, then rects/text on top.
 *
 * Returns w, h (the on-screen size actually drawn) on success, or nil, err. */
static int l_draw_image(lua_State *L)
{
    require_ready(L);
    int x = check_int(L, 1), y = check_int(L, 2);
    const char *path = luaL_checkstring(L, 3);
    int req_w = luaL_optinteger(L, 4, 0);
    int req_h = luaL_optinteger(L, 5, 0);
    if (req_w < 0) req_w = 0;
    if (req_h < 0) req_h = 0;

    /* ---- read the compressed JPEG into RAM (no lv_fs driver is registered, so
     * we cannot use the decoder's file path; feed it a memory buffer) -------- */
    FILE *f = fopen(path, "rb");
    if (!f) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open '%s'", path);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz <= 0 || (uint32_t)fsz > CLAW_DISPLAY_JPEG_MAX_FILE) {
        fclose(f);
        lua_pushnil(L);
        lua_pushfstring(L, "image file empty or too large (%d bytes)", (int)fsz);
        return 2;
    }
    uint8_t *jpg = malloc((size_t)fsz);
    if (!jpg) {
        fclose(f);
        lua_pushnil(L);
        lua_pushstring(L, "out of memory reading image");
        return 2;
    }
    size_t rd = fread(jpg, 1, (size_t)fsz, f);
    fclose(f);
    if (rd != (size_t)fsz) {
        free(jpg);
        lua_pushnil(L);
        lua_pushstring(L, "short read on image file");
        return 2;
    }

    /* ---- hardware decode + PP downscale to native RGB565 ------------------- */
    char err[64] = {0};
    uint32_t ow = 0, oh = 0;
    uint16_t *img = jpeg_hw_decode_rgb565(jpg, (uint32_t)fsz,
                                          (uint32_t)req_w, (uint32_t)req_h,
                                          &ow, &oh, err, sizeof(err));
    free(jpg);
    if (!img) {
        lua_pushnil(L);
        lua_pushstring(L, err[0] ? err : "jpeg decode failed");
        return 2;
    }

    /* ---- blit into render_buf (same immediate-write model as draw_pixels) -- */
    flush_layer();   /* composite any queued vector draws before we overwrite */

    const bool swap = (s_disp.be != &display_backend_lcdc);  /* SPI = swapped 565 */
    uint16_t *dst   = (uint16_t *)s_disp.surf.render_buf;
    int W = (int)s_disp.surf.w, H = (int)s_disp.surf.h;
    for (uint32_t row = 0; row < oh; row++) {
        int dy = y + (int)row;
        if (dy < 0 || dy >= H) continue;
        for (uint32_t col = 0; col < ow; col++) {
            int dx = x + (int)col;
            if (dx < 0 || dx >= W) continue;
            uint16_t px = img[row * ow + col];
            if (swap) {
                px = (uint16_t)((px >> 8) | (px << 8));
            }
            dst[dy * W + dx] = px;
        }
    }

    jpeg_hw_free(img);
    lua_pushinteger(L, (lua_Integer)ow);
    lua_pushinteger(L, (lua_Integer)oh);
    return 2;
}

/* ========================================================================= */
/* width/height read-only properties via module __index                       */
/* ========================================================================= */

static int module_index(lua_State *L)
{
    const char *key = lua_tostring(L, 2);
    if (key) {
        if (strcmp(key, "width") == 0) {
            lua_pushinteger(L, s_disp.mode == DISP_OWNER_DISPLAY ? s_disp.surf.w : 0);
            return 1;
        }
        if (strcmp(key, "height") == 0) {
            lua_pushinteger(L, s_disp.mode == DISP_OWNER_DISPLAY ? s_disp.surf.h : 0);
            return 1;
        }
    }
    /* fall back to the function table stored as uservalue-ish upvalue */
    lua_pushvalue(L, 2);
    lua_rawget(L, lua_upvalueindex(1));
    return 1;
}

/* ========================================================================= */
/* Registration                                                               */
/* ========================================================================= */

static const luaL_Reg display_funcs[] = {
    { "init",              l_init },
    { "deinit",            l_deinit },
    { "backlight",         l_backlight },
    { "begin_frame",       l_begin_frame },
    { "present",           l_present },
    { "present_full",      l_present_full },
    { "present_rect",      l_present_rect },
    { "end_frame",         l_end_frame },
    { "fps",               l_fps },
    { "frame_ms",          l_frame_ms },
    { "render_ms",         l_render_ms },
    { "clear",             l_clear },
    { "fill_rect",         l_fill_rect },
    { "draw_rect",         l_draw_rect },
    { "fill_round_rect",   l_fill_round_rect },
    { "draw_round_rect",   l_draw_round_rect },
    { "draw_pixel",        l_draw_pixel },
    { "draw_points",       l_draw_points },
    { "draw_line",         l_draw_line },
    { "fill_circles",      l_fill_circles },
    { "fill_ellipses",     l_fill_ellipses },
    { "draw_circle",       l_draw_circle },
    { "draw_ellipse",      l_draw_ellipse },
    { "draw_arc",          l_draw_arc },
    { "fill_arc",          l_fill_arc },
    { "fill_triangle",     l_fill_triangle },
    { "draw_triangle",     l_draw_triangle },
    { "draw_text",         l_draw_text },
    { "draw_text_aligned", l_draw_text_aligned },
    { "measure_text",      l_measure_text },
    { "set_clip_rect",     l_set_clip_rect },
    { "clear_clip_rect",   l_clear_clip_rect },
    { "draw_pixels",       l_draw_pixels },
    { "draw_image",        l_draw_image },
    { NULL, NULL },
};

int luaopen_display(lua_State *L)
{
    /* sentinel metatable (registered once). */
    if (luaL_newmetatable(L, DISPLAY_SENTINEL_MT)) {
        lua_pushcfunction(L, sentinel_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);

    /* Build the function table. */
    lua_newtable(L);
    luaL_setfuncs(L, display_funcs, 0);

    /* Wrap it in a proxy table whose __index exposes width/height + funcs. */
    lua_newtable(L);                 /* module proxy */
    lua_newtable(L);                 /* metatable */
    lua_pushvalue(L, -3);            /* the function table */
    lua_pushcclosure(L, module_index, 1);
    lua_setfield(L, -2, "__index");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");   /* protect metatable */
    lua_setmetatable(L, -2);         /* proxy.__mt = metatable */
    lua_remove(L, -2);               /* drop the raw function table */
    return 1;
}

void lua_module_display_init(void)
{
    if (s_disp.lock == NULL) {
        rtos_mutex_create(&s_disp.lock);
    }
}

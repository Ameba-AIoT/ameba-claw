/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * display_backend.h — the panel-backend seam for the "display" Lua module.
 *
 * display_lua.c is the backend-AGNOSTIC common layer: Lua bindings, ownership,
 * lv_init, argument/clip/z-order scheduling, the direct-write fast path
 * (dispatched by surface.bpp), the LVGL anti-aliased primitives, and the
 * present() timing wrappers.  Everything panel-specific lives behind this
 * vtable so a new transport plugs in WITHOUT touching the common layer.
 *
 *   - display_backend_spi   — ST7789 over SPI (implemented; see phase2 §1).
 *   - display_backend_lcdc  — RGB/LCDC framebuffer (stub; see phase2 §2/§2.6).
 *
 * Porting to LCDC touches ONLY the LCDC backend.  The three SPI-specific
 * assumptions it must override are called out in phase2_present_fastpath.md
 * §2.6: (1) pixel encoding (SPI = SWAPPED565; LCDC = native 565 / 888),
 * (2) which buffer direct-writes target (render_buf), (3) cache coherency —
 * SPI's present DMA cleans the frame for free; LCDC's present (page-flip) must
 * DCache_Clean the back buffer itself.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The drawing surface a backend hands to the common layer at init().  The
 * common layer reads these; it never allocates or frees them — the backend
 * owns their lifetime (freed in deinit()).
 */
typedef struct {
    lv_display_t *disp;        /* LVGL display, backend-created                 */
    lv_obj_t     *canvas;      /* canvas widget the AA primitives draw into     */
    uint8_t      *render_buf;  /* buffer the direct-write fast path targets     */
    uint8_t      *scratch;     /* strided present_rect gather scratch (or NULL) */
    size_t        scratch_sz;  /* bytes available in `scratch`                  */
    uint16_t      w;           /* width  in pixels                              */
    uint16_t      h;           /* height in pixels                              */
    uint8_t       bpp;         /* bytes per pixel in render_buf (2 = RGB565)    */

    /* ---- Multi-buffer contract (backend-agnostic swap policy) ---------------
     * The backend allocates `buf_count` (1..3) full-frame buffers and lists
     * them in `buffers[]`; `render_buf` always aliases `buffers[back_idx]` (the
     * one the CPU is drawing into).  present_full() presents `render_buf` and
     * does its own hardware sync (LCDC: page-flip + vblank; SPI: push).
     * When buf_count>1 the COMMON LAYER then advances back_idx round-robin
     * ((back_idx+1) % buf_count), re-aliases `render_buf`, and re-points the
     * canvas — so the next frame is drawn into a buffer no longer on screen.
     *   2 = double-buffer: present() blocks ~one vblank (next draw target is the
     *       buffer still being scanned); (idx+1)%2 == the classic toggle.
     *   3 = triple-buffer: present() arms the flip and returns; the round-robin
     *       hands back a THIRD, already-free buffer, so the CPU never waits on
     *       the flip it just armed — the vblank wait leaves the critical path.
     * buf_count==1 keeps the legacy single-buffer behaviour (no swap). */
    uint8_t      *buffers[3];  /* the 1..3 frame buffers, backend-owned          */
    uint8_t       buf_count;   /* 1 = single, 2 = double, 3 = triple buffered    */
    uint8_t       back_idx;    /* index in buffers[] currently == render_buf     */
} display_surface_t;

/*
 * Panel backend vtable.  All hooks are mandatory (no NULL checks in the caller).
 */
typedef struct display_backend_s {
    const char *name;

    /* Claim (or decline) board device `dev_id`.  Reads the device's `interface`
     * field from cap_board_mgr and returns 1 if this backend drives that
     * transport, 0 otherwise.  The common layer tries backends in registration
     * order and uses the first that returns 1 — keeping all board/cJSON
     * knowledge inside the backends (phase3 §decision 4). */
    int  (*probe)(const char *dev_id);

    /* Bring up the panel + LVGL display for board device `dev_id`, filling
     * `*s`.  Returns 0 on success; on failure returns non-zero, writes a short
     * message into `err`, and leaves nothing allocated (self-cleans). */
    int  (*init)(const char *dev_id, display_surface_t *s, char *err, size_t errlen);

    /* Tear down everything init() created and zero the surface.  Idempotent. */
    void (*deinit)(display_surface_t *s);

    /* Backlight on/off. */
    void (*backlight)(int on);

    /* Push the whole finished frame to the panel (SPI: CASET/RASET/RAMWR + DMA;
     * LCDC: cache-clean back buffer + VSYNC page-flip). */
    void (*present_full)(display_surface_t *s);

    /* Push only the given rectangle.  Coordinates are pre-flooring'd ints in
     * pixel space; the backend clamps to the screen. */
    void (*present_rect)(display_surface_t *s, int x, int y, int w, int h);

    /* Encode a colour into the native pixel value stored in render_buf, for the
     * direct-write fast path.  Must match exactly what the LVGL software
     * renderer stores for an opaque fill in this buffer's format (SPI:
     * lv_color_swap_16(lv_color_to_u16(c))). */
    uint32_t (*encode)(lv_color_t c);

    /* Inverse of encode(): turn a native render_buf pixel back into an
     * lv_color_t.  Needed by the AA fast path (fill_circles) to read-modify-
     * write blend the boundary fringe: dst = decode(stored); mix with src;
     * store encode(mixed).  Must round-trip encode(decode(v)) == v for the
     * bits the format carries (SPI un-swaps first; both expand 565→888). */
    lv_color_t (*decode)(uint32_t native);
} display_backend_t;

extern const display_backend_t display_backend_spi;
extern const display_backend_t display_backend_lcdc;

#ifdef __cplusplus
}
#endif

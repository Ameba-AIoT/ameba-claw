/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * jpeg_hw.h — hardware JPEG decode + post-processor (PP) downscale, producing a
 * native RGB565 bitmap ready to blit into the LCDC canvas (which is native
 * LV_COLOR_FORMAT_RGB565).
 *
 * WHY a direct API instead of the SDK's LVGL image-decoder (lv_ameba_jpeg.c):
 *   1. LVGL's image-decoder callback path decodes at FULL image resolution
 *      (ppOutImg.width == source width) and scales only later, at draw time —
 *      so a large photo must first fit in RAM at full size.  Here the caller
 *      passes the on-screen target size and the PP downscales DURING decode
 *      (JpegDecDecode + PPDecCombinedMode), so we never materialise the full
 *      image.  The RTL8721F PP supports arbitrary continuous scaling
 *      (down to 1/70, up to 3x) — verified in the SDK's jpegdec_pp_test.c and
 *      ppinternal.c::PPSetupScaling.
 *   2. LVGL's decoder honours LV_COLOR_DEPTH (=32 here) and emits XRGB8888; we
 *      force PP_PIX_FMT_RGB16_5_6_5 so the output matches the LCDC RGB565 canvas
 *      1:1 and can be memcpy-blitted with no per-pixel conversion.
 *
 * The decode sequence mirrors the SDK's tested flow (lv_ameba_jpeg.c
 * decoder_open_cb + jpegdec_pp_test.c) exactly; only the output size/format and
 * the buffer ownership differ.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One-time hardware bring-up (MJPEG clock + hx170 decoder core).  Idempotent —
 * safe to call on every draw_image; the first call does the work, later calls
 * are no-ops.  Returns 0 on success, non-zero on failure.
 */
int jpeg_hw_init(void);

/*
 * Decode `len` bytes of JPEG at `jpg` into a freshly-allocated native RGB565
 * (little-endian, LV_COLOR_FORMAT_RGB565) bitmap, downscaled/upscaled by the PP
 * to `req_w` x `req_h`.  Pass 0 for req_w and/or req_h to keep the source
 * dimension for that axis.
 *
 * On success returns a malloc'd buffer of (out_w * out_h * 2) bytes (free it
 * with jpeg_hw_free()) and writes the actual output size to *out_w and *out_h.
 * On failure returns NULL and writes a short message into `err`.
 *
 * Note: mixed scaling (one axis up while the other goes down) is rejected by the
 * PP; keep the aspect ratio (both axes shrink, or both grow) — callers that fit
 * an image to a box should compute a single uniform scale.
 */
uint16_t *jpeg_hw_decode_rgb565(const uint8_t *jpg, uint32_t len,
                                uint32_t req_w, uint32_t req_h,
                                uint32_t *out_w, uint32_t *out_h,
                                char *err, size_t errlen);

/* Free a buffer returned by jpeg_hw_decode_rgb565(). */
void jpeg_hw_free(uint16_t *buf);

#ifdef __cplusplus
}
#endif

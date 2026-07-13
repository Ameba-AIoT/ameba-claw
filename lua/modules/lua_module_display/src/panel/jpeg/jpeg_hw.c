/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * jpeg_hw.c — hardware JPEG decode + PP downscale to native RGB565.  See
 * jpeg_hw.h for the rationale.  The decode sequence copies the SDK's tested
 * flow (component/soc/amebagreen2/fwlib/jpeg_decoder: lv_ameba_jpeg.c
 * decoder_open_cb + verification/jpegdec/jpegdec_pp_test.c), differing only in
 * the PP output size (target, not source) and format (RGB565, not RGB32).
 */

#include "jpeg_hw.h"

#include <string.h>
#include <stdlib.h>

#include "ameba_soc.h"
#include "jpegdecapi.h"
#include "ppapi.h"
#include "decapicommon.h"

/* ---- one-time HW bring-up ------------------------------------------------ */

static bool s_jpeg_hw_ready;

int jpeg_hw_init(void)
{
    if (s_jpeg_hw_ready) {
        return 0;
    }
    /* Same bring-up lv_ameba_jpeg_init() does: MJPEG clock + hx170 core. */
    RCC_PeriphClockCmd(APBPeriph_MJPEG, APBPeriph_MJPEG_CLOCK, ENABLE);
    if (hx170dec_init() != 0) {
        return -1;
    }
    s_jpeg_hw_ready = true;
    return 0;
}

/* ---- helpers ------------------------------------------------------------- */

/* 32-byte-aligned alloc with a hidden base pointer, so jpeg_hw_free() can
 * recover the malloc() base.  The PP output only needs bus-width (4/8 B)
 * alignment, but a cache-line-aligned buffer keeps DCache ops clean. */
static uint16_t *aligned_alloc32(size_t size)
{
    void *base = malloc(size + 32 + sizeof(void *));
    if (!base) {
        return NULL;
    }
    uintptr_t raw     = (uintptr_t)base + sizeof(void *);
    uintptr_t aligned = (raw + 31u) & ~(uintptr_t)31u;
    ((void **)aligned)[-1] = base;
    return (uint16_t *)aligned;
}

void jpeg_hw_free(uint16_t *buf)
{
    if (buf) {
        free(((void **)buf)[-1]);
    }
}

/* Map a JpegDecImageInfo.outputFormat (chroma subsampling) to the matching PP
 * input pixel format.  Same correspondence lv_ameba_jpeg.c uses via its two
 * hw2sw/sw2hw hops, collapsed into one step. */
static int jpegdec_fmt_to_pp(uint32_t jpeg_fmt)
{
    switch (jpeg_fmt) {
    case JPEGDEC_YCbCr400:            return PP_PIX_FMT_YCBCR_4_0_0;
    case JPEGDEC_YCbCr420_SEMIPLANAR: return PP_PIX_FMT_YCBCR_4_2_0_SEMIPLANAR;
    case JPEGDEC_YCbCr422_SEMIPLANAR: return PP_PIX_FMT_YCBCR_4_2_2_SEMIPLANAR;
    case JPEGDEC_YCbCr440:            return PP_PIX_FMT_YCBCR_4_4_0;
    case JPEGDEC_YCbCr411_SEMIPLANAR: return PP_PIX_FMT_YCBCR_4_1_1_SEMIPLANAR;
    case JPEGDEC_YCbCr444_SEMIPLANAR: return PP_PIX_FMT_YCBCR_4_4_4_SEMIPLANAR;
    default:                          return -1;
    }
}

/* ---- decode -------------------------------------------------------------- */

/* PP downscale limit (ppinternal.h PP_OUT_MIN_DOWNSCALING_FACTOR): input may be
 * at most 70x the output on each axis.  Upscale limit is 3x. */
#define JPEG_HW_MAX_DOWNSCALE 70u
#define JPEG_HW_MIN_OUT       16u   /* PP_OUT_MIN_WIDTH / PP_OUT_MIN_HEIGHT */

uint16_t *jpeg_hw_decode_rgb565(const uint8_t *jpg, uint32_t len,
                                uint32_t req_w, uint32_t req_h,
                                uint32_t *out_w, uint32_t *out_h,
                                char *err, size_t errlen)
{
#define FAIL(msg) do { if (err && errlen) { snprintf(err, errlen, "%s", (msg)); } goto fail; } while (0)

    JpegDecInst  jpeg_inst = NULL;
    PPInst       pp_inst   = NULL;
    uint16_t    *out_buf   = NULL;
    bool         combined  = false;

    if (!jpg || len < 4) {
        if (err && errlen) { snprintf(err, errlen, "empty jpeg buffer"); }
        return NULL;
    }
    if (jpg[0] != 0xFF || jpg[1] != 0xD8 || jpg[2] != 0xFF) {
        if (err && errlen) { snprintf(err, errlen, "not a JPEG (bad SOI marker)"); }
        return NULL;
    }

    if (jpeg_hw_init() != 0) {
        if (err && errlen) { snprintf(err, errlen, "jpeg hw init failed"); }
        return NULL;
    }

    JpegDecInput     jpeg_in;
    JpegDecOutput    jpeg_out;
    JpegDecImageInfo info;
    PPConfig         pp_conf;

    memset(&jpeg_in,  0, sizeof(jpeg_in));
    memset(&jpeg_out, 0, sizeof(jpeg_out));
    memset(&info,     0, sizeof(info));
    memset(&pp_conf,  0, sizeof(pp_conf));

    jpeg_in.streamBuffer.pVirtualAddress = (u32 *)jpg;
    jpeg_in.streamBuffer.busAddress      = (u32)jpg;
    jpeg_in.streamLength                 = len;
    /* PP DMA reads the stream from RAM — make sure our fread() bytes are flushed. */
    DCache_Clean((u32)jpg, len);

    if (JpegDecInit(&jpeg_inst) != JPEGDEC_OK) {
        FAIL("JpegDecInit failed");
    }
    if (JpegDecGetImageInfo(jpeg_inst, &jpeg_in, &info) != JPEGDEC_OK) {
        FAIL("JpegDecGetImageInfo failed");
    }

    int pp_in_fmt = jpegdec_fmt_to_pp(info.outputFormat);
    if (pp_in_fmt < 0) {
        FAIL("unsupported JPEG chroma format");
    }

    uint32_t nw = info.outputWidth;
    uint32_t nh = info.outputHeight;

    /* Target size.  Four cases, so a caller can keep aspect ratio cheaply:
     *   both 0        -> native size (no scaling)
     *   both given    -> exactly that (may distort — caller's choice)
     *   only w given  -> h scaled to preserve aspect (fit-to-width)
     *   only h given  -> w scaled to preserve aspect (fit-to-height)
     * Proportional math rounds to nearest to avoid a systematic off-by-one. */
    uint32_t tw, th;
    if (req_w && req_h) {
        tw = req_w;  th = req_h;
    } else if (req_w) {
        tw = req_w;  th = (uint32_t)(((uint64_t)nh * req_w + nw / 2) / nw);
    } else if (req_h) {
        th = req_h;  tw = (uint32_t)(((uint64_t)nw * req_h + nh / 2) / nh);
    } else {
        tw = nw;     th = nh;
    }
    if (tw < JPEG_HW_MIN_OUT) tw = JPEG_HW_MIN_OUT;
    if (th < JPEG_HW_MIN_OUT) th = JPEG_HW_MIN_OUT;

    /* Reject what the PP would reject anyway, with a clearer message. */
    if (nw > tw * JPEG_HW_MAX_DOWNSCALE || nh > th * JPEG_HW_MAX_DOWNSCALE) {
        FAIL("downscale factor exceeds 70x");
    }
    if ((tw > nw && th < nh) || (tw < nw && th > nh)) {
        FAIL("mixed up/down scaling not supported (keep aspect ratio)");
    }

    out_buf = aligned_alloc32((size_t)tw * th * 2u);
    if (!out_buf) {
        FAIL("out of memory for decoded bitmap");
    }

    if (PPInit(&pp_inst) != PP_OK) {
        FAIL("PPInit failed");
    }
    if (PPDecCombinedModeEnable(pp_inst, jpeg_inst, PP_PIPELINED_DEC_TYPE_JPEG) != PP_OK) {
        FAIL("PPDecCombinedModeEnable failed");
    }
    combined = true;
    if (PPGetConfig(pp_inst, &pp_conf) != PP_OK) {
        FAIL("PPGetConfig failed");
    }

    pp_conf.ppInImg.width     = nw;
    pp_conf.ppInImg.height    = nh;
    pp_conf.ppInImg.videoRange = 1;
    pp_conf.ppInImg.pixFormat = (u32)pp_in_fmt;
    pp_conf.ppOutRgb.rgbTransform = PP_YCBCR2RGB_TRANSFORM_BT_709;
    pp_conf.ppOutImg.width     = tw;                    /* <-- decode-time scale */
    pp_conf.ppOutImg.height    = th;
    pp_conf.ppOutImg.pixFormat = PP_PIX_FMT_RGB16_5_6_5; /* native RGB565 */
    pp_conf.ppOutImg.bufferBusAddr = (u32)out_buf;

    /* Match the SDK's coherency handling: full clean+invalidate before kicking
     * the PP DMA so both the input stream and the output region are coherent. */
    DCache_CleanInvalidate(0xFFFFFFFF, 0xFFFFFFFF);

    if (PPSetConfig(pp_inst, &pp_conf) != PP_OK) {
        FAIL("PPSetConfig failed (scaling unsupported or size invalid)");
    }
    if (JpegDecDecode(jpeg_inst, &jpeg_in, &jpeg_out) != JPEGDEC_FRAME_READY) {
        FAIL("JpegDecDecode failed");
    }

    /* PP wrote the bitmap via DMA — drop stale CPU cache lines before we blit. */
    DCache_CleanInvalidate(0xFFFFFFFF, 0xFFFFFFFF);

    PPDecCombinedModeDisable(pp_inst, jpeg_inst);
    PPRelease(pp_inst);
    JpegDecRelease(jpeg_inst);

    *out_w = tw;
    *out_h = th;
    return out_buf;

fail:
    if (combined && pp_inst && jpeg_inst) {
        PPDecCombinedModeDisable(pp_inst, jpeg_inst);
    }
    if (pp_inst)   { PPRelease(pp_inst); }
    if (jpeg_inst) { JpegDecRelease(jpeg_inst); }
    if (out_buf)   { jpeg_hw_free(out_buf); }
    return NULL;
#undef FAIL
}

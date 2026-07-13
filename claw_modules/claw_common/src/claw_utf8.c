/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_utf8.h"
#include <string.h>

size_t claw_utf8_safe_len(const char *buf, size_t len)
{
    if (!buf || len == 0) return 0;

    size_t n = len;
    /* Walk back over UTF-8 continuation bytes (0x80-0xBF). */
    while (n > 0 && ((unsigned char)buf[n - 1] & 0xC0) == 0x80)
        n--;

    /* buf[n-1] is now ASCII or a lead byte.  If it's a lead byte, determine
     * whether the full multi-byte sequence fits within the original len: keep
     * it when complete, drop the dangling lead byte when clipped. */
    if (n > 0 && (unsigned char)buf[n - 1] >= 0xC0) {
        size_t lead_idx = n - 1;
        unsigned char lead = (unsigned char)buf[lead_idx];
        size_t seq_len = (lead < 0xE0) ? 2 : (lead < 0xF0) ? 3 : 4;
        if (lead_idx + seq_len <= len)
            n = lead_idx + seq_len; /* complete: keep the whole character */
        else
            n = lead_idx;           /* clipped: drop the dangling lead byte */
    }

    return n;
}

size_t claw_utf8_sanitize_inplace(char *s)
{
    if (!s) return 0;

    unsigned char *p = (unsigned char *)s;
    size_t n = strlen(s);
    size_t i = 0;

    while (i < n) {
        unsigned char c = p[i];

        if (c < 0x80) { i++; continue; }            /* ASCII: always valid */

        /* Determine expected sequence length from the lead byte. 0xC0/0xC1
         * (overlong 2-byte), 0xF5-0xFF (> U+10FFFF) and stray continuation
         * bytes 0x80-0xBF are never valid leads. */
        size_t seq_len;
        if (c >= 0xC2 && c <= 0xDF)      seq_len = 2;
        else if (c >= 0xE0 && c <= 0xEF) seq_len = 3;
        else if (c >= 0xF0 && c <= 0xF4) seq_len = 4;
        else { p[i++] = '?'; continue; }            /* invalid lead byte */

        /* The full sequence must fit and every trailing byte must be a
         * continuation byte (0x80-0xBF). On any failure, replace only the
         * lead byte and advance one: orphaned continuation bytes left behind
         * are then re-examined and replaced on the next iterations. */
        if (i + seq_len > n) { p[i++] = '?'; continue; }
        int ok = 1;
        for (size_t k = 1; k < seq_len; k++) {
            if ((p[i + k] & 0xC0) != 0x80) { ok = 0; break; }
        }
        if (!ok) { p[i++] = '?'; continue; }

        /* Reject overlong encodings and the UTF-16 surrogate range, which are
         * valid byte-wise but illegal code points that strict parsers reject. */
        if (seq_len == 3) {
            if (c == 0xE0 && p[i + 1] < 0xA0)  { p[i++] = '?'; continue; }
            if (c == 0xED && p[i + 1] >= 0xA0) { p[i++] = '?'; continue; }
        } else if (seq_len == 4) {
            if (c == 0xF0 && p[i + 1] < 0x90)  { p[i++] = '?'; continue; }
            if (c == 0xF4 && p[i + 1] > 0x8F)  { p[i++] = '?'; continue; }
        }

        i += seq_len;                               /* complete & valid */
    }

    return n;
}

void claw_utf8_truncate_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }

    size_t src_len = strlen(src);
    if (src_len < dst_size) {
        memcpy(dst, src, src_len + 1);
        return;
    }

    static const char notice[] = CLAW_UTF8_TRUNCATION_NOTICE;
    size_t notice_len = sizeof(notice) - 1;
    if (dst_size < notice_len + 1) { dst[0] = '\0'; return; }

    size_t limit = dst_size - notice_len - 1; /* reserve for notice + NUL */
    memcpy(dst, src, limit);
    size_t safe = claw_utf8_safe_len(dst, limit);

    memcpy(dst + safe, notice, notice_len);
    dst[safe + notice_len] = '\0';
}

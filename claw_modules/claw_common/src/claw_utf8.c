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

/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/*
 * Platform portability shim: provides logging macros, FreeRTOS/POSIX includes,
 * and BSD string utilities (strlcpy) for Ameba RTOS targets.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "os_wrapper.h"

/* strlcpy is BSD-only; provide a safe fallback if not available */
#ifndef strlcpy
static inline size_t claw_strlcpy(char *dst, const char *src, size_t sz)
{
    size_t slen = strlen(src);
    if (sz > 0) {
        size_t copy = slen < sz - 1 ? slen : sz - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return slen;
}
#define strlcpy(dst, src, sz) claw_strlcpy((dst), (src), (sz))
#endif

#include "rtk_status.h"

static inline const char *rtk_err_to_name(int err)
{
    switch (err) {
    case RTK_SUCCESS:       return "OK";
    case RTK_FAIL:          return "FAIL";
    case RTK_ERR_BADARG:    return "ERR_BADARG";
    case RTK_ERR_BUSY:      return "ERR_BUSY";
    case RTK_ERR_NOMEM:     return "ERR_NOMEM";
    case RTK_ERR_TIMEOUT:   return "ERR_TIMEOUT";
    default:                return "ERR_UNKNOWN";
    }
}

#include "log.h"

/* stdlib and mem utilities needed by ported modules */
#include <stdlib.h>
#include "memproc.h"

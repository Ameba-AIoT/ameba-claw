/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ameba_soc.h"   /* pulls platform_autoconf.h → CONFIG_CLAW_ENABLE_TESTS */

#ifdef CONFIG_CLAW_ENABLE_TESTS

#include "atcmd_service.h"
#include "claw_test.h"
#include <string.h>
#include <stdlib.h>

void handle_cmd_test(const char *arg2)
{
    const char *suite = arg2[0] ? arg2 : "all";
    char *buf = (char *)malloc(4096);
    if (!buf) { at_printf(ATCMD_ERROR_END_STR, 1); return; }
    buf[0] = '\0';
    int fails = 0;
    if      (strcmp(suite, "cap")    == 0) fails = claw_test_cap(buf, 4096);
    else if (strcmp(suite, "mem")    == 0) fails = claw_test_mem(buf, 4096);
    else if (strcmp(suite, "router") == 0) fails = claw_test_router(buf, 4096);
    else if (strcmp(suite, "fs")     == 0) fails = claw_test_fs(buf, 4096);
    else                                    fails = claw_test_all(buf, 4096);
    at_printf("\r\n%s", buf);
    free(buf);
    if (fails == 0) at_printf(ATCMD_OK_END_STR);
    else            at_printf(ATCMD_ERROR_END_STR, fails);
}

#endif /* CONFIG_CLAW_ENABLE_TESTS */

/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ameba_soc.h"
#include "atcmd_service.h"
#include "claw_memory.h"
#include <string.h>
#include <stdlib.h>

/* ---- Background task ---- */

static void memory_op_task(void *p)
{
    int flag = p ? *(int *)p : 1;
    rtos_mem_free(p);
    if (flag == 2) {
        int rc = claw_memory_clear_long_term();
        at_printf("\r\n+CLAW:memory,cleared,%s\r\n", rc == 0 ? "ok" : "not found");
        at_printf(ATCMD_OK_END_STR);
    } else {
        char *json = claw_memory_list(32);
        if (json) {
            at_printf("\r\n+CLAW:memory,list=%s\r\n", json);
            free(json);
        } else {
            at_printf("\r\n+CLAW:memory,list=[]\r\n");
        }
        at_printf(ATCMD_OK_END_STR);
    }
    rtos_task_delete(NULL);
}

/* ---- Handler ---- */

void handle_cmd_memory(const char *arg2)
{
    if (strcmp(arg2, "list") == 0 || strcmp(arg2, "clear") == 0) {
        /* "1"=list, "2"=clear */
        int *flag = (int *)rtos_mem_malloc(sizeof(int));
        if (flag) *flag = (strcmp(arg2, "clear") == 0) ? 2 : 1;
        if (!flag || rtos_task_create(NULL, "mem_op", memory_op_task,
                                      flag, 5120, 1) != RTK_SUCCESS) {
            rtos_mem_free(flag);
            at_printf(ATCMD_ERROR_END_STR, 1);
        } else {
            at_printf(ATCMD_OK_END_STR);
        }
    } else {
        at_printf("\r\n+CLAW:usage: AT+CLAW=memory,<list|clear>\r\n");
        at_printf(ATCMD_ERROR_END_STR, 1);
    }
}

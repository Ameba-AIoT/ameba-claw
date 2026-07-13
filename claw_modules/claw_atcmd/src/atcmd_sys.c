/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ameba_soc.h"
#include "atcmd_service.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>

void handle_cmd_sys(const char *arg2)
{
    if (strcmp(arg2, "tasks") == 0) {
        UBaseType_t n = uxTaskGetNumberOfTasks();
        TaskStatus_t *arr = rtos_mem_malloc(n * sizeof(TaskStatus_t));
        if (!arr) { at_printf(ATCMD_ERROR_END_STR, 1); return; }
        UBaseType_t filled = uxTaskGetSystemState(arr, n, NULL);
        char line[80];
        snprintf(line, sizeof(line), "%-16s %-9s PRI  STK_FREE", "TASK", "STATE");
        at_printf("\r\n+CLAW:%s\r\n", line);
        for (UBaseType_t i = 0; i < filled; i++) {
            const char *st;
            switch (arr[i].eCurrentState) {
            case eRunning:   st = "running";   break;
            case eReady:     st = "ready";     break;
            case eBlocked:   st = "blocked";   break;
            case eSuspended: st = "suspended"; break;
            default:         st = "deleted";   break;
            }
            snprintf(line, sizeof(line), "%-16s %-9s %3u  %u bytes",
                     arr[i].pcTaskName, st,
                     (unsigned)arr[i].uxCurrentPriority,
                     (unsigned)arr[i].usStackHighWaterMark * 4);
            at_printf("+CLAW:%s\r\n", line);
        }
        rtos_mem_free(arr);
        at_printf(ATCMD_OK_END_STR);
    } else {
        at_printf("\r\n+CLAW:usage: sys,tasks\r\n");
        at_printf(ATCMD_ERROR_END_STR, 4);
    }
}

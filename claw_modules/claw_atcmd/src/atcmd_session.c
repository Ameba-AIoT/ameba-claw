/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ameba_soc.h"
#include "atcmd_service.h"
#include "claw_memory.h"
#include "cap_session_mgr.h"
#include <string.h>
#include <stdlib.h>

/* ---- Background tasks ---- */

static void list_session_task(void *p)
{
    (void)p;
    char *buf = (char *)rtos_mem_malloc(512);
    if (!buf) {
        at_printf("\r\n+CLAW:session,list=empty\r\n");
        rtos_task_delete(NULL);
        return;
    }
    int n = cap_session_mgr_list("serial", "atcmd", buf, 512);
    if (n <= 0) {
        rtos_mem_free(buf);
        at_printf("\r\n+CLAW:session,list=empty\r\n");
        rtos_task_delete(NULL);
        return;
    }
    /* Count sessions: one entry per line; trailing newline already stripped by
     * cap_session_mgr_list, so count '\\n' separators and add 1. */
    int count = 1;
    for (int i = 0; i < n; i++) {
        if (buf[i] == '\n') count++;
    }
    at_printf("%s\r\n", buf);
    at_printf("+CLAW:session,total=%d\r\n", count);
    rtos_mem_free(buf);
    rtos_task_delete(NULL);
}

static void session_clear_task(void *p)
{
    int flag = p ? *(int *)p : 1;
    rtos_mem_free(p);
    if (flag == 2) {
        int n = claw_memory_clear_all_sessions();
        at_printf("\r\n+CLAW:session,cleared,%d files\r\n", n < 0 ? 0 : n);
        at_printf(ATCMD_OK_END_STR);
    } else {
        /* Check return: cap_session_mgr_clear_chat now returns RTK_FAIL on
         * malloc failure so we must not report success unconditionally. */
        int clr_rc = cap_session_mgr_clear_chat("serial", "atcmd");
        if (clr_rc == RTK_SUCCESS) {
            at_printf("\r\n+CLAW:session,cleared\r\n");
            at_printf(ATCMD_OK_END_STR);
        } else {
            at_printf(ATCMD_ERROR_END_STR, 1);
        }
    }
    rtos_task_delete(NULL);
}

/* ---- Handler ---- */

void handle_cmd_session(u16 argc, char **argv, const char *arg2, const char *arg3)
{
    if (strcmp(arg2, "list") == 0) {
        if (rtos_task_create(NULL, "ses_list", list_session_task,
                             NULL, 4096, 1) != RTK_SUCCESS) {
            at_printf(ATCMD_ERROR_END_STR, 1);
        } else {
            at_printf(ATCMD_OK_END_STR);
        }
    } else if (strcmp(arg2, "clear") == 0) {
        /* "1" = clear serial only, "2" = clear all */
        int *flag = (int *)rtos_mem_malloc(sizeof(int));
        if (flag) *flag = (strcmp(arg3, "all") == 0) ? 2 : 1;
        if (!flag || rtos_task_create(NULL, "ses_clr", session_clear_task,
                                      flag, 4096, 1) != RTK_SUCCESS) {
            rtos_mem_free(flag);
            at_printf(ATCMD_ERROR_END_STR, 1);
        } else {
            at_printf(ATCMD_OK_END_STR);
        }
    } else if (strcmp(arg2, "new") == 0) {
        /* AT+CLAW=session,new[,channel,chat_id[,name]] — create new session */
        const char *channel = (argc >= 4 && argv[3] && argv[3][0]) ? argv[3] : "serial";
        const char *chat_id = (argc >= 5 && argv[4] && argv[4][0]) ? argv[4] : "atcmd";
        const char *name    = (argc >= 6 && argv[5] && argv[5][0]) ? argv[5] : NULL;
        char actual_alias[40] = {0};
        int rc = cap_session_mgr_new(channel, chat_id, name,
                                     actual_alias, sizeof(actual_alias));
        if (rc == RTK_SUCCESS) {
            at_printf("\r\n+CLAW:session,new,%s,%s,%s\r\n", channel, chat_id, actual_alias);
            at_printf(ATCMD_OK_END_STR);
        } else {
            at_printf(ATCMD_ERROR_END_STR, 1);
        }
    } else {
        at_printf("\r\n+CLAW:usage: AT+CLAW=session,<list|clear[,all]|new[,ch,id[,name]]>\r\n");
        at_printf(ATCMD_ERROR_END_STR, 1);
    }
}

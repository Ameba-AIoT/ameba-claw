/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ameba_soc.h"
#include "atcmd_service.h"
#include "claw_agent.h"
#include "ameba_claw_defs.h"
#include "claw_utf8.h"
#include <string.h>
#include <stdlib.h>

static uint32_t s_serial_req_id = 0;

/* Buffer for multi-packet ask_buf assembly (ROM shell is limited to ~127 bytes
 * per command; ask_buf allows chunking a long prompt across multiple AT lines). */
#define CLAW_ASK_BUF_SIZE 1024
static char    s_ask_buf[CLAW_ASK_BUF_SIZE];
static size_t  s_ask_buf_len = 0;

/* Submit msg to the agent on session_id and print the reply. */
static void _ask_submit(const char *msg, const char *session_id)
{
    char *copy = malloc(CLAW_ASK_BUF_SIZE);
    if (!copy) {
        at_printf(ATCMD_ERROR_END_STR, 1);
        return;
    }
    claw_utf8_truncate_copy(copy, CLAW_ASK_BUF_SIZE, msg);

    claw_agent_response_t resp = {0};
    claw_agent_request_t req = {
        .request_id     = ++s_serial_req_id,
        .flags          = CLAW_AGENT_REQUEST_FLAG_SYNC_RECEIVE,
        .session_id     = session_id,
        .user_text      = copy,
        .source_channel = "serial",
        .source_chat_id = session_id,
    };

    at_printf("\r\n+CLAW:ask,session=%s\r\n", session_id);
    at_printf("+CLAW:%s\r\n", CLAW_IM_ACK_MSG);
    int rc = claw_agent_submit(&req, 5000);
    free(copy);
    if (rc == RTK_SUCCESS) {
        rc = claw_agent_receive_for(req.request_id, &resp, 300000);
    }
    if (rc != RTK_SUCCESS) {
        RTK_LOGA(NOTAG, "[claw] ask submit failed: %d\r\n", rc);
        at_printf(ATCMD_ERROR_END_STR, 2);
        return;
    }
    if (resp.status == CLAW_AGENT_RESPONSE_STATUS_OK && resp.text) {
        at_printf("+CLAW:%s\r\n", resp.text);
        at_printf(ATCMD_OK_END_STR);
    } else {
        RTK_LOGA(NOTAG, "[claw] ask error: %s\r\n",
                 resp.error_message ? resp.error_message : "unknown");
        at_printf(ATCMD_ERROR_END_STR, 3);
    }
    claw_agent_response_free(&resp);
}

void handle_cmd_ask(u16 argc, char **argv, const char *arg2, const char *arg3)
{
    (void)arg3;

    if (arg2[0] == '\0') {
        at_printf("\r\n+CLAW:usage: AT+CLAW=ask,<message>[,sid,<session_id>]\r\n");
        at_printf(ATCMD_ERROR_END_STR, 1);
        return;
    }

    /* Reconstruct message from argv[2..n], restoring commas that
     * parse_param_advance replaced with '\0'.  argv pointers are
     * into a single contiguous buffer, so the original comma sat at
     * (argv[i+1] - 1) — just write it back.
     *
     * Also detect optional trailing ",sid,<id>" suffix: walk from
     * the end looking for the "sid" keyword and strip it off. */
    const char *session_id = "serial";

    /* Patch '\0' separators back to ',' in the argv buffer so that
     * argv[2] .. argv[argc-1] form one continuous C-string again. */
    for (int i = 2; i < argc - 1 && argv[i] && argv[i + 1]; i++) {
        /* The char between argv[i]'s end and argv[i+1]'s start was ','. */
        char *sep = argv[i] + strlen(argv[i]);
        *sep = ',';
    }
    /* Now argv[2] is the whole message (commas restored).
     * Check for trailing ,sid,<id> and strip it. */
    char *raw = (char *)arg2;   /* arg2 == argv[2] */
    char *sid_pos = NULL;
    {
        /* Search backwards for ",sid," pattern */
        size_t rlen = strlen(raw);
        if (rlen > 5) {
            char *p = raw + rlen - 1;
            while (p > raw + 4) {
                if (strncmp(p - 4, ",sid,", 5) == 0) {
                    sid_pos = p - 4;
                    break;
                }
                p--;
            }
        }
    }
    if (sid_pos) {
        session_id = sid_pos + 5;  /* skip ",sid," */
        *sid_pos = '\0';            /* truncate message */
    }

    _ask_submit(raw, session_id);
}

/*
 * AT+CLAW=ask_buf,<chunk>   Append <chunk> to the internal ask buffer.
 * AT+CLAW=ask_buf           Execute the accumulated buffer as an ask, then clear.
 * AT+CLAW=ask_buf,clear     Clear the buffer without executing.
 *
 * Allows sending prompts longer than the ~126-byte ROM shell limit by
 * splitting them across multiple AT lines:
 *   AT+CLAW=ask_buf,写一个程序，读取板子上连接的光照传感器
 *   AT+CLAW=ask_buf,，每隔 2 秒采样一次，连续采样 10 次
 *   AT+CLAW=ask_buf,，每次将当前光照状态（亮或暗）打印到串口；全部完成后打印 done 并退出。
 *   AT+CLAW=ask_buf
 */
void handle_cmd_ask_buf(u16 argc, char **argv, const char *arg2)
{
    (void)argc; (void)argv;

    if (arg2 && arg2[0] != '\0') {
        if (strcmp(arg2, "clear") == 0) {
            s_ask_buf[0]  = '\0';
            s_ask_buf_len = 0;
            at_printf("\r\n+CLAW:ask_buf cleared\r\n");
            at_printf(ATCMD_OK_END_STR);
            return;
        }
        /* Append chunk */
        size_t chunk = strlen(arg2);
        if (s_ask_buf_len + chunk >= CLAW_ASK_BUF_SIZE - 1) {
            at_printf("\r\n+CLAW:ask_buf overflow (max %d bytes total)\r\n",
                      CLAW_ASK_BUF_SIZE - 1);
            at_printf(ATCMD_ERROR_END_STR, 1);
            return;
        }
        memcpy(s_ask_buf + s_ask_buf_len, arg2, chunk);
        s_ask_buf_len += chunk;
        s_ask_buf[s_ask_buf_len] = '\0';
        at_printf("\r\n+CLAW:ask_buf len=%d\r\n", (int)s_ask_buf_len);
        at_printf(ATCMD_OK_END_STR);
    } else {
        /* Execute */
        if (s_ask_buf_len == 0) {
            at_printf("\r\n+CLAW:ask_buf empty — use AT+CLAW=ask_buf,<text> to append first\r\n");
            at_printf(ATCMD_ERROR_END_STR, 1);
            return;
        }
        char tmp[CLAW_ASK_BUF_SIZE];
        memcpy(tmp, s_ask_buf, s_ask_buf_len + 1);
        s_ask_buf[0]  = '\0';
        s_ask_buf_len = 0;
        _ask_submit(tmp, "serial");
    }
}

void handle_cmd_lua(void)
{
    extern void lua_run_repl_once(void);
    RTK_LOGA(NOTAG, "[claw] entering Lua REPL — type exit() to return\r\n");
    lua_run_repl_once();
    RTK_LOGA(NOTAG, "[claw] Lua REPL exited\r\n");
    at_printf(ATCMD_OK_END_STR);
}

/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * atcmd_lua.c — every AT+CLAW subcommand that touches Lua execution:
 *   lua_repl           interactive REPL (its own lua_State, no job table)
 *   lua_execute_sync   run a .lua file by path, blocking until done
 *   lua_execute_async  run a .lua file by path as a background job
 *
 * lua_execute_{sync,async} call straight into cap_lua.h's plain-C job API —
 * the same core (cap_lua_run_script[_async]) that lua_module_thread's
 * thread.run()/thread.start() use. See
 * design_spec/lua/lua_module_thread_architecture.md for the shared-core
 * diagram (this file is "entry③" in that doc).
 */

#include "ameba_soc.h"
#include "atcmd_service.h"
#include "cap_lua.h"
#include <string.h>
#include <stdlib.h>
#include <cJSON.h>

/* ---- lua_repl ---- */

void handle_cmd_lua_repl(void)
{
    extern void lua_run_repl_once(void);
    RTK_LOGA(NOTAG, "[claw] entering Lua REPL — type exit() to return\r\n");
    lua_run_repl_once();
    RTK_LOGA(NOTAG, "[claw] Lua REPL exited\r\n");
    at_printf(ATCMD_OK_END_STR);
}

/* ---- lua_execute_{sync,async} ---- */

typedef struct { char path[128]; char args[256]; } lua_execute_args_t;

/* `args` (comma-reconstructed from argv, see lua_execute_build below) may be
 * empty or not a JSON object — normalize to a heap JSON-object string (or
 * NULL for "no args"), the shape cap_lua_run_script[_async] pass straight
 * through to the script's run(args). Caller frees the non-NULL result. */
static char *lua_execute_normalize_args(const char *raw)
{
    if (!raw || raw[0] == '\0') return NULL;
    cJSON *parsed = cJSON_Parse(raw);
    if (!parsed || !cJSON_IsObject(parsed)) {
        cJSON_Delete(parsed);
        return NULL;
    }
    char *out = cJSON_PrintUnformatted(parsed);
    cJSON_Delete(parsed);
    return out;
}

static void lua_execute_sync_task(void *p)
{
    lua_execute_args_t *a = (lua_execute_args_t *)p;

    at_printf("\r\n+CLAW:lua_execute_sync,running=%s\r\n", a->path);

    char *args_json = lua_execute_normalize_args(a->args);
    char *output = NULL;
    /* allow_tmp is always false here (not the direct-LLM-tool-call case). */
    int rc = cap_lua_run_script(a->path, args_json, 0, NULL, NULL, &output);
    free(args_json);
    free(a);

    if (rc == RTK_SUCCESS && output) {
        at_printf("+CLAW:lua_execute_sync,result=%s\r\n", output);
        at_printf(ATCMD_OK_END_STR);
    } else {
        at_printf("+CLAW:lua_execute_sync,rc=%d\r\n", rc);
        if (output) at_printf("+CLAW:lua_execute_sync,msg=%s\r\n", output);
        at_printf(ATCMD_ERROR_END_STR, rc ? rc : -1);
    }
    free(output);
    rtos_task_delete(NULL);
}

static void lua_execute_async_task(void *p)
{
    lua_execute_args_t *a = (lua_execute_args_t *)p;

    char *args_json = lua_execute_normalize_args(a->args);
    char *output = NULL;
    /* Returns almost immediately with a job_id — the script itself runs in
     * its own job task. Origin is the serial console so a script launched this
     * way can reply to it via event.notify() (serial is a registered channel). */
    int rc = cap_lua_run_script_async(a->path, args_json, 0,
                                       NULL, NULL, false, "serial", "serial", &output);
    free(args_json);
    free(a);

    if (rc == RTK_SUCCESS && output) {
        at_printf("\r\n+CLAW:lua_execute_async,result=%s\r\n", output);
        at_printf(ATCMD_OK_END_STR);
    } else {
        at_printf("\r\n+CLAW:lua_execute_async,rc=%d\r\n", rc);
        if (output) at_printf("+CLAW:lua_execute_async,msg=%s\r\n", output);
        at_printf(ATCMD_ERROR_END_STR, rc ? rc : -1);
    }
    free(output);
    rtos_task_delete(NULL);
}

/* Shared arg-building for both lua_execute_sync/async: <path> is used
 * verbatim (no name-to-skill-directory resolution — this is a pure "run this
 * file" primitive, decoupled from the skill concept). args_json is
 * reconstructed from comma-split argv[3..] because a JSON object contains
 * commas that the AT parser already split on. */
static lua_execute_args_t *lua_execute_build(u16 argc, char **argv, const char *arg2)
{
    lua_execute_args_t *a = (lua_execute_args_t *)malloc(sizeof(*a));
    if (!a) return NULL;
    strlcpy(a->path, arg2, sizeof(a->path));
    a->args[0] = '\0';
    if (argc >= 4 && argv[3] && argv[3][0]) {
        strlcpy(a->args, argv[3], sizeof(a->args));
        for (int i = 4; i < argc && argv[i]; i++) {
            strlcat(a->args, ",", sizeof(a->args));
            strlcat(a->args, argv[i], sizeof(a->args));
        }
    }
    return a;
}

void handle_cmd_lua_execute_sync(u16 argc, char **argv, const char *arg2, const char *arg3)
{
    if (arg2[0] == '\0') {
        at_printf("\r\n+CLAW:usage: AT+CLAW=lua_execute_sync,<path>[,<args_json>]\r\n");
        at_printf(ATCMD_ERROR_END_STR, 1);
        return;
    }
    lua_execute_args_t *a = lua_execute_build(argc, argv, arg2);
    if (!a) { at_printf(ATCMD_ERROR_END_STR, 2); return; }
    if (rtos_task_create(NULL, "lua_exec_sync", lua_execute_sync_task,
                         a, 8192, 1) != RTK_SUCCESS) {
        free(a);
        at_printf(ATCMD_ERROR_END_STR, 3);
        return;
    }
    at_printf("\r\n+CLAW:lua_execute_sync,queued=%s\r\n", arg2);
    at_printf(ATCMD_OK_END_STR);

    (void)arg3;
}

void handle_cmd_lua_execute_async(u16 argc, char **argv, const char *arg2, const char *arg3)
{
    if (arg2[0] == '\0') {
        at_printf("\r\n+CLAW:usage: AT+CLAW=lua_execute_async,<path>[,<args_json>]\r\n");
        at_printf(ATCMD_ERROR_END_STR, 1);
        return;
    }
    lua_execute_args_t *a = lua_execute_build(argc, argv, arg2);
    if (!a) { at_printf(ATCMD_ERROR_END_STR, 2); return; }
    if (rtos_task_create(NULL, "lua_exec_async", lua_execute_async_task,
                         a, 8192, 1) != RTK_SUCCESS) {
        free(a);
        at_printf(ATCMD_ERROR_END_STR, 3);
        return;
    }
    at_printf("\r\n+CLAW:lua_execute_async,queued=%s\r\n", arg2);
    at_printf(ATCMD_OK_END_STR);

    (void)arg3;
}

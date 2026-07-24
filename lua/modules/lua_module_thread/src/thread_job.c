/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * thread_job.c — `thread.run/start/list/get/stop`: Lua bindings onto the
 * plain-C job API added to cap_lua.h for D2 (design_spec/lua/
 * lua_module_thread_architecture.md). A running skill script can orchestrate other
 * scripts as jobs without going through cap.call()'s JSON string round-trip.
 *
 * Table<->JSON conversion is delegated to the already-loaded `cjson` module
 * (encode for args going in, decode for the job-table JSON coming back) —
 * no second JSON encoder in this file.
 *
 * Deviations from the original design, both documented in docs/thread.md:
 *   - get()/stop() take a numeric job_id only, not id_or_name.
 *   - stop()'s grace period is the fixed LUA_JOB_STOP_WAIT_MS (no per-call
 *     wait_ms override — cap_lua_stop_job doesn't expose one).
 */
#include "lua_module_thread_internal.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cap_lua.h"
#include "claw_compat.h"
#include "lauxlib.h"

/* Encode the Lua value at stack index `idx` to a JSON string via the
 * already-loaded cjson module. Returns a malloc'd string (caller frees) or
 * NULL on any failure (missing cjson, non-encodable value, etc). */
static char *thread_encode_json(lua_State *L, int idx)
{
    lua_getglobal(L, "cjson");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return NULL; }
    lua_getfield(L, -1, "encode");
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return NULL; }
    lua_pushvalue(L, idx);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) { lua_pop(L, 1); return NULL; }
    const char *s = lua_tostring(L, -1);
    char *out = s ? strdup(s) : NULL;
    lua_pop(L, 1);
    return out;
}

/* Decode `json` via cjson and push exactly one value (the decoded value, or
 * nil if json is NULL or decode fails). */
static void thread_decode_json(lua_State *L, const char *json)
{
    if (!json) { lua_pushnil(L); return; }
    lua_getglobal(L, "cjson");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_pushnil(L); return; }
    lua_getfield(L, -1, "decode");
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); lua_pushnil(L); return; }
    lua_pushstring(L, json);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) { lua_pop(L, 1); lua_pushnil(L); return; }
}

/* Decode `output`'s .error field (best-effort) and push nil, errmsg — always
 * exactly 2 values. Falls back to `fallback` if .error is absent/unparsable. */
static int thread_push_error(lua_State *L, const char *output, const char *fallback)
{
    char errbuf[160] = "";
    thread_decode_json(L, output);
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "error");
        if (lua_isstring(L, -1)) {
            strncpy(errbuf, lua_tostring(L, -1), sizeof(errbuf) - 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_pushstring(L, errbuf[0] ? errbuf : fallback);
    return 2;
}

/* Read the current script's launch origin (same registry keys
 * lua_module_event.c's event.origin() reads) into caller-owned buffers, so a
 * job orchestrating child jobs can pass its own origin through — children
 * can then event.notify() the same user without knowing the ids themselves. */
static void thread_get_origin(lua_State *L, char *channel, size_t channel_cap,
                               char *chat, size_t chat_cap)
{
    channel[0] = '\0';
    chat[0] = '\0';
    lua_getfield(L, LUA_REGISTRYINDEX, "__origin_channel");
    if (lua_isstring(L, -1)) {
        strncpy(channel, lua_tostring(L, -1), channel_cap - 1);
        channel[channel_cap - 1] = '\0';
    }
    lua_pop(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "__origin_chat");
    if (lua_isstring(L, -1)) {
        strncpy(chat, lua_tostring(L, -1), chat_cap - 1);
        chat[chat_cap - 1] = '\0';
    }
    lua_pop(L, 1);
}

/* thread.run(path, args, opts) -> true, result_table | nil, err
 * opts.timeout_ms optional (0/absent -> LUA_EXEC_TIMEOUT_MS default).
 * Synchronous — blocks the calling job, consuming a second concurrent slot
 * from the shared LUA_JOB_MAX_RUNNING budget for the duration (see D1). */
static int lthread_run(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    char *args_json = !lua_isnoneornil(L, 2) ? thread_encode_json(L, 2) : NULL;
    int timeout_ms = 0;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "timeout_ms");
        if (lua_isnumber(L, -1)) timeout_ms = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }

    char channel[64], chat[64];
    thread_get_origin(L, channel, sizeof(channel), chat, sizeof(chat));

    char *output = NULL;
    int rc = cap_lua_run_script(path, args_json, timeout_ms, channel, chat, &output);
    free(args_json);

    int nret;
    if (rc == RTK_SUCCESS) {
        lua_pushboolean(L, 1);
        thread_decode_json(L, output);
        nret = 2;
    } else {
        nret = thread_push_error(L, output, "run failed");
    }
    free(output);
    return nret;
}

/* thread.start(path, args, opts) -> job_id | nil, err
 * opts: timeout_ms, name, exclusive, replace. Async — returns immediately. */
static int lthread_start(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    char *args_json = !lua_isnoneornil(L, 2) ? thread_encode_json(L, 2) : NULL;
    int timeout_ms = 0;
    const char *name = NULL, *exclusive = NULL;
    bool replace = false;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "timeout_ms");
        if (lua_isnumber(L, -1)) timeout_ms = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "name");
        if (lua_isstring(L, -1)) name = lua_tostring(L, -1);
        /* leave name's string on the stack — it must stay valid until we're
         * done reading it below, since we don't strdup it. */
        lua_getfield(L, 3, "exclusive");
        if (lua_isstring(L, -1)) exclusive = lua_tostring(L, -1);
        lua_getfield(L, 3, "replace");
        if (lua_isboolean(L, -1)) replace = lua_toboolean(L, -1);
        /* 3 values pushed above (name, exclusive, replace) — pop after use. */
    }

    char channel[64], chat[64];
    thread_get_origin(L, channel, sizeof(channel), chat, sizeof(chat));

    char *output = NULL;
    int rc = cap_lua_run_script_async(path, args_json, timeout_ms, name, exclusive, replace,
                                       channel, chat, &output);
    free(args_json);
    if (lua_istable(L, 3)) lua_pop(L, 3);   /* pop name/exclusive/replace strings */

    int nret;
    if (rc == RTK_SUCCESS) {
        thread_decode_json(L, output);
        lua_Integer jid = 0;
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "job_id");
            jid = lua_tointeger(L, -1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        lua_pushinteger(L, jid);
        nret = 1;
    } else {
        nret = thread_push_error(L, output, "start failed");
    }
    free(output);
    return nret;
}

/* thread.list([status]) -> array of {job_id, status, name}
 * status is an exact-case match against job_status_str() ("QUEUED",
 * "RUNNING", "DONE", "FAILED", "TIMEOUT", "STOPPED") or nil/"all" for
 * everything. */
static int lthread_list(lua_State *L)
{
    const char *status_filter = lua_isstring(L, 1) ? lua_tostring(L, 1) : NULL;
    if (status_filter && strcmp(status_filter, "all") == 0) status_filter = NULL;

    char *output = NULL;
    int rc = cap_lua_list_jobs(&output);
    if (rc != RTK_SUCCESS) {
        int nret = thread_push_error(L, output, "list failed");
        free(output);
        return nret;
    }
    thread_decode_json(L, output);
    free(output);
    if (!lua_istable(L, -1)) {
        return 1;   /* decode failed; nil already on top */
    }
    lua_getfield(L, -1, "jobs");
    lua_remove(L, -2);
    if (!status_filter || !lua_istable(L, -1)) {
        return 1;
    }

    int n = (int)lua_rawlen(L, -1);
    lua_newtable(L);
    int result_idx = lua_gettop(L);
    int out_i = 1;
    for (int i = 1; i <= n; i++) {
        lua_geti(L, -2, i);
        lua_getfield(L, -1, "status");
        const char *st = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        bool match = (strcmp(st, status_filter) == 0);
        lua_pop(L, 1);
        if (match) {
            lua_rawseti(L, result_idx, out_i++);
        } else {
            lua_pop(L, 1);
        }
    }
    lua_remove(L, -2);   /* drop the unfiltered jobs array, keep result */
    return 1;
}

/* thread.get(job_id) -> {job_id, status, path, log_seq, log_truncated,
 *                         started_ms, finished_ms, log} | nil, err */
static int lthread_get(lua_State *L)
{
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    char *output = NULL;
    int rc = cap_lua_get_job(id, 0, &output);
    int nret;
    if (rc == RTK_SUCCESS) {
        thread_decode_json(L, output);
        nret = 1;
    } else {
        nret = thread_push_error(L, output, "not found");
    }
    free(output);
    return nret;
}

/* thread.stop(job_id) -> {job_id, stopped, status[, hint]} | nil, err
 * Grace period is the fixed LUA_JOB_STOP_WAIT_MS — no per-call override. */
static int lthread_stop(lua_State *L)
{
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    char *output = NULL;
    int rc = cap_lua_stop_job(id, &output);
    int nret;
    if (rc == RTK_SUCCESS) {
        thread_decode_json(L, output);
        nret = 1;
    } else {
        nret = thread_push_error(L, output, "stop failed");
    }
    free(output);
    return nret;
}

void lua_module_thread_register_job_funcs(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"run",   lthread_run},
        {"start", lthread_start},
        {"list",  lthread_list},
        {"get",   lthread_get},
        {"stop",  lthread_stop},
        {NULL, NULL},
    };
    luaL_setfuncs(L, funcs, 0);
}

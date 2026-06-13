/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * cap_lua — Lua execution primitive. Exposes a single tool `lua_run(path, args
 * [, timeout_ms])`. See cap_lua.h for the responsibility split with cap_skill.
 *
 * Inc 3 of improvement #12:
 *   - execution decoupled from cap_skill_mgr (was skill_run, by-name);
 *   - lua_run takes an explicit PATH (allowed roots only, must end .lua, no "..");
 *   - args is a JSON object decoded into a Lua TABLE and passed to run(args)
 *     (no more JSON string + cjson.decode boilerplate in scripts);
 *   - strict sandbox (no load/loadfile/io/os/debug);
 *   - timeout (30s) + cooperative cancel hook + hard-timeout abandon, ported
 *     verbatim from the previous cap_skill_run implementation.
 *
 * Inc 4 of improvement #12:
 *   - the SINGLE sandbox concession: a RESTRICTED require() override lets scripts
 *     `require("lib/<name>")` to pull in blessed libraries from the read-only
 *     rolfs:/lib/ directory. (The platform Lua uses a stubbed package library
 *     with no searchers chain — see lua/lloadlib_stub.c — so the concession is a
 *     require() override, not a package.searchers entry.) It only resolves names
 *     of the form "lib/<name>" to rolfs:/lib/<name>.lua (no "/", "\\" or ".."
 *     in <name>, so it cannot escape that dir); C modules still resolve via
 *     _LOADED; no other filesystem require is possible; io/os/debug stay closed.
 *
 * Inc 7 of improvement #12 — ASYNCHRONOUS JOBS (full tier):
 *   - lua_run_async(path, args[, timeout_ms][, name][, exclusive][, replace])
 *     spawns a background job and returns a job_id immediately.
 *   - lua_job_get(job_id) / lua_job_list() / lua_job_stop(job_id) inspect and
 *     control jobs.
 *   - Fixed job table (LUA_JOB_SLOTS=4), bounded concurrency
 *     (LUA_JOB_MAX_RUNNING=2), per-job ring log (LUA_JOB_LOG_SIZE=2048) with a
 *     monotonic seq for tail/incremental reads, all guarded by one FreeRTOS
 *     mutex. Terminal slots are reusable.
 *   - name / exclusive / replace arbitration: at most one ACTIVE job per name
 *     and per exclusive group; replace=false rejects with a reason, replace=true
 *     stops the conflicting job first.
 *   - print() is captured into the job's ring log (multi-arg \t-joined), so the
 *     LLM can watch a long job's progress via lua_job_get.
 * Constants are sized for the device memory budget: 4 slots, 1 concurrent,
 * 2 KiB ring log.
 */
#include "cap_lua.h"
#include "claw_cap.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include <cJSON.h>
#include "platform_stdlib.h"
#include "vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ameba_claw_defs.h"


/* The sandbox-safe Lua module set is described once in the registry
 * (modules tagged LUA_MOD_ENV_SKILL); this cap just installs them. */
#include "lua_module_registry.h"

#define TAG "cap_lua"

/* Default per-run wall-clock timeout (ms). Can be overridden per call via the
 * optional timeout_ms argument (0 → use default). */
#define LUA_EXEC_TIMEOUT_MS   30000
/* Cancel hook fires every N Lua instructions. */
#define LUA_CANCEL_INSTR_FREQ 500
/* Maximum script size read from the filesystem. */
#define LUA_SCRIPT_MAX        65536

/* ---- Path validation -------------------------------------------------------
 *
 * lua_run may only execute scripts under one of these roots:
 *   rolfs:/skills/   rolfs:/lib/   vfs:/skills/   vfs:/scripts/   vfs:/tmp/
 * The path must end in ".lua" and must not contain a ".." segment. This mirrors
 * 12_skill_lua_separation.md §C.4. require() of arbitrary files stays disabled
 * (Inc 4), so these roots are the only execution surface.
 *
 * vfs:/scripts/ — persistent user application scripts (survive reboot).
 *   Use this for long-running user apps (clock, monitor daemons, etc.).
 *   Unlike vfs:/tmp/ it is NOT wiped on boot; unlike vfs:/skills/ it is
 *   NOT managed by the skill catalog.
 */
static const char *const s_allowed_roots[] = {
    "rolfs:/skills/",
    "rolfs:/lib/",
    "vfs:/skills/",
    "vfs:/scripts/",
    "vfs:/tmp/",
    NULL,
};

/* Returns true if `path` contains a ".." path segment (could escape a root). */
static bool path_has_dotdot(const char *path)
{
    const char *p = path;
    while ((p = strstr(p, "..")) != NULL) {
        char before = (p == path) ? '/' : p[-1];
        char after  = p[2];
        if ((before == '/' || before == '\0') &&
            (after == '/'  || after == '\0')) {
            return true;
        }
        p += 2;
    }
    return false;
}

/* Validate the path against the allowed roots, ".lua" suffix and no "..".
 * Returns NULL on success, or a static human-readable error string. */
static const char *validate_lua_path(const char *path)
{
    if (!path || path[0] == '\0') {
        return "missing required field: path";
    }
    if (path_has_dotdot(path)) {
        return "path must not contain '..'";
    }
    size_t len = strlen(path);
    if (len < 4 || strcmp(path + len - 4, ".lua") != 0) {
        return "path must end in .lua";
    }
    for (int i = 0; s_allowed_roots[i]; i++) {
        size_t rlen = strlen(s_allowed_roots[i]);
        if (len > rlen && strncmp(path, s_allowed_roots[i], rlen) == 0) {
            return NULL; /* OK */
        }
    }
    return "path must be under rolfs:/skills/, rolfs:/lib/, vfs:/skills/, vfs:/scripts/ or vfs:/tmp/";
}

/* ---- Helpers ---------------------------------------------------------------- */

/* Read file into malloc'd buffer (caller frees). Returns NULL on error. */
static char *read_file_alloc(const char *path, size_t max_size)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    size_t read_sz = (size_t)sz < max_size ? (size_t)sz : max_size;
    char *buf = malloc(read_sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, read_sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* ---- Restricted require for blessed libs (Inc 4) --------------------------
 *
 * The platform Lua ships a STUBBED package library (lua/lloadlib_stub.c): there
 * is no package.searchers chain — its require() only resolves modules already in
 * _LOADED (the eagerly-required C modules) or package.preload. So the single
 * sandbox concession for blessed libraries is implemented as a thin require()
 * override installed in the sandbox: it
 *   1. returns the cached module if already in _LOADED (covers the C modules
 *      gpio/cjson/... and re-require of an already-loaded lib);
 *   2. for names of the form "lib/<name>", loads the blessed library read-only
 *      from rolfs:/lib/<name>.lua, runs it, caches and returns its result;
 *   3. otherwise errors "module not found" — exactly as before.
 * Restrictions on (2): name must be "lib/<single-component>" — no extra "/",
 * no "\\", no ".." — so the resolved path can never escape rolfs:/lib/. No
 * other filesystem require is possible (no searchers, path/cpath stay empty).
 */
#define LUA_LIB_PREFIX     "lib/"
#define LUA_LIB_PREFIX_LEN 4
#define LUA_LIB_ROOT       "rolfs:/lib/"

/* Load + run rolfs:/lib/<name>.lua, leaving its return value on the stack.
 * On any validation / IO / compile / run error, raises a Lua error. */
static int load_blessed_lib(lua_State *L, const char *name)
{
    /* <name> must be a single, plain component: non-empty, no path separators,
     * no ".." — this is what keeps the lookup inside rolfs:/lib/. */
    if (name[0] == '\0' || strchr(name, '/') != NULL || strchr(name, '\\') != NULL ||
        strstr(name, "..") != NULL) {
        return luaL_error(L, "invalid blessed lib name 'lib/%s'", name);
    }

    char path[160];
    int n = snprintf(path, sizeof(path), "%s%s.lua", LUA_LIB_ROOT, name);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return luaL_error(L, "blessed lib name too long 'lib/%s'", name);
    }

    char *code = read_file_alloc(path, LUA_SCRIPT_MAX);
    if (!code) {
        return luaL_error(L, "blessed lib not found: %s", path);
    }

    char chunkname[176];
    snprintf(chunkname, sizeof(chunkname), "@%s", path);
    int rc = luaL_loadbuffer(L, code, strlen(code), chunkname);
    free(code);
    if (rc != LUA_OK) {
        return lua_error(L);   /* propagate the library's syntax error */
    }
    lua_pushstring(L, name);   /* arg passed to the chunk (modname) */
    lua_call(L, 1, 1);         /* run library; result (its table) on stack */
    return 1;
}

/* Sandbox require(): _LOADED cache → blessed lib (lib/<name>) → error. */
static int sandbox_require(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);

    /* 1. already loaded? (C modules live here after luaL_requiref; libs cache
     *    here after first load) */
    luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);  /* [_LOADED] */
    lua_getfield(L, -1, name);                                 /* [_LOADED][mod] */
    if (!lua_isnil(L, -1)) {
        lua_remove(L, -2);   /* drop _LOADED, keep module */
        return 1;
    }
    lua_pop(L, 1);           /* drop nil; _LOADED still on stack at -1 */

    /* 2. blessed library namespace "lib/<name>"? */
    if (strncmp(name, LUA_LIB_PREFIX, LUA_LIB_PREFIX_LEN) == 0) {
        load_blessed_lib(L, name + LUA_LIB_PREFIX_LEN);  /* [_LOADED][result] */
        /* cache in _LOADED[name] and return it */
        lua_pushvalue(L, -1);          /* [_LOADED][result][result] */
        lua_setfield(L, -3, name);     /* _LOADED[name] = result */
        lua_remove(L, -2);             /* drop _LOADED, keep result */
        return 1;
    }

    /* 3. anything else is not available in the sandbox. */
    return luaL_error(L, "module '%s' not found", name);
}

/* Install the restricted require() into the sandbox, overriding the stub's
 * preload-only require. Keeps C-module resolution (via _LOADED) intact. */
static void install_lib_require(lua_State *L)
{
    lua_pushcfunction(L, sandbox_require);
    lua_setglobal(L, "require");
}

/* ---- JSON → Lua table conversion (#6) --------------------------------------
 *
 * Recursively pushes a cJSON value onto the Lua stack as the natural Lua type:
 *   null    → nil
 *   bool    → boolean
 *   number  → number
 *   string  → string
 *   array   → table with 1-based integer keys
 *   object  → table keyed by member name
 * Scripts therefore receive run(args) where args is a ready-to-use table; no
 * cjson.decode boilerplate. A recursion-depth guard prevents stack abuse from a
 * deeply nested (LLM-supplied) object.
 */
#define JSON_PUSH_MAX_DEPTH 32

static void json_push_value(lua_State *L, const cJSON *v, int depth)
{
    if (depth > JSON_PUSH_MAX_DEPTH || !v) {
        lua_pushnil(L);
        return;
    }
    if (cJSON_IsNull(v) || cJSON_IsInvalid(v)) {
        lua_pushnil(L);
    } else if (cJSON_IsTrue(v)) {
        lua_pushboolean(L, 1);
    } else if (cJSON_IsFalse(v)) {
        lua_pushboolean(L, 0);
    } else if (cJSON_IsNumber(v)) {
        lua_pushnumber(L, (lua_Number)v->valuedouble);
    } else if (cJSON_IsString(v)) {
        lua_pushstring(L, v->valuestring ? v->valuestring : "");
    } else if (cJSON_IsArray(v)) {
        lua_newtable(L);
        int idx = 1;
        const cJSON *child;
        cJSON_ArrayForEach(child, v) {
            json_push_value(L, child, depth + 1);   /* value */
            lua_rawseti(L, -2, idx++);              /* t[idx] = value */
        }
    } else if (cJSON_IsObject(v)) {
        lua_newtable(L);
        const cJSON *child;
        cJSON_ArrayForEach(child, v) {
            if (!child->string) continue;
            lua_pushstring(L, child->string);       /* key */
            json_push_value(L, child, depth + 1);   /* value */
            lua_rawset(L, -3);                      /* t[key] = value */
        }
    } else {
        lua_pushnil(L);
    }
}

/* Push the decoded args object as a Lua table (empty table if NULL/empty/not
 * an object). Always leaves exactly one table on the stack. */
/* ---- Concurrency constants / globals (defined early; used by lua_exec_task
 *      and cap_lua_run which appear before the full async-jobs section). ---- */
#define LUA_JOB_MAX_RUNNING  2   /* shared cap: sync + async together        */
static rtos_mutex_t s_jobs_lock = NULL;
static int          s_sync_lua_run_count = 0; /* sync lua_run calls in flight */
static int          job_count_active(void);   /* forward decl; body is later  */

static void push_args_table(lua_State *L, const cJSON *args_obj)
{
    if (args_obj && (cJSON_IsObject(args_obj) || cJSON_IsArray(args_obj))) {
        json_push_value(L, args_obj, 0);
    } else {
        lua_newtable(L);   /* missing / null / scalar → empty table */
    }
}

/* ---- Async execution (ported from old cap_skill_run) ----------------------- */

typedef struct {
    /* Lua VM creation (newstate, sandbox, load, pcall) is done inside
     * lua_exec_task on its own 8 KB stack — not on the caller's stack.
     * The caller only fills in path + args_json; the task does the rest. */
    char        *path;        /* heap-allocated; freed by task                 */
    char        *args_json;   /* JSON args string; heap-allocated; freed by task */
    char        *result;      /* heap-allocated on success                     */
    int          lua_rc;
    volatile int cancel;
    volatile int abandoned;   /* set by caller on hard-timeout; task self-frees */
    rtos_sema_t  done;
    /* print() capture for lua_run (sync path) */
    char         stdout_buf[2048];
    size_t       stdout_len;
} lua_exec_ctx_t;

static void lua_cancel_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    lua_getfield(L, LUA_REGISTRYINDEX, "__cancel_ptr");
    volatile int *p = (volatile int *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (p && *p) luaL_error(L, "lua execution cancelled (timeout)");

    /* Async jobs also enforce their own wall-clock deadline here (the sync path
     * leaves __deadline_ms == 0 and is timed out by its parent task instead). */
    lua_getfield(L, LUA_REGISTRYINDEX, "__deadline_ms");
    lua_Integer deadline = lua_tointeger(L, -1);
    lua_pop(L, 1);
    if (deadline != 0 &&
        (lua_Integer)rtos_time_get_current_system_time_ms() >= deadline) {
        luaL_error(L, "lua execution timed out");
    }
}

/* Format Lua print() args into line (tab-joined, newline-terminated).
 * Returns the number of bytes written (always <= size). */
static size_t lua_format_print_line(lua_State *L, char *line, size_t size)
{
    int    nargs = lua_gettop(L);
    size_t off   = 0;
    for (int i = 1; i <= nargs; i++) {
        const char *s = lua_tostring(L, i);
        if (!s) s = lua_isnil(L, i) ? "nil" : "?";
        if (i > 1 && off < size - 1) line[off++] = '\t';
        size_t sl = strlen(s);
        if (off + sl > size - 1) sl = size - 1 - off;
        memcpy(line + off, s, sl);
        off += sl;
        if (off >= size - 1) break;
    }
    if (off < size) line[off++] = '\n';
    return off;
}

/* Forward declarations */
static int  lua_capture_print(lua_State *L);  /* defined after job_log_append */
static void install_sandbox(lua_State *L);    /* defined after cap_lua_run    */

static void lua_exec_task(void *param)
{
    lua_exec_ctx_t *ctx = (lua_exec_ctx_t *)param;

    /* ---- All Lua VM work runs here on this task's own 8 KB stack ----------- */
    lua_State *L = luaL_newstate();
    if (!L) {
        RTK_LOGE(TAG, "lua_exec_task: failed to create Lua state\n");
        ctx->lua_rc = LUA_ERRMEM;
        goto done;
    }
    install_sandbox(L);

    char *file_code = read_file_alloc(ctx->path, LUA_SCRIPT_MAX);
    if (!file_code) {
        RTK_LOGE(TAG, "lua_exec_task: script not found: %s\n", ctx->path);
        ctx->lua_rc = LUA_ERRFILE;
        lua_close(L);
        goto done;
    }
    {
        int rc_load = luaL_loadstring(L, file_code);
        free(file_code);
        if (rc_load != LUA_OK) {
            RTK_LOGE(TAG, "lua_exec_task: load failed: %s\n", lua_tostring(L, -1));
            ctx->lua_rc = rc_load;
            lua_close(L);
            goto done;
        }
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        RTK_LOGE(TAG, "lua_exec_task: top-level error: %s\n", lua_tostring(L, -1));
        ctx->lua_rc = LUA_ERRRUN;
        lua_close(L);
        goto done;
    }
    lua_getglobal(L, "run");
    if (!lua_isfunction(L, -1)) {
        RTK_LOGE(TAG, "lua_exec_task: no run() in %s\n", ctx->path);
        ctx->lua_rc = LUA_ERRRUN;
        lua_close(L);
        goto done;
    }

    /* Register cancel pointer so sleep_ms and the hook can see it. */
    lua_pushlightuserdata(L, (void *)&ctx->cancel);
    lua_setfield(L, LUA_REGISTRYINDEX, "__cancel_ptr");
    lua_sethook(L, lua_cancel_hook, LUA_MASKCOUNT, LUA_CANCEL_INSTR_FREQ);

    /* Capture print() into ctx->stdout_buf. */
    lua_pushlightuserdata(L, (void *)ctx);
    lua_setfield(L, LUA_REGISTRYINDEX, "__print_cap");
    lua_pushcfunction(L, lua_capture_print);
    lua_setglobal(L, "print");

    /* run(args) */
#ifdef CLAW_LUA_TIME_LOG_ENABLE
    uint32_t exec_start_ms = rtos_time_get_current_system_time_ms();
    RTK_LOGI(TAG, "lua_exec START path=%s t=%u\n", ctx->path, (unsigned)exec_start_ms);
#endif
    {
        cJSON *args_obj = ctx->args_json ? cJSON_Parse(ctx->args_json) : cJSON_CreateObject();
        free(ctx->args_json); ctx->args_json = NULL;
        free(ctx->path);      ctx->path = NULL;
        push_args_table(L, args_obj);
        cJSON_Delete(args_obj);
    }
    ctx->lua_rc = lua_pcall(L, 1, 1, 0);

#ifdef CLAW_LUA_TIME_LOG_ENABLE
    RTK_LOGI(TAG, "lua_exec DONE path=%s rc=%d elapsed=%ums\n",
             ctx->path, ctx->lua_rc,
             (unsigned)(rtos_time_get_current_system_time_ms() - exec_start_ms));
#endif

    /* Read abandoned flag before giving the semaphore to avoid UAF. If the
     * caller already hard-timed-out and set abandoned=1, it will not touch ctx
     * again — this task owns cleanup. */
    if (ctx->lua_rc == LUA_OK) {
        const char *r = lua_tostring(L, -1);
        ctx->result = r ? strdup(r) : strdup("{\"result\":null}");
    } else {
        const char *err = lua_tostring(L, -1);
        RTK_LOGE(TAG, "lua run() failed: %s\n", err ? err : "unknown");
    }

    lua_close(L);

done:
    /* Free any un-consumed context strings (early-exit paths above). */
    free(ctx->path);
    free(ctx->args_json);
    ctx->path = NULL;
    ctx->args_json = NULL;

    int was_abandoned = ctx->abandoned;
    rtos_sema_give(ctx->done);

    if (was_abandoned) {
        free(ctx->result);
        rtos_sema_delete(ctx->done);
        free(ctx);
    }

    /* Release the sync-run concurrency slot. */
    if (rtos_mutex_take(s_jobs_lock, RTOS_MAX_DELAY) == RTK_SUCCESS) {
        if (s_sync_lua_run_count > 0) s_sync_lua_run_count--;
        rtos_mutex_give(s_jobs_lock);
    }

    rtos_task_delete(NULL);
}

/* ---- Sandbox setup (strict — UNCHANGED from previous cap_skill_run) ---------
 *
 * Opens only a safe subset of the standard library. io/os/debug are
 * intentionally excluded: scripts must not access arbitrary files, run shell
 * commands, or inspect raw memory / break out via the debug library. package is
 * loaded for require() of pre-registered C modules. The require() implementation
 * is then overridden (install_lib_require) with a restricted version that also
 * resolves require("lib/<name>") to the read-only rolfs:/lib/<name>.lua and
 * nothing else — see Inc 4.
 */
static void install_sandbox(lua_State *L)
{
    luaopen_base(L); lua_pop(L, 1);
    {
        static const char *s_kill[] = {
            "load", "loadfile", "dofile",
            "rawget", "rawset", "rawequal", "rawlen",
            NULL
        };
        for (int i = 0; s_kill[i]; i++) {
            lua_pushnil(L); lua_setglobal(L, s_kill[i]);
        }
    }
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math,  1); lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME,  luaopen_table, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_LOADLIBNAME, luaopen_package, 1); lua_pop(L, 1);
    lua_getglobal(L, "package");
    if (lua_istable(L, -1)) {
        lua_pushnil(L); lua_setfield(L, -2, "loadlib");
        lua_pushstring(L, ""); lua_setfield(L, -2, "path");
        lua_pushstring(L, ""); lua_setfield(L, -2, "cpath");
    }
    lua_pop(L, 1);

    /* Install the sandbox-safe Ameba modules (gpio/i2c/rtc/sys/cjson/timer/
     * cap/file, plus USB when built). Membership lives in the registry. They are
     * eagerly required, so they land in _LOADED before our require() override. */
    lua_module_registry_install(L, LUA_MOD_ENV_SKILL);

    /* Inc 4: the single sandbox concession — override the stub's preload-only
     * require() with one that additionally serves blessed libraries from the
     * read-only rolfs:/lua/lib/ directory (require("lib/<name>")). Installed last
     * so it replaces the require() set up by luaopen_package above. */
    install_lib_require(L);
}

/* ---- execute: lua_run ------------------------------------------------------ */

static int cap_lua_run(const char *input_json,
                       const claw_cap_call_context_t *ctx,
                       char **output)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        claw_cap_set_output(output, "{\"error\":\"invalid input JSON\"}");
        return RTK_FAIL;
    }

    cJSON *jpath = cJSON_GetObjectItem(root, "path");
    if (!jpath || !cJSON_IsString(jpath) || !jpath->valuestring) {
        claw_cap_set_output(output, "{\"error\":\"missing required field: path\"}");
        cJSON_Delete(root);
        return RTK_FAIL;
    }
    /* Copy path to a stack buffer before any cJSON_Delete(root) so the pointer
     * stays valid for error logging in the hard-timeout path below. */
    char path[128];
    strncpy(path, jpath->valuestring, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    const char *perr = validate_lua_path(path);
    if (perr) {
        claw_cap_set_output(output, "{\"error\":\"%s\"}", perr);
        cJSON_Delete(root);
        return RTK_FAIL;
    }

    /* Invariant: vfs:/tmp/ is throwaway scratch wiped on every reboot.
     * Executing a script from there via a scheduled (non-LLM) call would
     * silently do nothing after a restart.  Reject early so the scheduler
     * (or any internal caller) gets an actionable error rather than a
     * confusing "file not found" at fire time.
     * LLM callers (CLAW_CAP_CALLER_LLM) are allowed — the LLM writes the
     * script itself and then runs it in the same session, so it knows the
     * file is transient. */
    if (strncmp(path, "vfs:/tmp/", 9) == 0 &&
        ctx && ctx->caller != CLAW_CAP_CALLER_LLM) {
        cJSON_Delete(root);
        claw_cap_set_output(output,
            "{\"error\":\"vfs:/tmp/ scripts are wiped on reboot and cannot be used "
            "in scheduled or internal calls. Use vfs:/scripts/ for persistent scripts "
            "or vfs:/skills/ for skill-managed scripts.\"}");
        return RTK_FAIL;
    }

    /* Optional per-call timeout. 0 / absent → default. */
    int timeout_ms = LUA_EXEC_TIMEOUT_MS;
    cJSON *jto = cJSON_GetObjectItem(root, "timeout_ms");
    if (jto && cJSON_IsNumber(jto) && jto->valueint > 0) {
        timeout_ms = jto->valueint;
    }

#ifdef CLAW_LUA_TIME_LOG_ENABLE
    uint32_t run_start_ms = rtos_time_get_current_system_time_ms();
    RTK_LOGI(TAG, "lua_run START path=%s timeout=%dms t=%u\n",
             path, timeout_ms, (unsigned)run_start_ms);
#endif

    /* Detach the args object; serialise to JSON string so the exec task can
     * parse it independently on its own stack. */
    cJSON *args_obj = cJSON_DetachItemFromObject(root, "args");
    char  *args_json = args_obj ? cJSON_PrintUnformatted(args_obj) : NULL;
    cJSON_Delete(args_obj);
    cJSON_Delete(root);

    /* Concurrency gate: lua_run (sync) shares the LUA_JOB_MAX_RUNNING budget
     * with lua_run_async jobs.  Timer-driven calls must NOT bypass this limit —
     * excess concurrent lua_States exhaust heap and corrupt adjacent allocations.
     * If we are at capacity, drop this call (the next timer tick will retry). */
    if (rtos_mutex_take(s_jobs_lock, 100) == RTK_SUCCESS) {
        int total = s_sync_lua_run_count + job_count_active();
        if (total >= LUA_JOB_MAX_RUNNING) {
            rtos_mutex_give(s_jobs_lock);
            claw_cap_set_output(output,
                "{\"error\":\"lua busy (max %d concurrent runs; retry later)\"}",
                LUA_JOB_MAX_RUNNING);
            free(args_json);
            return RTK_FAIL;
        }
        s_sync_lua_run_count++;
        rtos_mutex_give(s_jobs_lock);
    }

    /* Run in a separate task so we don't block the calling (agent/AT) task. The
     * ctx is heap-allocated so its pointer stays valid even if this function
     * returns early (hard timeout) before the task exits. */
/* Decrement s_sync_lua_run_count on early-exit error paths that occur after
 * the concurrency gate has already incremented it.
 * Use RTOS_MAX_DELAY: skipping this decrement would permanently leak a slot
 * and cause spurious "lua busy" errors; a timeout here must never happen. */
#define SYNC_COUNT_RELEASE() \
    do { \
        if (rtos_mutex_take(s_jobs_lock, RTOS_MAX_DELAY) == RTK_SUCCESS) { \
            if (s_sync_lua_run_count > 0) s_sync_lua_run_count--; \
            rtos_mutex_give(s_jobs_lock); \
        } \
    } while (0)

    lua_exec_ctx_t *exec = calloc(1, sizeof(*exec));
    if (!exec) {
        SYNC_COUNT_RELEASE();
        claw_cap_set_output(output, "{\"error\":\"out of memory\"}");
        free(args_json);
        return RTK_FAIL;
    }
    exec->path      = strdup(path);
    exec->args_json = args_json;   /* task owns it */
    exec->lua_rc    = LUA_OK;
    if (!exec->path) {
        SYNC_COUNT_RELEASE();
        claw_cap_set_output(output, "{\"error\":\"out of memory\"}");
        free(args_json); free(exec);
        return RTK_FAIL;
    }

    if (rtos_sema_create_binary(&exec->done) != RTK_SUCCESS) {
        SYNC_COUNT_RELEASE();
        claw_cap_set_output(output, "{\"error\":\"failed to create semaphore\"}");
        free(exec->path); free(exec->args_json); free(exec);
        return RTK_FAIL;
    }

    rtos_task_t lua_run_task = NULL;
    if (rtos_task_create(&lua_run_task, "lua_run", lua_exec_task,
                         exec, 8192, 1) != RTK_SUCCESS) {
        SYNC_COUNT_RELEASE();
        claw_cap_set_output(output, "{\"error\":\"failed to spawn lua task\"}");
        rtos_sema_delete(exec->done);
        free(exec->path); free(exec->args_json); free(exec);
        return RTK_FAIL;
    }

    /* Wait for completion. On timeout, signal cancel and give the task up to 2
     * more seconds to exit cleanly (cancel hook fires within ~10 ms). */
    bool task_done = (rtos_sema_take(exec->done, timeout_ms) == RTK_SUCCESS);
    if (!task_done) {
        exec->cancel = 1;
        task_done = (rtos_sema_take(exec->done, 2000) == RTK_SUCCESS);
    }

    if (!task_done) {
        /* Task did not respond to cancel — transfer ownership to the task via
         * the abandoned flag so it cleans up exec/sema when it eventually exits. */
        exec->abandoned = 1;
        RTK_LOGE(TAG, "lua_run '%s' hard-timeout: exec ownership transferred to task\n", path);
        claw_cap_set_output(output,
            "{\"error\":\"lua_run '%s' timed out after %dms\"}", path, timeout_ms);
        return RTK_FAIL;
    }

    int   lua_rc    = exec->lua_rc;
    char *result    = exec->result;
    bool  cancelled = (bool)exec->cancel;

    /* Save captured stdout before freeing exec. */
    char *stdout_str = NULL;
    if (exec->stdout_len > 0) {
        stdout_str = malloc(exec->stdout_len + 1);
        if (stdout_str) {
            memcpy(stdout_str, exec->stdout_buf, exec->stdout_len);
            stdout_str[exec->stdout_len] = '\0';
        }
    }

    rtos_sema_delete(exec->done);
    free(exec);

#ifdef CLAW_LUA_TIME_LOG_ENABLE
    RTK_LOGI(TAG, "lua_run EXEC path=%s rc=%d elapsed=%ums\n",
             path, lua_rc,
             (unsigned)(rtos_time_get_current_system_time_ms() - run_start_ms));
#endif

    if (lua_rc != LUA_OK || !result) {
        free(stdout_str);
        if (cancelled) {
            claw_cap_set_output(output,
                "{\"error\":\"lua_run '%s' timed out after %dms\"}", path, timeout_ms);
        } else {
            claw_cap_set_output(output,
                "{\"error\":\"lua_run '%s' execution failed\"}", path);
        }
        /* lua_close is called by lua_exec_task. */
        return RTK_FAIL;
    }

    int set_rc;
    if (stdout_str) {
        /* JSON-escape stdout, then wrap result + stdout in one envelope. */
        size_t slen = strlen(stdout_str);
        char *esc = malloc(slen * 2 + 1);
        if (esc) {
            size_t eo = 0;
            for (size_t i = 0; i < slen; i++) {
                unsigned char c = (unsigned char)stdout_str[i];
                switch (c) {
                case '"':  esc[eo++] = '\\'; esc[eo++] = '"';  break;
                case '\\': esc[eo++] = '\\'; esc[eo++] = '\\'; break;
                case '\n': esc[eo++] = '\\'; esc[eo++] = 'n';  break;
                case '\r': esc[eo++] = '\\'; esc[eo++] = 'r';  break;
                case '\t': esc[eo++] = '\\'; esc[eo++] = 't';  break;
                default:
                    if (c >= 0x20) esc[eo++] = (char)c;
                    break;
                }
            }
            esc[eo] = '\0';
            set_rc = claw_cap_set_output(output,
                "{\"result\":%s,\"stdout\":\"%s\"}", result, esc);
            free(esc);
        } else {
            set_rc = claw_cap_set_output(output, "%s", result);
        }
        free(stdout_str);
    } else {
        set_rc = claw_cap_set_output(output, "%s", result);
    }
    free(result);
    /* lua_close is called by lua_exec_task. */
    return set_rc;
}

/* ============================================================================
 * Inc 7: asynchronous Lua jobs (full tier). Reuses the strict sandbox,
 * restricted require and cancel hook above.
 * ==========================================================================*/

/* Bounded resources sized for the device memory budget. */
#define LUA_JOB_SLOTS        4       /* fixed job table size                  */
/* LUA_JOB_MAX_RUNNING defined earlier (shared with sync lua_run). */
#define LUA_JOB_LOG_SIZE     2048    /* per-job ring log buffer (bytes)        */
#define LUA_JOB_NAME_MAX     32      /* job name / exclusive group max length  */
#define LUA_JOB_PATH_MAX     128
#define LUA_JOB_STOP_WAIT_MS 2000    /* lua_job_stop grace period              */

typedef enum {
    LUA_JOB_FREE = 0,    /* slot unused / reusable                            */
    LUA_JOB_QUEUED,      /* task created, run() not started yet               */
    LUA_JOB_RUNNING,
    LUA_JOB_DONE,        /* run() returned OK                                 */
    LUA_JOB_FAILED,      /* run() raised an error                             */
    LUA_JOB_TIMEOUT,     /* hit its wall-clock timeout                        */
    LUA_JOB_STOPPED,     /* stopped on request                               */
} lua_job_status_t;

static const char *job_status_str(lua_job_status_t s)
{
    switch (s) {
    case LUA_JOB_QUEUED:  return "QUEUED";
    case LUA_JOB_RUNNING: return "RUNNING";
    case LUA_JOB_DONE:    return "DONE";
    case LUA_JOB_FAILED:  return "FAILED";
    case LUA_JOB_TIMEOUT: return "TIMEOUT";
    case LUA_JOB_STOPPED: return "STOPPED";
    case LUA_JOB_FREE:    default: return "FREE";
    }
}

static bool job_status_terminal(lua_job_status_t s)
{
    return s == LUA_JOB_DONE || s == LUA_JOB_FAILED ||
           s == LUA_JOB_TIMEOUT || s == LUA_JOB_STOPPED;
}

typedef struct {
    uint32_t          job_id;        /* 0 == free slot                        */
    char              name[LUA_JOB_NAME_MAX];      /* optional, "" if none    */
    char              exclusive[LUA_JOB_NAME_MAX]; /* optional group, "" none */
    char              path[LUA_JOB_PATH_MAX];
    lua_job_status_t  status;
    int               timeout_ms;
    volatile int      stop_requested; /* read by cancel hook / sleep          */
    rtos_task_t       task;
    /* ring log (LUA_JOB_LOG_SIZE bytes, overwrites oldest) */
    char              log[LUA_JOB_LOG_SIZE];
    uint32_t          log_seq;        /* total bytes ever written (monotonic)  */
    size_t            log_len;        /* bytes currently held (<= LOG_SIZE)    */
    size_t            log_head;       /* ring start index of oldest byte       */
    bool              log_truncated;  /* set once we start overwriting         */
    uint32_t          started_ms;
    uint32_t          finished_ms;    /* 0 until terminal                      */
    char             *result;         /* run() return string, heap (terminal)  */
} lua_job_t;

static lua_job_t  s_jobs[LUA_JOB_SLOTS];
/* s_jobs_lock and s_sync_lua_run_count defined earlier. */
static uint32_t   s_next_job_id = 1;

/* Per-task parameter handed to the async task.
 * The task owns all Lua VM creation (luaL_newstate + install_sandbox +
 * luaL_loadstring) so that the parse/compile step runs on the lua_job
 * task's stack rather than the caller's (scheduler, claw_agent, etc.). */
typedef struct {
    char      *path;        /* script path; heap-allocated, freed by task    */
    char      *args_json;   /* JSON args string; heap-allocated, freed by task */
    uint32_t   job_id;      /* identifies the slot                           */
    int        timeout_ms;
} lua_async_ctx_t;

/* ---- ring log -------------------------------------------------------------- */

/* Append raw bytes to a job's ring log. Caller holds s_jobs_lock. */
static void job_log_append(lua_job_t *j, const char *data, size_t n)
{
    j->log_seq += (uint32_t)n;
    for (size_t i = 0; i < n; i++) {
        size_t tail = (j->log_head + j->log_len) % LUA_JOB_LOG_SIZE;
        j->log[tail] = data[i];
        if (j->log_len < LUA_JOB_LOG_SIZE) {
            j->log_len++;
        } else {
            /* full: advance head, oldest byte dropped */
            j->log_head = (j->log_head + 1) % LUA_JOB_LOG_SIZE;
            j->log_truncated = true;
        }
    }
}

/* Unified print() capture for both sync (lua_run) and async (lua_run_async).
 * Sync:  __print_cap → lua_exec_ctx_t.stdout_buf (flat buffer)
 * Async: __job_slot  → lua_job_t ring log via job_log_append */
static int lua_capture_print(lua_State *L)
{
    char   line[256];
    size_t n = lua_format_print_line(L, line, sizeof(line));

    lua_getfield(L, LUA_REGISTRYINDEX, "__print_cap");
    lua_exec_ctx_t *ctx = (lua_exec_ctx_t *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (ctx) {
        size_t avail = sizeof(ctx->stdout_buf) - 1 - ctx->stdout_len;
        if (avail > 0) {
            size_t w = n < avail ? n : avail;
            memcpy(ctx->stdout_buf + ctx->stdout_len, line, w);
            ctx->stdout_len += w;
            ctx->stdout_buf[ctx->stdout_len] = '\0';
        }
        return 0;
    }

    lua_getfield(L, LUA_REGISTRYINDEX, "__job_slot");
    lua_job_t *slot = (lua_job_t *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (slot && rtos_mutex_take(s_jobs_lock, 1000) == RTK_SUCCESS) {
        if (slot->job_id != 0) job_log_append(slot, line, n);
        rtos_mutex_give(s_jobs_lock);
    }
    return 0;
}

/* Copy the ring log into `out` (linearised, oldest→newest), NUL-terminated.
 * Caller holds s_jobs_lock. Returns bytes written (excluding NUL). */
static size_t job_log_read(const lua_job_t *j, char *out, size_t out_cap)
{
    if (out_cap == 0) return 0;
    size_t n = j->log_len;
    if (n > out_cap - 1) n = out_cap - 1;   /* leave room for NUL            */
    /* If we clamp, keep the NEWEST n bytes (most useful tail). */
    size_t skip = (j->log_len > n) ? (j->log_len - n) : 0;
    size_t start = (j->log_head + skip) % LUA_JOB_LOG_SIZE;
    for (size_t i = 0; i < n; i++) {
        out[i] = j->log[(start + i) % LUA_JOB_LOG_SIZE];
    }
    out[n] = '\0';
    return n;
}

/* ---- slot lookup (caller holds lock) --------------------------------------- */

static lua_job_t *job_find_by_id(uint32_t id)
{
    if (id == 0) return NULL;
    for (int i = 0; i < LUA_JOB_SLOTS; i++) {
        if (s_jobs[i].job_id == id) return &s_jobs[i];
    }
    return NULL;
}

static int job_count_active(void)
{
    int n = 0;
    for (int i = 0; i < LUA_JOB_SLOTS; i++) {
        if (s_jobs[i].job_id != 0 && !job_status_terminal(s_jobs[i].status)) {
            n++;
        }
    }
    return n;
}

static lua_job_t *job_find_active_by_name(const char *name)
{
    if (!name || name[0] == '\0') return NULL;
    for (int i = 0; i < LUA_JOB_SLOTS; i++) {
        lua_job_t *j = &s_jobs[i];
        if (j->job_id != 0 && !job_status_terminal(j->status) &&
            strcmp(j->name, name) == 0) {
            return j;
        }
    }
    return NULL;
}

static lua_job_t *job_find_active_by_exclusive(const char *grp)
{
    if (!grp || grp[0] == '\0') return NULL;
    for (int i = 0; i < LUA_JOB_SLOTS; i++) {
        lua_job_t *j = &s_jobs[i];
        if (j->job_id != 0 && !job_status_terminal(j->status) &&
            strcmp(j->exclusive, grp) == 0) {
            return j;
        }
    }
    return NULL;
}

/* Find a reusable slot: a never-used one first, else any terminal slot. */
static lua_job_t *job_find_free_slot(void)
{
    for (int i = 0; i < LUA_JOB_SLOTS; i++) {
        if (s_jobs[i].job_id == 0) return &s_jobs[i];
    }
    for (int i = 0; i < LUA_JOB_SLOTS; i++) {
        if (job_status_terminal(s_jobs[i].status)) return &s_jobs[i];
    }
    return NULL;
}

/* ---- async exec task ------------------------------------------------------- */

static void lua_async_task(void *param)
{
    lua_async_ctx_t *ac = (lua_async_ctx_t *)param;
    uint32_t   id = ac->job_id;

    /* Find slot early so we can record FAILED if Lua init fails. */
    volatile int *stop_ptr = NULL;
    lua_job_t   *slot = NULL;
    if (rtos_mutex_take(s_jobs_lock, RTOS_MAX_DELAY) == RTK_SUCCESS) {
        lua_job_t *j = job_find_by_id(id);
        if (j) {
            j->status = LUA_JOB_RUNNING;
            j->started_ms = rtos_time_get_current_system_time_ms();
            stop_ptr = &j->stop_requested;
            slot = j;
        }
        rtos_mutex_give(s_jobs_lock);
    }
    uint32_t async_start_ms = slot ? slot->started_ms : rtos_time_get_current_system_time_ms();
    RTK_LOGI(TAG, "lua_async_task START job_id=%u path=%s t=%u\n",
             (unsigned)id, slot ? slot->path : "?", (unsigned)async_start_ms);

    /* ---- All Lua VM work (newstate, sandbox, load, pcall) runs here on the  ----
     * ---- lua_job task's own stack — not on the caller's (scheduler/agent).  ---- */
    lua_State *L = luaL_newstate();
    if (!L) {
        RTK_LOGE(TAG, "lua_async_task: failed to create Lua state\n");
        free(ac->path); free(ac->args_json); free(ac);
        goto mark_failed;
    }
    install_sandbox(L);

    char *file_code = read_file_alloc(ac->path, LUA_SCRIPT_MAX);
    if (!file_code) {
        RTK_LOGE(TAG, "lua_async_task: script not found: %s\n", ac->path);
        lua_close(L);
        free(ac->path); free(ac->args_json); free(ac);
        goto mark_failed;
    }
    {
        int rc_load = luaL_loadstring(L, file_code);
        free(file_code);
        if (rc_load != LUA_OK) {
            RTK_LOGE(TAG, "lua_async_task: load failed: %s\n", lua_tostring(L, -1));
            lua_close(L);
            free(ac->path); free(ac->args_json); free(ac);
            goto mark_failed;
        }
    }
    /* Execute top-level (defines run()) */
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        RTK_LOGE(TAG, "lua_async_task: top-level error: %s\n", lua_tostring(L, -1));
        lua_close(L);
        free(ac->path); free(ac->args_json); free(ac);
        goto mark_failed;
    }
    lua_getglobal(L, "run");
    if (!lua_isfunction(L, -1)) {
        RTK_LOGE(TAG, "lua_async_task: no run() in %s\n", ac->path);
        lua_close(L);
        free(ac->path); free(ac->args_json); free(ac);
        goto mark_failed;
    }

    /* Stash slot pointer (for print capture) + cancel pointer in the registry. */
    lua_pushlightuserdata(L, (void *)slot);
    lua_setfield(L, LUA_REGISTRYINDEX, "__job_slot");
    lua_pushlightuserdata(L, (void *)stop_ptr);
    lua_setfield(L, LUA_REGISTRYINDEX, "__cancel_ptr");
    uint32_t deadline_ms = rtos_time_get_current_system_time_ms() + (uint32_t)ac->timeout_ms;
    lua_pushinteger(L, (lua_Integer)deadline_ms);
    lua_setfield(L, LUA_REGISTRYINDEX, "__deadline_ms");
    lua_sethook(L, lua_cancel_hook, LUA_MASKCOUNT, LUA_CANCEL_INSTR_FREQ);

    lua_pushcfunction(L, lua_capture_print);
    lua_setglobal(L, "print");

    /* parse args JSON and push args table */
    {
        cJSON *args_obj = ac->args_json ? cJSON_Parse(ac->args_json) : cJSON_CreateObject();
        free(ac->args_json); ac->args_json = NULL;
        free(ac->path);      ac->path = NULL;
        free(ac);            ac = NULL;
        push_args_table(L, args_obj);
        cJSON_Delete(args_obj);
    }

    int rc = lua_pcall(L, 1, 1, 0);

    char *result = NULL;
    lua_job_status_t final;
    bool stopped_flag = (stop_ptr && *stop_ptr);

    if (rc == LUA_OK) {
        const char *r = lua_tostring(L, -1);
        result = r ? strdup(r) : strdup("{\"result\":null}");
        final = LUA_JOB_DONE;
    } else {
        const char *err = lua_tostring(L, -1);
        /* Classify by which interruption fired: the deadline hook raises a
         * message ending "timed out"; an explicit stop fires the cancel hook
         * ("cancelled (timeout)") with stop_requested set. (A slight clock skew
         * around the deadline made a pure now>=deadline test unreliable.) */
        bool timed_out = (err && strstr(err, "timed out") != NULL);
        char buf[160];
        DiagSnPrintf(buf, sizeof(buf), "run() error: %s", err ? err : "unknown");
        /* record the error text into the log too, so lua_job_get shows it */
        if (rtos_mutex_take(s_jobs_lock, 1000) == RTK_SUCCESS) {
            lua_job_t *j = job_find_by_id(id);
            if (j) { job_log_append(j, buf, strlen(buf)); job_log_append(j, "\n", 1); }
            rtos_mutex_give(s_jobs_lock);
        }
        result = strdup(err ? err : "unknown");
        /* Classify the failure: an explicit stop wins, then a deadline hit, then
         * a genuine script error. (A stop near the deadline is still a stop.) */
        if (stopped_flag)      final = LUA_JOB_STOPPED;
        else if (timed_out)    final = LUA_JOB_TIMEOUT;
        else                   final = LUA_JOB_FAILED;
    }

    /* Mark terminal in the slot. */
    if (rtos_mutex_take(s_jobs_lock, RTOS_MAX_DELAY) == RTK_SUCCESS) {
        lua_job_t *j = job_find_by_id(id);
        if (j) {
            j->status = final;
            j->finished_ms = rtos_time_get_current_system_time_ms();
            free(j->result);
            j->result = result;
            result = NULL;
            j->task = NULL;
        }
        rtos_mutex_give(s_jobs_lock);
    }
    free(result);  /* only non-NULL if slot vanished (shouldn't happen) */

    RTK_LOGI(TAG, "lua_async_task END job_id=%u status=%s elapsed=%ums\n",
             (unsigned)id, job_status_str(final),
             (unsigned)(rtos_time_get_current_system_time_ms() - async_start_ms));

    lua_close(L);
    rtos_task_delete(NULL);
    return;

mark_failed:
    /* Lua init/load failed before the main path — mark slot as FAILED. */
    if (rtos_mutex_take(s_jobs_lock, RTOS_MAX_DELAY) == RTK_SUCCESS) {
        lua_job_t *j = job_find_by_id(id);
        if (j) {
            j->status = LUA_JOB_FAILED;
            j->finished_ms = rtos_time_get_current_system_time_ms();
        }
        rtos_mutex_give(s_jobs_lock);
    }
    RTK_LOGI(TAG, "lua_async_task END job_id=%u status=FAILED elapsed=%ums\n",
             (unsigned)id,
             (unsigned)(rtos_time_get_current_system_time_ms() - async_start_ms));
    rtos_task_delete(NULL);
}

/* ---- copy a short string field safely -------------------------------------- */
static void copy_field(char *dst, size_t cap, const cJSON *obj, const char *key)
{
    dst[0] = '\0';
    const cJSON *it = obj ? cJSON_GetObjectItem(obj, key) : NULL;
    if (it && cJSON_IsString(it) && it->valuestring) {
        size_t n = strlen(it->valuestring);
        if (n > cap - 1) n = cap - 1;
        memcpy(dst, it->valuestring, n);
        dst[n] = '\0';
    }
}

/* ---- execute: lua_run_async ------------------------------------------------ */

static int cap_lua_run_async(const char *input_json,
                             const claw_cap_call_context_t *ctx,
                             char **output)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        claw_cap_set_output(output, "{\"error\":\"invalid input JSON\"}");
        return RTK_FAIL;
    }

    cJSON *jpath = cJSON_GetObjectItem(root, "path");
    if (!jpath || !cJSON_IsString(jpath) || !jpath->valuestring) {
        claw_cap_set_output(output, "{\"error\":\"missing required field: path\"}");
        cJSON_Delete(root);
        return RTK_FAIL;
    }
    const char *perr = validate_lua_path(jpath->valuestring);
    if (perr) {
        claw_cap_set_output(output, "{\"error\":\"%s\"}", perr);
        cJSON_Delete(root);
        return RTK_FAIL;
    }
    if (strlen(jpath->valuestring) >= LUA_JOB_PATH_MAX) {
        claw_cap_set_output(output, "{\"error\":\"path too long for async job\"}");
        cJSON_Delete(root);
        return RTK_FAIL;
    }
    /* Same invariant as lua_run: vfs:/tmp/ scripts are wiped on reboot and
     * must not be enqueued as background jobs from non-LLM callers. */
    if (strncmp(jpath->valuestring, "vfs:/tmp/", 9) == 0 &&
        ctx && ctx->caller != CLAW_CAP_CALLER_LLM) {
        cJSON_Delete(root);
        claw_cap_set_output(output,
            "{\"error\":\"vfs:/tmp/ scripts are wiped on reboot and cannot be used "
            "in scheduled or internal calls. Use vfs:/scripts/ for persistent scripts "
            "or vfs:/skills/ for skill-managed scripts.\"}");
        return RTK_FAIL;
    }
    /* Copy path into a local buffer: jpath->valuestring belongs to `root`, which
     * we delete (after detaching args) well before read_file_alloc / the slot
     * copy below — using the cJSON pointer past that point is a use-after-free. */
    char path[LUA_JOB_PATH_MAX];
    strncpy(path, jpath->valuestring, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    int timeout_ms = LUA_EXEC_TIMEOUT_MS;
    cJSON *jto = cJSON_GetObjectItem(root, "timeout_ms");
    if (jto && cJSON_IsNumber(jto) && jto->valueint > 0) timeout_ms = jto->valueint;

    char name[LUA_JOB_NAME_MAX], excl[LUA_JOB_NAME_MAX];
    copy_field(name, sizeof(name), root, "name");
    copy_field(excl, sizeof(excl), root, "exclusive");
    bool replace = false;
    cJSON *jrep = cJSON_GetObjectItem(root, "replace");
    if (jrep && cJSON_IsBool(jrep)) replace = cJSON_IsTrue(jrep);

    /* Detach args so it survives root deletion; owned by the task. */
    cJSON *args_obj = cJSON_DetachItemFromObject(root, "args");
    cJSON_Delete(root);

    if (rtos_mutex_take(s_jobs_lock, RTOS_MAX_DELAY) != RTK_SUCCESS) {
        claw_cap_set_output(output, "{\"error\":\"job lock unavailable\"}");
        cJSON_Delete(args_obj);
        return RTK_FAIL;
    }

    /* --- name / exclusive arbitration ----------------------------------- */
    lua_job_t *conflict = job_find_active_by_name(name);
    if (!conflict) conflict = job_find_active_by_exclusive(excl);
    if (conflict) {
        if (!replace) {
            uint32_t cid = conflict->job_id;
            const char *cname = conflict->name[0] ? conflict->name : conflict->exclusive;
            rtos_mutex_give(s_jobs_lock);
            claw_cap_set_output(output,
                "{\"error\":\"conflict: job %u ('%s') already active; pass replace=true to take over\"}",
                (unsigned)cid, cname);
            cJSON_Delete(args_obj);
            return RTK_FAIL;
        }
        /* replace=true: request stop, release lock, wait for it to finish. */
        uint32_t cid = conflict->job_id;
        conflict->stop_requested = 1;
        rtos_mutex_give(s_jobs_lock);

        uint32_t waited = 0;
        bool gone = false;
        while (waited <= LUA_JOB_STOP_WAIT_MS) {
            rtos_task_yield();
            rtos_time_delay_ms(50);
            waited += 50;
            if (rtos_mutex_take(s_jobs_lock, RTOS_MAX_DELAY) == RTK_SUCCESS) {
                lua_job_t *c = job_find_by_id(cid);
                gone = (!c || job_status_terminal(c->status));
                rtos_mutex_give(s_jobs_lock);
            }
            if (gone) break;
        }
        if (!gone) {
            claw_cap_set_output(output,
                "{\"error\":\"replace: conflicting job %u did not stop in time\"}",
                (unsigned)cid);
            cJSON_Delete(args_obj);
            return RTK_FAIL;
        }
        if (rtos_mutex_take(s_jobs_lock, RTOS_MAX_DELAY) != RTK_SUCCESS) {
            claw_cap_set_output(output, "{\"error\":\"job lock unavailable\"}");
            cJSON_Delete(args_obj);
            return RTK_FAIL;
        }
    }

    /* --- concurrency cap ------------------------------------------------- */
    if (job_count_active() >= LUA_JOB_MAX_RUNNING) {
        rtos_mutex_give(s_jobs_lock);
        claw_cap_set_output(output,
            "{\"error\":\"too many active jobs (max %d); stop one first\"}",
            LUA_JOB_MAX_RUNNING);
        cJSON_Delete(args_obj);
        return RTK_FAIL;
    }

    lua_job_t *slot = job_find_free_slot();
    if (!slot) {
        rtos_mutex_give(s_jobs_lock);
        claw_cap_set_output(output, "{\"error\":\"no free job slot\"}");
        cJSON_Delete(args_obj);
        return RTK_FAIL;
    }

    /* --- Build the async context. Lua VM creation (newstate, sandbox, load,
     *     pcall) is deferred to lua_async_task so it runs on the job task's
     *     own stack, not on the caller's (scheduler 2KB, claw_agent, etc.). --- */
    lua_async_ctx_t *ac = calloc(1, sizeof(*ac));
    if (!ac) {
        rtos_mutex_give(s_jobs_lock);
        claw_cap_set_output(output, "{\"error\":\"out of memory\"}");
        cJSON_Delete(args_obj);
        return RTK_FAIL;
    }
    ac->path = strdup(path);
    /* Serialise args_obj to a JSON string so the task can parse it independently. */
    ac->args_json = args_obj ? cJSON_PrintUnformatted(args_obj) : NULL;
    cJSON_Delete(args_obj);
    if (!ac->path) {
        rtos_mutex_give(s_jobs_lock);
        claw_cap_set_output(output, "{\"error\":\"out of memory\"}");
        free(ac->args_json); free(ac);
        return RTK_FAIL;
    }

    /* claim the slot */
    uint32_t jid = s_next_job_id++;
    if (s_next_job_id == 0) s_next_job_id = 1;
    memset(slot, 0, sizeof(*slot));
    slot->job_id     = jid;
    slot->status     = LUA_JOB_QUEUED;
    slot->timeout_ms = timeout_ms;
    strncpy(slot->name, name, sizeof(slot->name) - 1);
    strncpy(slot->exclusive, excl, sizeof(slot->exclusive) - 1);
    strncpy(slot->path, path, sizeof(slot->path) - 1);

    ac->job_id = jid;
    ac->timeout_ms = timeout_ms;

    if (rtos_task_create(&slot->task, "lua_job", lua_async_task, ac, 8192, 1) != RTK_SUCCESS) {
        slot->job_id = 0;
        slot->status = LUA_JOB_FREE;
        rtos_mutex_give(s_jobs_lock);
        claw_cap_set_output(output, "{\"error\":\"failed to spawn job task\"}");
        free(ac->path); free(ac->args_json); free(ac);
        return RTK_FAIL;
    }

    rtos_mutex_give(s_jobs_lock);

#ifdef CLAW_LUA_TIME_LOG_ENABLE
    RTK_LOGI(TAG, "lua_run_async SPAWN path=%s name=%s job_id=%u t=%u\n",
             path, name, (unsigned)jid,
             (unsigned)rtos_time_get_current_system_time_ms());
#endif

    return claw_cap_set_output(output,
        "{\"job_id\":%u,\"status\":\"QUEUED\",\"name\":\"%s\"}",
        (unsigned)jid, name);
}

/* ---- execute: lua_job_get -------------------------------------------------- */

static int cap_lua_job_get(const char *input_json,
                           const claw_cap_call_context_t *ctx,
                           char **output)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(input_json);
    cJSON *jid  = root ? cJSON_GetObjectItem(root, "job_id") : NULL;
    if (!jid || !cJSON_IsNumber(jid)) {
        claw_cap_set_output(output, "{\"error\":\"missing required field: job_id\"}");
        cJSON_Delete(root);
        return RTK_FAIL;
    }
    uint32_t id = (uint32_t)jid->valuedouble;
    uint32_t since = 0;
    cJSON *jsince = cJSON_GetObjectItem(root, "since_seq");
    if (jsince && cJSON_IsNumber(jsince) && jsince->valuedouble > 0) {
        since = (uint32_t)jsince->valuedouble;
    }
    cJSON_Delete(root);

    char *logbuf = malloc(LUA_JOB_LOG_SIZE + 1);  /* heap-allocated: avoids 2KB stack pressure */
    if (!logbuf) {
        claw_cap_set_output(output, "{\"error\":\"out of memory\"}");
        return RTK_FAIL;
    }
    char    status_s[16], path_s[LUA_JOB_PATH_MAX];
    uint32_t log_seq = 0, started = 0, finished = 0;
    bool   truncated = false, found = false;
    size_t logn = 0;

    if (rtos_mutex_take(s_jobs_lock, RTOS_MAX_DELAY) == RTK_SUCCESS) {
        lua_job_t *j = job_find_by_id(id);
        if (j) {
            found = true;
            strncpy(status_s, job_status_str(j->status), sizeof(status_s) - 1);
            status_s[sizeof(status_s) - 1] = '\0';
            strncpy(path_s, j->path, sizeof(path_s) - 1);
            path_s[sizeof(path_s) - 1] = '\0';
            log_seq   = j->log_seq;
            started   = j->started_ms;
            finished  = j->finished_ms;
            truncated = j->log_truncated;
            /* incremental tail: if caller passed since_seq and it's not behind
             * the ring window, only return the newer bytes. */
            if (since > 0 && since <= j->log_seq) {
                uint32_t new_bytes = j->log_seq - since;
                if (new_bytes > j->log_len) new_bytes = j->log_len;  /* clamped to ring */
                /* read whole ring then keep last new_bytes */
                size_t full = job_log_read(j, logbuf, LUA_JOB_LOG_SIZE + 1);
                if (new_bytes < full) {
                    memmove(logbuf, logbuf + (full - new_bytes), new_bytes);
                    logbuf[new_bytes] = '\0';
                    logn = new_bytes;
                } else {
                    logn = full;
                }
            } else {
                logn = job_log_read(j, logbuf, LUA_JOB_LOG_SIZE + 1);
            }
        }
        rtos_mutex_give(s_jobs_lock);
    }

    if (!found) {
        free(logbuf);
        claw_cap_set_output(output, "{\"error\":\"job %u not found\"}", (unsigned)id);
        return RTK_FAIL;
    }

    /* JSON-escape the log into a small heap buffer (worst case 2x + quotes). */
    char *esc = malloc(logn * 2 + 1);
    if (!esc) {
        free(logbuf);
        claw_cap_set_output(output, "{\"error\":\"out of memory\"}");
        return RTK_ERR_NOMEM;
    }
    size_t eo = 0;
    for (size_t i = 0; i < logn; i++) {
        unsigned char c = (unsigned char)logbuf[i];
        switch (c) {
        case '"':  esc[eo++] = '\\'; esc[eo++] = '"';  break;
        case '\\': esc[eo++] = '\\'; esc[eo++] = '\\'; break;
        case '\n': esc[eo++] = '\\'; esc[eo++] = 'n';  break;
        case '\r': esc[eo++] = '\\'; esc[eo++] = 'r';  break;
        case '\t': esc[eo++] = '\\'; esc[eo++] = 't';  break;
        default:
            if (c < 0x20) { /* drop other control chars */ }
            else esc[eo++] = (char)c;
            break;
        }
    }
    esc[eo] = '\0';

    int set_rc = claw_cap_set_output(output,
        "{\"job_id\":%u,\"status\":\"%s\",\"path\":\"%s\",\"log_seq\":%u,"
        "\"log_truncated\":%s,\"started_ms\":%u,\"finished_ms\":%u,\"log\":\"%s\"}",
        (unsigned)id, status_s, path_s, (unsigned)log_seq,
        truncated ? "true" : "false", (unsigned)started, (unsigned)finished, esc);
    free(logbuf);
    free(esc);
    return set_rc;
}

/* ---- execute: lua_job_list ------------------------------------------------- */

static int cap_lua_job_list(const char *input_json,
                            const claw_cap_call_context_t *ctx,
                            char **output)
{
    (void)input_json; (void)ctx;
    char buf[512];
    size_t off = 0;
    off += DiagSnPrintf(buf + off, sizeof(buf) - off, "{\"jobs\":[");
    bool first = true;

    if (rtos_mutex_take(s_jobs_lock, RTOS_MAX_DELAY) == RTK_SUCCESS) {
        for (int i = 0; i < LUA_JOB_SLOTS; i++) {
            lua_job_t *j = &s_jobs[i];
            if (j->job_id == 0) continue;
            int n = DiagSnPrintf(buf + off, sizeof(buf) - off,
                "%s{\"job_id\":%u,\"status\":\"%s\",\"name\":\"%s\"}",
                first ? "" : ",", (unsigned)j->job_id,
                job_status_str(j->status), j->name);
            if (n > 0 && (size_t)n < sizeof(buf) - off) { off += n; first = false; }
            else break;  /* buffer full; stop listing */
        }
        rtos_mutex_give(s_jobs_lock);
    }
    if (off < sizeof(buf) - 3) {
        DiagSnPrintf(buf + off, sizeof(buf) - off, "]}");
    }
    return claw_cap_set_output(output, "%s", buf);
}

/* ---- execute: lua_job_stop ------------------------------------------------- */

static int cap_lua_job_stop(const char *input_json,
                            const claw_cap_call_context_t *ctx,
                            char **output)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(input_json);
    cJSON *jid  = root ? cJSON_GetObjectItem(root, "job_id") : NULL;
    if (!jid || !cJSON_IsNumber(jid)) {
        claw_cap_set_output(output, "{\"error\":\"missing required field: job_id\"}");
        cJSON_Delete(root);
        return RTK_FAIL;
    }
    uint32_t id = (uint32_t)jid->valuedouble;
    cJSON_Delete(root);

    bool found = false, already_terminal = false;
    if (rtos_mutex_take(s_jobs_lock, RTOS_MAX_DELAY) == RTK_SUCCESS) {
        lua_job_t *j = job_find_by_id(id);
        if (j) {
            found = true;
            if (job_status_terminal(j->status)) {
                already_terminal = true;
            } else {
                j->stop_requested = 1;
            }
        }
        rtos_mutex_give(s_jobs_lock);
    }
    if (!found) {
        claw_cap_set_output(output, "{\"error\":\"job %u not found\"}", (unsigned)id);
        return RTK_FAIL;
    }
    if (already_terminal) {
        claw_cap_set_output(output,
            "{\"job_id\":%u,\"stopped\":false,\"reason\":\"already terminal\"}", (unsigned)id);
        return RTK_SUCCESS;
    }

    /* wait up to the grace period for the job to reach a terminal state */
    uint32_t waited = 0;
    lua_job_status_t final = LUA_JOB_STOPPED;
    while (waited <= LUA_JOB_STOP_WAIT_MS) {
        rtos_time_delay_ms(50);
        waited += 50;
        bool done = false;
        if (rtos_mutex_take(s_jobs_lock, RTOS_MAX_DELAY) == RTK_SUCCESS) {
            lua_job_t *j = job_find_by_id(id);
            if (!j || job_status_terminal(j->status)) {
                done = true;
                if (j) final = j->status;
            }
            rtos_mutex_give(s_jobs_lock);
        }
        if (done) break;
    }

    return claw_cap_set_output(output,
        "{\"job_id\":%u,\"stopped\":true,\"status\":\"%s\"}",
        (unsigned)id, job_status_str(final));
}

/* ---- Cap descriptor & group ------------------------------------------------ */

static const claw_cap_descriptor_t s_desc[] = {
    {
        .id          = "lua_run",
        .name        = "lua_run",
        .family      = "lua",
        .description = "Execute a Lua script by path. The script must define a global "
                       "run(args) function; args is the provided object as a Lua table. "
                       "Allowed paths: rolfs:/skills/**, rolfs:/lua/**, vfs:/skills/**, vfs:/scripts/**, vfs:/tmp/** (.lua only). "
                       "ZERO-STATE SANDBOX: each call creates a fresh lua_State destroyed on return — "
                       "no globals, objects, handles, or buffers survive between calls. "
                       "Persist state via file.write/file.read. "
                       "For multi-script apps or timer-driven display, activate skill_authoring first.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"Absolute .lua path under an allowed root\"},"
            "\"args\":{\"type\":\"object\",\"description\":\"Arguments passed to run(args) as a Lua table\"},"
            "\"timeout_ms\":{\"type\":\"integer\",\"description\":\"Optional per-call timeout in ms (default 30000)\"}"
            "},"
            "\"required\":[\"path\"]}",
        .execute     = cap_lua_run,
    },
    {
        .id          = "lua_run_async",
        .name        = "lua_run_async",
        .family      = "lua",
        .description = "Start a Lua script as a BACKGROUND job and return a job_id immediately. "
                       "Same script contract as lua_run (global run(args)). Use lua_job_get to "
                       "watch progress (captured print output) and lua_job_stop to stop it. "
                       "Optional name/exclusive give at most one active job per name or group; "
                       "replace=true takes over a conflicting job, replace=false (default) is rejected. "
                       "Bounded: max 2 concurrent jobs.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"Absolute .lua path under an allowed root\"},"
            "\"args\":{\"type\":\"object\",\"description\":\"Arguments passed to run(args) as a Lua table\"},"
            "\"timeout_ms\":{\"type\":\"integer\",\"description\":\"Optional wall-clock timeout in ms (default 30000)\"},"
            "\"name\":{\"type\":\"string\",\"description\":\"Optional job name; at most one active job per name\"},"
            "\"exclusive\":{\"type\":\"string\",\"description\":\"Optional exclusive group; at most one active job per group\"},"
            "\"replace\":{\"type\":\"boolean\",\"description\":\"If a name/exclusive conflict exists, true stops it first, false (default) rejects\"}"
            "},"
            "\"required\":[\"path\"]}",
        .execute     = cap_lua_run_async,
    },
    {
        .id          = "lua_job_get",
        .name        = "lua_job_get",
        .family      = "lua",
        .description = "Get a background Lua job's status and captured log (print output). "
                       "Pass since_seq (the previous log_seq) to fetch only newer log bytes incrementally.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"job_id\":{\"type\":\"integer\",\"description\":\"Job id returned by lua_run_async\"},"
            "\"since_seq\":{\"type\":\"integer\",\"description\":\"Optional: only return log bytes after this log_seq\"}"
            "},"
            "\"required\":[\"job_id\"]}",
        .execute     = cap_lua_job_get,
    },
    {
        .id          = "lua_job_list",
        .name        = "lua_job_list",
        .family      = "lua",
        .description = "List background Lua jobs (job_id, status, name).",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute     = cap_lua_job_list,
    },
    {
        .id          = "lua_job_stop",
        .name        = "lua_job_stop",
        .family      = "lua",
        .description = "Request a background Lua job to stop (cooperative cancel) and wait up to 2s "
                       "for it to finish. Returns the final status.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"job_id\":{\"type\":\"integer\",\"description\":\"Job id returned by lua_run_async\"}"
            "},"
            "\"required\":[\"job_id\"]}",
        .execute     = cap_lua_job_stop,
    },
};

static const claw_cap_group_t s_group = {
    .group_id         = "lua",
    .plugin_name      = "cap_lua",
    .version          = "1",
    .descriptors      = s_desc,
    .descriptor_count = sizeof(s_desc) / sizeof(s_desc[0]),
    .group_start      = NULL,
};

/* ---- Throwaway scratch (vfs:/tmp) -----------------------------------------
 *
 * 12_skill_lua_separation.md §C.1/§E lists vfs:/tmp/ as a "throwaway scratch"
 * area where the LLM may write transient scripts (via file_write) and run them
 * (via lua_run). The intended semantic is "cleared on restart".
 *
 * The VFS layer on this platform only supports flash-backed FATFS/LITTLEFS — it
 * has no RAM-disk / ramfs interface (vfs.h: VFS_FATFS, VFS_LITTLEFS only). Adding
 * a RAM block device + a new VFS region would be a large new surface and would
 * not reduce the rolfs blob, against the size constraint. So scratch is backed by
 * the writable VFS1 littlefs (vfs:/tmp), and the throwaway semantic is
 * approximated by WIPING vfs:/tmp at every boot: restart == clean scratch.
 *
 * littlefs does not auto-create parent directories, so we also (re)create the
 * directory here; without it fopen("vfs:/tmp/x.lua","w") would fail with NOENT.
 */
#define SCRATCH_DIR "vfs:/tmp"

static void scratch_clear_and_init(void)
{
    /* Remove any leftover entries from the previous boot, then ensure the
     * directory exists. opendir/readdir/remove/mkdir are the VFS POSIX-ish
     * wrappers (vfs.h). Best-effort: failures are logged but non-fatal. */
    void *dir = opendir(SCRATCH_DIR);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '\0' ||
                strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            char full[VFS_PATH_MAX + 1];
            int n = DiagSnPrintf(full, sizeof(full), "%s/%s", SCRATCH_DIR, ent->d_name);
            if (n > 0 && n < (int)sizeof(full)) {
                /* scratch is flat (LLM writes plain vfs:/tmp/x.lua); remove() of
                 * a regular file is all that is needed. */
                remove(full);
            }
        }
        closedir(dir);
    }
    /* mkdir returns LFS error codes directly; EEXIST after a successful wipe is
     * still fine (we only need the dir to exist afterwards). */
    mkdir(SCRATCH_DIR, 0755);
    RTK_LOGI(TAG, "scratch %s ready (throwaway: cleared on boot)\n", SCRATCH_DIR);
}

/* ---- Public helpers: safe .lua file lifecycle management -------------------
 *
 * Any code that removes a .lua file MUST use cap_lua_file_remove() instead of
 * remove() directly.  This ensures any background lua_async_task running that
 * script (and potentially holding WiFi sockets) is stopped first, preventing
 * WiFi IPC crashes when the next HTTP request is made.
 *
 * cap_lua_stop_jobs_for_path() is the internal primitive; callers that only
 * need to stop jobs without deleting the file may use it directly. */

int cap_lua_stop_jobs_for_path(const char *path)
{
    int count = 0;
    int i;

    if (!path || !path[0] || !s_jobs_lock) {
        return 0;
    }

    if (rtos_mutex_take(s_jobs_lock, 500) != RTK_SUCCESS) {
        return 0;
    }
    for (i = 0; i < LUA_JOB_SLOTS; i++) {
        lua_job_t *j = &s_jobs[i];
        if (j->job_id != 0 && !job_status_terminal(j->status) &&
                strcmp(j->path, path) == 0) {
            j->stop_requested = 1;
            count++;
            RTK_LOGI(TAG, "stop_jobs_for_path: signalled job %u (%s)\n",
                     (unsigned)j->job_id, path);
        }
    }
    rtos_mutex_give(s_jobs_lock);

    /* Brief wait — gives the Lua cancel hook a chance to fire so the WiFi
     * socket is closed before the caller proceeds (e.g. makes a new HTTP req). */
    if (count > 0) {
        rtos_time_delay_ms(400);
    }
    return count;
}

int cap_lua_file_remove(const char *path)
{
    cap_lua_stop_jobs_for_path(path);
    return remove(path);
}

int cap_lua_init(void)
{
    /* One mutex guards the whole async job table (Inc 7). Create it before
     * registering the group so the async tools are safe the moment they appear. */
    if (s_jobs_lock == NULL && rtos_mutex_create(&s_jobs_lock) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "Failed to create job mutex\n");
        return RTK_FAIL;
    }

    int err = claw_cap_register_group(&s_group);
    if (err != RTK_SUCCESS) {
        RTK_LOGE(TAG, "Failed to register group: %d\n", (int)err);
        return err;
    }
    scratch_clear_and_init();
    /* Ensure vfs:/scripts/ exists — persistent user app scripts live here.
     * EEXIST is fine; any other error is non-fatal (scripts dir is optional). */
    mkdir("vfs:/scripts", 0755);
    RTK_LOGI(TAG, "Initialized (lua_run + async jobs: %d slots, max %d running, "
                  "%dB ring log; strict sandbox)\n",
             LUA_JOB_SLOTS, LUA_JOB_MAX_RUNNING, LUA_JOB_LOG_SIZE);
    return RTK_SUCCESS;
}

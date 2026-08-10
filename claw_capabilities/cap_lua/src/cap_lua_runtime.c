/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * cap_lua_runtime — the Lua EXECUTION primitive (M1-4 split from cap_lua.c).
 *
 * Owns everything needed to run one Lua chunk in the strict sandbox: path
 * validation, file reading, the sandbox + restricted require(), JSON->Lua arg
 * decoding, the cancel/deadline hook, print() capture, and the synchronous
 * `lua_run` tool. The asynchronous job table lives in cap_lua_async.c; the cap
 * descriptors + registration live in cap_lua_cmd.c. See cap_lua_internal.h for
 * the shared surface and cap_lua.h for the responsibility split with cap_skill.
 */
#include "cap_lua.h"
#include "cap_lua_internal.h"
#include "claw_cap.h"
#include "ameba_soc.h"
#include "claw_utf8.h"
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
    "vfs:/",       /* allow .lua files directly under vfs: root */
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
const char *cap_lua_validate_path(const char *path)
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
    return "path must be under rolfs:/skills/, rolfs:/lib/, vfs:/skills/, vfs:/scripts/, vfs:/tmp/ or directly under vfs:/";
}

/* ---- Helpers ---------------------------------------------------------------- */

/* Read file into malloc'd buffer (caller frees). Returns NULL on error. */
char *cap_lua_read_file_alloc(const char *path, size_t max_size)
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

    char *code = cap_lua_read_file_alloc(path, LUA_SCRIPT_MAX);
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

    /* 2. blessed library — accepts both "lib/<name>" (legacy, explicit prefix)
     *    and bare "<name>" (auto-resolved via rolfs:/lib/<name>.lua probe).
     *    Both forms cache under the bare name AND "lib/<name>" in _LOADED so
     *    subsequent require() calls in either form hit the cache and skip I/O. */
    {
        bool has_prefix = (strncmp(name, LUA_LIB_PREFIX, LUA_LIB_PREFIX_LEN) == 0);
        const char *lib_name = NULL;   /* bare component to pass to load_blessed_lib */
        if (has_prefix) {
            lib_name = name + LUA_LIB_PREFIX_LEN;
        } else if (strchr(name, '/') == NULL && strchr(name, '\\') == NULL &&
                   strstr(name, "..") == NULL && name[0] != '\0') {
            /* Bare name: probe for the file before claiming it as a blessed lib.
             * fopen is cheap (one flash read) and only fires on the first require
             * of each bare name (subsequent calls hit _LOADED above). */
            char probe[160];
            int pn = snprintf(probe, sizeof(probe), "%s%s.lua", LUA_LIB_ROOT, name);
            if (pn > 0 && (size_t)pn < sizeof(probe)) {
                FILE *pf = fopen(probe, "r");
                if (pf) { fclose(pf); lib_name = name; }
            }
        }
        if (lib_name) {
            load_blessed_lib(L, lib_name);  /* raises on error; result at top */
            /* Cache under bare name (_LOADED[lib_name]) */
            lua_pushvalue(L, -1);
            lua_setfield(L, -3, lib_name);
            /* Cache under "lib/<lib_name>" for symmetry */
            char prefixed[LUA_LIB_PREFIX_LEN + 64];
            int plen = snprintf(prefixed, sizeof(prefixed),
                                "%s%s", LUA_LIB_PREFIX, lib_name);
            if (plen > 0 && (size_t)plen < sizeof(prefixed)) {
                lua_pushvalue(L, -1);
                lua_setfield(L, -3, prefixed);
            }
            lua_remove(L, -2);   /* drop _LOADED, keep result */
            return 1;
        }
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
void cap_lua_push_args_table(lua_State *L, const cJSON *args_obj)
{
    if (args_obj && (cJSON_IsObject(args_obj) || cJSON_IsArray(args_obj))) {
        json_push_value(L, args_obj, 0);
    } else {
        lua_newtable(L);   /* missing / null / scalar → empty table */
    }
}

/* ---- Synchronous execution context ----------------------------------------- */

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
    /* Lua error message captured on failure (load error, runtime error, etc.).
     * Populated by lua_exec_task before lua_close so the error string survives
     * back to cap_lua_run for inclusion in the tool result. */
    char         lua_error[256];
    /* Caller's origin (channel/chat_id), copied from the cap call context so
     * event.notify()/event.origin() can reach the launching user. Fixed-size
     * (no extra alloc/free) — empty string when the call had no channel. */
    char         origin_channel[64];
    char         origin_chat[128];
} lua_exec_ctx_t;

void cap_lua_cancel_hook(lua_State *L, lua_Debug *ar)
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

/* True if a cooperative stop (__cancel_ptr) or the wall-clock deadline
 * (__deadline_ms) is pending RIGHT NOW. Mirrors cap_lua_cancel_hook's two
 * conditions exactly. Used by the guarded pcall/xpcall below so a stop/timeout
 * can never be swallowed by script-level error handling. */
static bool cap_lua_cancel_pending(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__cancel_ptr");
    volatile int *p = (volatile int *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (p && *p) {
        return true;
    }
    lua_getfield(L, LUA_REGISTRYINDEX, "__deadline_ms");
    lua_Integer deadline = lua_tointeger(L, -1);
    lua_pop(L, 1);
    return (deadline != 0 &&
            (lua_Integer)rtos_time_get_current_system_time_ms() >= deadline);
}

/* ── Guarded pcall / xpcall ───────────────────────────────────────────────────
 * Stock pcall/xpcall turn ANY error — including the cooperative-cancel and
 * wall-clock-deadline errors raised by cap_lua_cancel_hook — into a catchable
 * (false, msg) pair. A script whose loop body is wrapped in pcall (the common
 * "robust loop" idiom) therefore SWALLOWS its own stop/timeout and runs forever,
 * making lua_job_stop / replace / the deadline ineffective.
 *
 * These drop-in replacements behave exactly like the originals for ordinary
 * errors, but if a stop/deadline is pending when the protected call fails they
 * re-raise instead of returning — so the cancel propagates up through every
 * (also-guarded) pcall layer to the task's outer lua_pcall and the job actually
 * terminates. Cancellation stays cooperative (no task kill, so locks/sockets are
 * never left dangling) yet is no longer swallowable. */
static int cap_lua_guarded_pcall(lua_State *L)
{
    luaL_checkany(L, 1);
    int nargs  = lua_gettop(L) - 1;                 /* f + arg1..argN          */
    int status = lua_pcall(L, nargs, LUA_MULTRET, 0);
    if (status != LUA_OK) {
        if (cap_lua_cancel_pending(L)) {
            return lua_error(L);                    /* propagate: err on top   */
        }
        lua_pushboolean(L, 0);
        lua_insert(L, -2);                          /* → false, err            */
        return 2;
    }
    int nres = lua_gettop(L);                        /* results (f+args gone)   */
    lua_pushboolean(L, 1);
    lua_insert(L, 1);                               /* → true, results...      */
    return nres + 1;
}

static int cap_lua_guarded_xpcall(lua_State *L)
{
    luaL_checkany(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);            /* msgh at index 2         */
    /* Reorder f(1),msgh(2),args(3..) → msgh(1),f(2),args(3..) so the message
     * handler sits at the fixed errfunc index 1 below the called function. */
    lua_pushvalue(L, 1);                            /* push copy of f          */
    lua_remove(L, 1);                               /* msgh(1),args..,f(top)   */
    lua_insert(L, 2);                               /* msgh(1),f(2),args(3..)  */
    int nargs  = lua_gettop(L) - 2;
    int status = lua_pcall(L, nargs, LUA_MULTRET, 1);   /* errfunc = msgh@1    */
    if (status != LUA_OK) {
        if (cap_lua_cancel_pending(L)) {
            return lua_error(L);                    /* msgh(1), err(top)       */
        }
        lua_pushboolean(L, 0);                      /* msgh, err, false        */
        lua_remove(L, 1);                           /* err, false              */
        lua_insert(L, 1);                           /* → false, err            */
        return 2;
    }
    lua_remove(L, 1);                               /* drop msgh: results...   */
    int nres = lua_gettop(L);
    lua_pushboolean(L, 1);
    lua_insert(L, 1);                               /* → true, results...      */
    return nres + 1;
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

/* Unified print() capture for both sync (lua_run) and async (lua_run_async).
 * Sync:  __print_cap → lua_exec_ctx_t.stdout_buf (flat buffer)
 * Async: delegated to cap_lua_async_capture_print (cap_lua_async.c) which
 *        appends to the running job's ring log via __job_slot. */
int cap_lua_capture_print(lua_State *L)
{
    char   line[256];
    size_t n = lua_format_print_line(L, line, sizeof(line));

#ifdef CONFIG_CLAW_LUA_PRINT_ECHO_SERIAL
    /* Optional live echo to the serial console. This is ON TOP OF the capture
     * below (buffer / job ring log) — capture behaviour is unchanged. `line`
     * already ends with '\n'; copy + NUL-terminate for the %s logger (the
     * content is the data arg, never the format string, so any '%' is safe). */
    {
        char   echo[257];
        size_t m = n < sizeof(echo) - 1 ? n : sizeof(echo) - 1;
        memcpy(echo, line, m);
        echo[m] = '\0';
        RTK_LOGS(NOTAG, RTK_LOG_INFO, "[lua] %s", echo);
    }
#endif

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

    cap_lua_async_capture_print(L, line, n);
    return 0;
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
void cap_lua_install_sandbox(lua_State *L)
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
    /* Replace base-lib pcall/xpcall with cancel-aware versions so a script can
     * never swallow its own lua_job_stop / wall-clock timeout (see the guarded
     * wrappers above). Ordinary errors are still catchable exactly as before. */
    lua_pushcfunction(L, cap_lua_guarded_pcall);  lua_setglobal(L, "pcall");
    lua_pushcfunction(L, cap_lua_guarded_xpcall); lua_setglobal(L, "xpcall");
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

/* ---- sync exec task -------------------------------------------------------- */

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
    cap_lua_install_sandbox(L);

    char *file_code = cap_lua_read_file_alloc(ctx->path, LUA_SCRIPT_MAX);
    if (!file_code) {
        RTK_LOGE(TAG, "lua_exec_task: script not found: %s\n", ctx->path);
        ctx->lua_rc = LUA_ERRFILE;
        lua_close(L);
        goto done;
    }
    {
        char chunkname[176];
        snprintf(chunkname, sizeof(chunkname), "@%s", ctx->path);
        int rc_load = luaL_loadbuffer(L, file_code, strlen(file_code), chunkname);
        free(file_code);
        if (rc_load != LUA_OK) {
            const char *lerr = lua_tostring(L, -1);
            RTK_LOGE(TAG, "lua_exec_task: load failed: %s\n", lerr);
            strncpy(ctx->lua_error, lerr ? lerr : "compile error", sizeof(ctx->lua_error) - 1);
            ctx->lua_rc = rc_load;
            lua_close(L);
            goto done;
        }
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        const char *lerr = lua_tostring(L, -1);
        RTK_LOGE(TAG, "lua_exec_task: top-level error: %s\n", lerr);
        strncpy(ctx->lua_error, lerr ? lerr : "top-level error", sizeof(ctx->lua_error) - 1);
        ctx->lua_rc = LUA_ERRRUN;
        lua_close(L);
        goto done;
    }
    lua_getglobal(L, "run");
    if (!lua_isfunction(L, -1)) {
        RTK_LOGE(TAG, "lua_exec_task: no run() in %s\n", ctx->path);
        strncpy(ctx->lua_error, "script has no global run() function — add: function run(args) ... end (not local, not self-executing)", sizeof(ctx->lua_error) - 1);
        ctx->lua_rc = LUA_ERRRUN;
        lua_close(L);
        goto done;
    }

    /* Register cancel pointer so sleep_ms and the hook can see it. */
    lua_pushlightuserdata(L, (void *)&ctx->cancel);
    lua_setfield(L, LUA_REGISTRYINDEX, "__cancel_ptr");
    lua_sethook(L, cap_lua_cancel_hook, LUA_MASKCOUNT, LUA_CANCEL_INSTR_FREQ);

    /* Stash the caller's origin so event.notify()/event.origin() can reply to
     * the launching user (same registry keys as the async path). */
    lua_pushstring(L, ctx->origin_channel);
    lua_setfield(L, LUA_REGISTRYINDEX, "__origin_channel");
    lua_pushstring(L, ctx->origin_chat);
    lua_setfield(L, LUA_REGISTRYINDEX, "__origin_chat");

    /* Capture print() into ctx->stdout_buf. */
    lua_pushlightuserdata(L, (void *)ctx);
    lua_setfield(L, LUA_REGISTRYINDEX, "__print_cap");
    lua_pushcfunction(L, cap_lua_capture_print);
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
        cap_lua_push_args_table(L, args_obj);
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
        strncpy(ctx->lua_error, err ? err : "run() error", sizeof(ctx->lua_error) - 1);
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

    /* Release the sync-run concurrency slot (shared budget in cap_lua_async.c). */
    cap_lua_sync_slot_release();

    rtos_task_delete(NULL);
}

/* ---- execute: lua_run ------------------------------------------------------ */

/* Shared core (D2, design_spec/lua/lua_module_thread_architecture.md). allow_tmp is
 * true only for the direct-LLM-tool-call JSON path (cap_lua_run with
 * ctx->caller == CLAW_CAP_CALLER_LLM) — preserves the pre-refactor exception
 * exactly. The public plain-C entry point (cap_lua_run_script, used by
 * lua_module_thread's thread.run()) always passes false. */
static int cap_lua_run_script_impl(const char *path, const char *args_json, int timeout_ms,
                                    const char *origin_channel, const char *origin_chat,
                                    bool allow_tmp, char **output)
{
    const char *perr = cap_lua_validate_path(path);
    if (perr) {
        claw_cap_set_output(output, "{\"error\":\"%s\"}", perr);
        return RTK_FAIL;
    }

    /* Invariant: vfs:/tmp/ is throwaway scratch wiped on every reboot.
     * Executing a script from there via a scheduled (non-LLM) call would
     * silently do nothing after a restart. LLM callers (allow_tmp) are the
     * one exception — the LLM writes the script itself and runs it in the
     * same session, so it knows the file is transient. */
    if (!allow_tmp && strncmp(path, "vfs:/tmp/", 9) == 0) {
        claw_cap_set_output(output,
            "{\"error\":\"vfs:/tmp/ scripts are wiped on reboot and cannot be used "
            "in scheduled or internal calls. Use vfs:/scripts/ for persistent scripts "
            "or vfs:/skills/ for skill-managed scripts.\"}");
        return RTK_FAIL;
    }

    if (timeout_ms <= 0) timeout_ms = LUA_EXEC_TIMEOUT_MS;

#ifdef CLAW_LUA_TIME_LOG_ENABLE
    uint32_t run_start_ms = rtos_time_get_current_system_time_ms();
    RTK_LOGI(TAG, "lua_run START path=%s timeout=%dms t=%u\n",
             path, timeout_ms, (unsigned)run_start_ms);
#endif

    /* Concurrency gate: lua_run (sync) shares the LUA_JOB_MAX_RUNNING budget
     * with lua_run_async jobs.  The shared accounting lives in cap_lua_async.c.
     * acquire() returns 1 when at capacity (reject), 0/2 to proceed (0 = slot
     * reserved, 2 = lock unavailable but proceed as the pre-split code did).
     * Timer-driven calls must NOT bypass this limit — excess concurrent
     * lua_States exhaust heap and corrupt adjacent allocations. */
    if (cap_lua_sync_slot_acquire() == 1) {
        claw_cap_set_output(output,
            "{\"error\":\"lua busy (max %d concurrent runs); use lua_job_list to find running jobs, then lua_job_stop to free a slot\"}",
            LUA_JOB_MAX_RUNNING);
        return RTK_FAIL;
    }

    /* Run in a separate task so we don't block the calling (agent/AT/script)
     * task. The ctx is heap-allocated so its pointer stays valid even if this
     * function returns early (hard timeout) before the task exits. exec owns
     * its own copy of args_json (task frees it) — args_json param is the
     * caller's, not ours to free. */
    lua_exec_ctx_t *exec = calloc(1, sizeof(*exec));
    if (!exec) {
        cap_lua_sync_slot_release();
        claw_cap_set_output(output, "{\"error\":\"out of memory\"}");
        return RTK_FAIL;
    }
    exec->path      = strdup(path);
    exec->args_json = args_json ? strdup(args_json) : NULL;
    exec->lua_rc    = LUA_OK;
    if (origin_channel && origin_channel[0]) {
        strncpy(exec->origin_channel, origin_channel, sizeof(exec->origin_channel) - 1);
    }
    if (origin_chat && origin_chat[0]) {
        strncpy(exec->origin_chat, origin_chat, sizeof(exec->origin_chat) - 1);
    }
    if (!exec->path) {
        cap_lua_sync_slot_release();
        claw_cap_set_output(output, "{\"error\":\"out of memory\"}");
        free(exec->args_json); free(exec);
        return RTK_FAIL;
    }

    if (rtos_sema_create_binary(&exec->done) != RTK_SUCCESS) {
        cap_lua_sync_slot_release();
        claw_cap_set_output(output, "{\"error\":\"failed to create semaphore\"}");
        free(exec->path); free(exec->args_json); free(exec);
        return RTK_FAIL;
    }

    rtos_task_t lua_run_task = NULL;
    if (rtos_task_create(&lua_run_task, "lua_run", lua_exec_task,
                         exec, 8192, 1) != RTK_SUCCESS) {
        cap_lua_sync_slot_release();
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

    /* Save lua_error before freeing exec. */
    char lua_error_buf[256] = "";
    if (exec->lua_error[0]) {
        strncpy(lua_error_buf, exec->lua_error, sizeof(lua_error_buf) - 1);
    }

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
        if (cancelled) {
            free(stdout_str);
            claw_cap_set_output(output,
                "{\"error\":\"lua_run '%s' timed out after %dms\"}", path, timeout_ms);
        } else {
            /* Enrich the error with lua_error (stack trace / message) and any
             * stdout captured before the failure, so the LLM can diagnose without
             * needing a pcall wrapper around the script. */
            char *esc_err = NULL;
            char *esc_out = NULL;
            if (lua_error_buf[0]) {
                size_t elen = strlen(lua_error_buf);
                esc_err = malloc(elen * 2 + 1);
                if (esc_err) {
                    size_t eo = 0;
                    for (size_t i = 0; i < elen; i++) {
                        unsigned char c = (unsigned char)lua_error_buf[i];
                        switch (c) {
                        case '"':  esc_err[eo++] = '\\'; esc_err[eo++] = '"';  break;
                        case '\\': esc_err[eo++] = '\\'; esc_err[eo++] = '\\'; break;
                        case '\n': esc_err[eo++] = '\\'; esc_err[eo++] = 'n';  break;
                        case '\r': esc_err[eo++] = '\\'; esc_err[eo++] = 'r';  break;
                        case '\t': esc_err[eo++] = '\\'; esc_err[eo++] = 't';  break;
                        default: if (c >= 0x20) esc_err[eo++] = (char)c; break;
                        }
                    }
                    esc_err[eo] = '\0';
                }
            }
            if (stdout_str) {
                size_t slen = strlen(stdout_str);
                esc_out = malloc(slen * 2 + 1);
                if (esc_out) {
                    size_t eo = 0;
                    for (size_t i = 0; i < slen; i++) {
                        unsigned char c = (unsigned char)stdout_str[i];
                        switch (c) {
                        case '"':  esc_out[eo++] = '\\'; esc_out[eo++] = '"';  break;
                        case '\\': esc_out[eo++] = '\\'; esc_out[eo++] = '\\'; break;
                        case '\n': esc_out[eo++] = '\\'; esc_out[eo++] = 'n';  break;
                        case '\r': esc_out[eo++] = '\\'; esc_out[eo++] = 'r';  break;
                        case '\t': esc_out[eo++] = '\\'; esc_out[eo++] = 't';  break;
                        default: if (c >= 0x20) esc_out[eo++] = (char)c; break;
                        }
                    }
                    esc_out[eo] = '\0';
                }
                free(stdout_str);
                stdout_str = NULL;
            }
            if (esc_err && esc_out) {
                claw_cap_set_output(output,
                    "{\"error\":\"execution failed\",\"lua_error\":\"%s\",\"stdout\":\"%s\"}",
                    esc_err, esc_out);
            } else if (esc_err) {
                claw_cap_set_output(output,
                    "{\"error\":\"execution failed\",\"lua_error\":\"%s\"}", esc_err);
            } else if (esc_out) {
                claw_cap_set_output(output,
                    "{\"error\":\"execution failed\",\"stdout\":\"%s\"}", esc_out);
            } else {
                claw_cap_set_output(output,
                    "{\"error\":\"lua_run '%s' execution failed\"}", path);
            }
            free(esc_err);
            free(esc_out);
        }
        /* lua_close is called by lua_exec_task. */
        return RTK_FAIL;
    }

    /* Guarantee the return value is valid JSON inside the envelope. Scripts
     * using resp.ok()/resp.err() already return JSON (kept as-is); a bare
     * string/other return would splice in unquoted and break the JSON, so
     * encode those as a JSON string. */
    char *result_json = result;
    int   result_json_owned = 0;
    {
        cJSON *probe = cJSON_Parse(result);
        if (probe) {
            cJSON_Delete(probe);
        } else {
            cJSON *sv  = cJSON_CreateString(result);
            char  *enc = sv ? cJSON_PrintUnformatted(sv) : NULL;
            cJSON_Delete(sv);
            if (enc) { result_json = enc; result_json_owned = 1; }
        }
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
            /* stdout_len reaching the buffer limit means output was truncated */
            int truncated = (slen + 1 >= sizeof(((lua_exec_ctx_t *)0)->stdout_buf));
            set_rc = claw_cap_set_output(output,
                "{\"result\":%s,\"stdout\":\"%s\",\"stdout_truncated\":%s}",
                result_json, esc, truncated ? "true" : "false");
            free(esc);
        } else {
            set_rc = claw_cap_set_output(output, "%s", result_json);
        }
        free(stdout_str);
    } else {
        set_rc = claw_cap_set_output(output, "%s", result_json);
    }
    if (result_json_owned) free(result_json);
    free(result);
    /* lua_close is called by lua_exec_task. */
    return set_rc;
}

int cap_lua_run_script(const char *path, const char *args_json, int timeout_ms,
                       const char *origin_channel, const char *origin_chat,
                       char **output)
{
    return cap_lua_run_script_impl(path, args_json, timeout_ms,
                                    origin_channel, origin_chat, false, output);
}

int cap_lua_run(const char *input_json,
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

    /* Optional per-call timeout. 0 / absent → default (impl applies it). */
    int timeout_ms = 0;
    cJSON *jto = cJSON_GetObjectItem(root, "timeout_ms");
    if (jto && cJSON_IsNumber(jto) && jto->valueint > 0) {
        timeout_ms = jto->valueint;
    }

    /* Detach the args object; serialise to JSON string so the exec task can
     * parse it independently on its own stack. */
    cJSON *args_obj = cJSON_DetachItemFromObject(root, "args");
    char  *args_json = args_obj ? cJSON_PrintUnformatted(args_obj) : NULL;
    cJSON_Delete(args_obj);
    cJSON_Delete(root);

    /* This JSON tool allows vfs:/tmp/ paths for direct LLM tool-calls only —
     * preserved exactly as before the D2 refactor. */
    bool allow_tmp = ctx && ctx->caller == CLAW_CAP_CALLER_LLM;
    int rc = cap_lua_run_script_impl(path, args_json, timeout_ms,
                                      ctx ? ctx->channel : NULL, ctx ? ctx->chat_id : NULL,
                                      allow_tmp, output);
    free(args_json);
    return rc;
}

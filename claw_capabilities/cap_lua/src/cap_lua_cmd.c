/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * cap_lua_cmd — cap_lua tool surface + lifecycle (M1-4 split from cap_lua.c).
 *
 * Owns the cap descriptor table (lua_run + the async job tools), group
 * registration (cap_lua_init), and the vfs:/tmp scratch lifecycle. The execute
 * functions themselves live in cap_lua_runtime.c (lua_run) and cap_lua_async.c
 * (lua_run_async / lua_job_*); this file only wires them into the registry. See
 * cap_lua_internal.h for the shared surface and cap_lua.h for the public API.
 */
#include "cap_lua.h"
#include "cap_lua_internal.h"
#include "claw_cap.h"
#include "claw_cap_registry.h"
#include "claw_config.h"
#include "lua_module_registry_mgmt.h"
#include "os_wrapper.h"
#include "platform_stdlib.h"
#include "vfs.h"
#include <stdio.h>
#include <string.h>
#include "ameba_claw_defs.h"

extern void lua_task(void *param);

#define TAG "cap_lua"

/* ---- Cap descriptor & group ------------------------------------------------ */

static const claw_cap_descriptor_t s_desc[] = {
    {
        .id          = "lua_run",
        .name        = "lua_run",
        .family      = "lua",
        .description = "Execute a Lua script by path. "
                       "REQUIRED: script MUST define a global function named exactly 'run': "
                       "  function run(args) ... end  "
                       "(NOT local, NOT self-executing, NOT return run()). "
                       "args is the provided object as a Lua table. "
                       "Allowed paths: vfs:/**, rolfs:/skills/**, rolfs:/lib/** (.lua only). For one-off task scripts use vfs:/tmp/ (auto-cleared on session,clear and reboot); for permanent skills use vfs:/skills/. "
                       "ZERO-STATE SANDBOX: each call creates a fresh lua_State destroyed on return - "
                       "no globals, objects, handles, or buffers survive between calls. "
                       "Persist state via file.write/file.read. "
                       "SANDBOX LIBS: io, os, coroutine, debug are NOT available (all nil) - do NOT use io.open or os.*; for file I/O use require('file') (write/read/exists/remove/list) or require('storage'). "
                       "Return: rc=OK means run() completed successfully regardless of stdout length. "
                       "stdout_truncated=true means output was cut off but execution still succeeded - do NOT re-run. "
                       "For multi-script apps or timer-driven display, activate skill_authoring first. "
                       "BUILT-IN LUA MODULES (require directly, no install needed): "
                       "storage: auto-selects SD or vfs: root; read rolfs:/docs/storage.md for full API before use. "
                       "display, led_strip, imu, pwm, i2c: hardware modules; docs under rolfs:/docs/. "
                       "NOTE: print() in Lua sends output directly to serial UART -- do NOT use im_send_media for serial output.",
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
                       "Bounded: max 2 concurrent jobs. "
                       "By DEFAULT a job runs UNBOUNDED (until lua_job_stop) - this is the right "
                       "choice for continuous monitors, animations and pollers whose run() never "
                       "returns. Pass timeout_ms ONLY to impose a wall-clock limit; hitting it is a "
                       "configured limit, not a script error.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"Absolute .lua path under an allowed root\"},"
            "\"args\":{\"type\":\"object\",\"description\":\"Arguments passed to run(args) as a Lua table\"},"
            "\"timeout_ms\":{\"type\":\"integer\",\"description\":\"Optional wall-clock limit in ms. OMIT for an unbounded job that runs until lua_job_stop (the default, correct for continuous monitors/animations). Set a positive value only to force-stop after a deadline.\"},"
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

void cap_lua_scratch_reset(void)
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

/* lua_module_thread's one-time init: creates the global thread.sync registry
 * mutex and registers the quiescence-reclaim callback. Declared extern (same
 * cross-module style as atcmd_lua.c's lua_run_repl_once) so we can invoke it
 * here at boot. thread_sync_init() is also called per-lua_State from
 * luaopen_thread, but doing it once here — in the single-threaded boot phase,
 * before any job task exists — pre-creates the mutex so the per-lua_State
 * lazy path can never race two concurrent job inits into a double-create
 * (leaked mutex + an unguarded global object list). It is idempotent. */
extern int thread_sync_init(void);

int cap_lua_init(void)
{
    /* One mutex guards the whole async job table (Inc 7) + the shared sync/async
     * concurrency budget. Create it before registering the group so the async
     * tools (and the sync gate) are safe the moment they appear. */
    if (cap_lua_async_init_lock() != RTK_SUCCESS) {
        return RTK_FAIL;
    }

    /* Pre-create the thread.sync registry mutex single-threaded (see note on
     * the extern above) — removes the lazy-init TOCTOU in thread_sync_ensure_lock. */
    if (thread_sync_init() != RTK_SUCCESS) {
        return RTK_FAIL;
    }

    int err = claw_cap_register_group(&s_group);
    if (err != RTK_SUCCESS) {
        RTK_LOGE(TAG, "Failed to register group: %d\n", (int)err);
        return err;
    }
    cap_lua_scratch_reset();
    /* Ensure vfs:/scripts/ exists — persistent user app scripts live here.
     * EEXIST is fine; any other error is non-fatal (scripts dir is optional). */
    mkdir("vfs:/scripts", 0755);
    RTK_LOGI(TAG, "Initialized (lua_run + async jobs: %d slots, max %d running, "
                  "%dB ring log; strict sandbox)\n",
             LUA_JOB_SLOTS, LUA_JOB_MAX_RUNNING, LUA_JOB_LOG_SIZE);
    return RTK_SUCCESS;
}

/* ---- Lifecycle registration (claw_cap_registry): CORE, INIT phase ----
 * cap_lua is a core capability (CLAW_CAP_FLAG_CORE): always active, exempt from
 * the runtime enable-list — cap_files / cap_skill_mgr / the serial scratch REPL
 * all depend on it. */
static void lua_on_init(const claw_config_t *cfg)
{
    lua_module_registry_init();
    lua_module_registry_set_disabled(cfg->lua.disabled_modules);
    cap_lua_init();
    if (rtos_task_create(NULL, "lua_task", lua_task, NULL, 8192, 1) != RTK_SUCCESS)
        RTK_LOGE("cap_lua", "lua_task create failed\n");
}
CLAW_CAP_REGISTER(lua, {
    .group   = "lua",
    .flags   = CLAW_CAP_FLAG_CORE,
    .order   = 80,
    .on_init = lua_on_init,
});

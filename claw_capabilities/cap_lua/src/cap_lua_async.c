/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * cap_lua_async — asynchronous Lua jobs (M1-4 split from cap_lua.c, Inc 7 full
 * tier). Owns the bounded background job table and the lua_run_async /
 * lua_job_{get,list,stop} tools, plus the single mutex that also guards the
 * synchronous lua_run concurrency budget (cap_lua_sync_slot_*). The Lua VM,
 * sandbox, cancel hook and print formatting are reused from cap_lua_runtime.c
 * via cap_lua_internal.h. The cap descriptors + registration live in
 * cap_lua_cmd.c.
 */
#include "cap_lua.h"
#include "cap_lua_internal.h"
#include "claw_cap.h"
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

#define TAG "cap_lua"

/* Bounded resources sized for the device memory budget.
 * (LUA_JOB_SLOTS / LUA_JOB_LOG_SIZE / LUA_JOB_MAX_RUNNING live in
 * cap_lua_internal.h — shared with the sync path and the startup log.) */
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

static lua_job_t    s_jobs[LUA_JOB_SLOTS];
/* One mutex guards the whole async job table AND the sync lua_run counter so
 * the "sync + active jobs" total used by the shared concurrency gate is read
 * atomically. */
static rtos_mutex_t s_jobs_lock = NULL;
static int          s_sync_lua_run_count = 0; /* sync lua_run calls in flight */
static uint32_t     s_next_job_id = 1;

/* Fired when the job world goes quiescent (see cap_lua_set_quiescence_cb).
 * Plain pointer store; the write is a single word and only ever set once from
 * luaopen_thread, so no lock is needed to read it. */
static void (*s_quiescence_cb)(void) = NULL;

/* Per-task parameter handed to the async task.
 * The task owns all Lua VM creation (luaL_newstate + install_sandbox +
 * luaL_loadstring) so that the parse/compile step runs on the lua_job
 * task's stack rather than the caller's (scheduler, claw_agent, etc.). */
typedef struct {
    char      *path;        /* script path; heap-allocated, freed by task    */
    char      *args_json;   /* JSON args string; heap-allocated, freed by task */
    char      *channel;     /* caller's IM/serial channel (origin); may be NULL */
    char      *chat_id;     /* caller's chat_id (origin); may be NULL        */
    uint32_t   job_id;      /* identifies the slot                           */
    int        timeout_ms;
} lua_async_ctx_t;

/* Free an async job context and all its heap-owned strings. NULL-safe on both
 * the struct and each field, so it is correct on every partial-init error path. */
static void async_ctx_free(lua_async_ctx_t *ac)
{
    if (!ac) return;
    free(ac->path);
    free(ac->args_json);
    free(ac->channel);
    free(ac->chat_id);
    free(ac);
}

/* ---- shared concurrency budget (sync lua_run + async jobs) ----------------- */

int cap_lua_async_init_lock(void)
{
    if (s_jobs_lock == NULL && rtos_mutex_create(&s_jobs_lock) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "Failed to create job mutex\n");
        return RTK_FAIL;
    }
    return RTK_SUCCESS;
}

/* forward decl (defined below; counts non-terminal jobs, caller holds lock) */
static int job_count_active(void);

void cap_lua_set_quiescence_cb(void (*cb)(void))
{
    s_quiescence_cb = cb;
}

/* Fire the quiescence callback iff no async job is active AND no synchronous
 * lua_run is in flight. Called from every job/sync-run termination path. Takes
 * s_jobs_lock only to sample the two counters, then RELEASES it before invoking
 * the callback — the callback reclaims thread.sync objects under its OWN
 * (separate) registry lock, so it must never run nested inside s_jobs_lock. */
static void cap_lua_notify_if_quiescent(void)
{
    bool idle = false;
    if (rtos_mutex_take(s_jobs_lock, RTOS_MAX_DELAY) == RTK_SUCCESS) {
        idle = (s_sync_lua_run_count == 0 && job_count_active() == 0);
        rtos_mutex_give(s_jobs_lock);
    }
    if (idle && s_quiescence_cb) {
        s_quiescence_cb();
    }
}

int cap_lua_sync_slot_acquire(void)
{
    /* Short timeout matches the pre-split gate: if the lock is momentarily
     * unavailable we proceed without reserving a slot (return 2) rather than
     * blocking the caller, exactly as before. */
    if (rtos_mutex_take(s_jobs_lock, 100) == RTK_SUCCESS) {
        int total = s_sync_lua_run_count + job_count_active();
        if (total >= LUA_JOB_MAX_RUNNING) {
            rtos_mutex_give(s_jobs_lock);
            return 1;   /* busy */
        }
        s_sync_lua_run_count++;
        rtos_mutex_give(s_jobs_lock);
        return 0;       /* acquired */
    }
    return 2;           /* lock unavailable — proceed anyway */
}

void cap_lua_sync_slot_release(void)
{
    /* Use RTOS_MAX_DELAY: skipping this decrement would permanently leak a slot
     * and cause spurious "lua busy" errors; a timeout here must never happen. */
    if (rtos_mutex_take(s_jobs_lock, RTOS_MAX_DELAY) == RTK_SUCCESS) {
        if (s_sync_lua_run_count > 0) s_sync_lua_run_count--;
        rtos_mutex_give(s_jobs_lock);
    }
    /* A finishing sync lua_run may be the last thing running — sweep if idle. */
    cap_lua_notify_if_quiescent();
}

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

/* Async branch of the unified print() capture (cap_lua_capture_print in
 * cap_lua_runtime.c). Looks up the running job via __job_slot and appends the
 * formatted line to its ring log. No-op outside an async context. */
void cap_lua_async_capture_print(lua_State *L, const char *line, size_t n)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__job_slot");
    lua_job_t *slot = (lua_job_t *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (slot && rtos_mutex_take(s_jobs_lock, 1000) == RTK_SUCCESS) {
        if (slot->job_id != 0) job_log_append(slot, line, n);
        rtos_mutex_give(s_jobs_lock);
    }
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

    /* Heap-allocated error buffer for early-init failures (script not found,
     * compile error, etc.) that occur before the job log is writable.
     * Must be freed on EVERY exit path (normal bottom + mark_failed). */
    char *init_err = calloc(1, CLAW_LUA_INIT_ERR_MAX);

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
            job_log_append(j, "[init]\n", 7); /* sentinel: log_seq > 0 immediately so
                                                * lua_job_get callers know the task is
                                                * alive before print() is hooked */
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
        if (init_err) strncpy(init_err, "failed to create Lua state", CLAW_LUA_INIT_ERR_MAX - 1);
        async_ctx_free(ac);
        goto mark_failed;
    }
    cap_lua_install_sandbox(L);

    char *file_code = cap_lua_read_file_alloc(ac->path, LUA_SCRIPT_MAX);
    if (!file_code) {
        RTK_LOGE(TAG, "lua_async_task: script not found: %s\n", ac->path);
        if (init_err) DiagSnPrintf(init_err, CLAW_LUA_INIT_ERR_MAX,
                                   "script not found: %s", ac->path);
        lua_close(L);
        async_ctx_free(ac);
        goto mark_failed;
    }
    {
        char chunkname[176];
        snprintf(chunkname, sizeof(chunkname), "@%s", ac->path);
        int rc_load = luaL_loadbuffer(L, file_code, strlen(file_code), chunkname);
        free(file_code);
        if (rc_load != LUA_OK) {
            const char *lerr = lua_tostring(L, -1);
            RTK_LOGE(TAG, "lua_async_task: load failed: %s\n", lerr);
            if (init_err) DiagSnPrintf(init_err, CLAW_LUA_INIT_ERR_MAX,
                                       "compile error: %s", lerr ? lerr : "unknown");
            lua_close(L);
            async_ctx_free(ac);
            goto mark_failed;
        }
    }
    /* Stash slot pointer (for print capture) + cancel pointer in the registry.
     * Done BEFORE the top-level pcall so that print() calls in top-level code
     * (e.g. initialisation prints) are captured in the job log just like prints
     * inside run(). The cancel hook also applies to top-level code — correct,
     * since lua_job_stop should abort the job at any point. */
    lua_pushlightuserdata(L, (void *)slot);
    lua_setfield(L, LUA_REGISTRYINDEX, "__job_slot");
    lua_pushlightuserdata(L, (void *)stop_ptr);
    lua_setfield(L, LUA_REGISTRYINDEX, "__cancel_ptr");
    /* Stash the caller's origin (channel/chat_id) so event.notify()/event.origin()
     * can push messages back to the user that launched this job, without the
     * script (or the LLM) having to know or pass the ids. Registry copies the
     * strings, so `ac`'s copies can be freed after run() starts. Empty string
     * when the launch had no channel (e.g. a trigger/scheduler job). */
    lua_pushstring(L, ac->channel ? ac->channel : "");
    lua_setfield(L, LUA_REGISTRYINDEX, "__origin_channel");
    lua_pushstring(L, ac->chat_id ? ac->chat_id : "");
    lua_setfield(L, LUA_REGISTRYINDEX, "__origin_chat");
    /* timeout_ms <= 0 ⇒ no deadline: leave __deadline_ms == 0 so the cancel
     * hook treats it as disabled and the job runs until lua_job_stop. Only a
     * positive limit arms a wall-clock deadline. (Computing now + 0 here would
     * have armed a deadline equal to "now" and tripped on the first hook fire.) */
    lua_Integer deadline_ms = 0;
    if (ac->timeout_ms > 0) {
        deadline_ms = (lua_Integer)(rtos_time_get_current_system_time_ms()
                                    + (uint32_t)ac->timeout_ms);
    }
    lua_pushinteger(L, deadline_ms);
    lua_setfield(L, LUA_REGISTRYINDEX, "__deadline_ms");
    lua_sethook(L, cap_lua_cancel_hook, LUA_MASKCOUNT, LUA_CANCEL_INSTR_FREQ);

    lua_pushcfunction(L, cap_lua_capture_print);
    lua_setglobal(L, "print");

    /* Execute top-level (defines run()) */
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        const char *lerr = lua_tostring(L, -1);
        RTK_LOGE(TAG, "lua_async_task: top-level error: %s\n", lerr);
        if (init_err) DiagSnPrintf(init_err, CLAW_LUA_INIT_ERR_MAX,
                                   "top-level error: %s", lerr ? lerr : "unknown");
        lua_close(L);
        async_ctx_free(ac);
        goto mark_failed;
    }
    lua_getglobal(L, "run");
    if (!lua_isfunction(L, -1)) {
        RTK_LOGE(TAG, "lua_async_task: no run() in %s\n", ac->path);
        if (init_err) strncpy(init_err, "script has no global run() function — add: function run(args) ... end (not local, not self-executing)", CLAW_LUA_INIT_ERR_MAX - 1);
        lua_close(L);
        async_ctx_free(ac);
        goto mark_failed;
    }

    /* parse args JSON and push args table */
    {
        cJSON *args_obj = ac->args_json ? cJSON_Parse(ac->args_json) : cJSON_CreateObject();
        /* Origin already copied into the registry above; safe to free ac now. */
        async_ctx_free(ac); ac = NULL;
        cap_lua_push_args_table(L, args_obj);
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
        char buf[200];
        if (timed_out && !stopped_flag) {
            /* A deadline hit is a CONFIGURED LIMIT, not a script bug. Phrase it
             * so the LLM does not "debug" a perfectly healthy long-running
             * script (which previously sent it into a retry spiral). */
            int tmo = slot ? slot->timeout_ms : 0;
            DiagSnPrintf(buf, sizeof(buf),
                "job reached its timeout_ms=%d limit and was stopped. This is "
                "the configured wall-clock limit, NOT a script error. For an "
                "unbounded monitor/animation, call lua_run_async WITHOUT "
                "timeout_ms so it runs until lua_job_stop.", tmo);
        } else {
            DiagSnPrintf(buf, sizeof(buf), "run() error: %s", err ? err : "unknown");
        }
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

    free(init_err);
    lua_close(L);
    /* This job is now terminal; if it was the last one running, reclaim any
     * job-scoped thread.sync objects it (or a stopped peer) left behind. */
    cap_lua_notify_if_quiescent();
    rtos_task_delete(NULL);
    return;

mark_failed:
    /* Lua init/load failed before the main path — mark slot as FAILED and
     * write the captured init_err (if any) into the job log so lua_job_get
     * returns a non-empty log with the actual failure reason. */
    if (rtos_mutex_take(s_jobs_lock, RTOS_MAX_DELAY) == RTK_SUCCESS) {
        lua_job_t *j = job_find_by_id(id);
        if (j) {
            j->status = LUA_JOB_FAILED;
            j->finished_ms = rtos_time_get_current_system_time_ms();
            if (init_err && init_err[0]) {
                job_log_append(j, init_err, strlen(init_err));
                job_log_append(j, "\n", 1);
            }
        }
        rtos_mutex_give(s_jobs_lock);
    }
    RTK_LOGI(TAG, "lua_async_task END job_id=%u status=FAILED elapsed=%ums\n",
             (unsigned)id,
             (unsigned)(rtos_time_get_current_system_time_ms() - async_start_ms));
    free(init_err);
    /* Same reclaim-on-quiescence backstop as the normal terminal path. */
    cap_lua_notify_if_quiescent();
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

/* Shared core. allow_tmp is true only for the direct-LLM-tool-call JSON path
 * (cap_lua_run_async with ctx->caller == CLAW_CAP_CALLER_LLM) — preserves the
 * pre-D2-refactor exception exactly. The public plain-C entry point
 * (cap_lua_run_script_async) always passes false: a nested job launch from a
 * running script is never that direct-LLM-tool-call case. */
static int cap_lua_run_script_async_impl(const char *path, const char *args_json, int timeout_ms,
                                          const char *name_in, const char *exclusive_in, bool replace,
                                          const char *origin_channel, const char *origin_chat,
                                          bool allow_tmp, char **output)
{
    const char *perr = cap_lua_validate_path(path);
    if (perr) {
        claw_cap_set_output(output, "{\"error\":\"%s\"}", perr);
        return RTK_FAIL;
    }
    if (strlen(path) >= LUA_JOB_PATH_MAX) {
        claw_cap_set_output(output, "{\"error\":\"path too long for async job\"}");
        return RTK_FAIL;
    }
    /* Same invariant as lua_run: vfs:/tmp/ scripts are wiped on reboot and
     * must not be enqueued as background jobs except for the direct-LLM
     * tool-call exception (allow_tmp). */
    if (!allow_tmp && strncmp(path, "vfs:/tmp/", 9) == 0) {
        claw_cap_set_output(output,
            "{\"error\":\"vfs:/tmp/ scripts are wiped on reboot and cannot be used "
            "as background jobs. Use vfs:/scripts/ for persistent scripts or "
            "vfs:/skills/ for skill-managed scripts.\"}");
        return RTK_FAIL;
    }

    char name[LUA_JOB_NAME_MAX], excl[LUA_JOB_NAME_MAX];
    name[0] = '\0';
    excl[0] = '\0';
    if (name_in) { strncpy(name, name_in, sizeof(name) - 1); name[sizeof(name) - 1] = '\0'; }
    if (exclusive_in) { strncpy(excl, exclusive_in, sizeof(excl) - 1); excl[sizeof(excl) - 1] = '\0'; }

    cJSON *args_obj = args_json ? cJSON_Parse(args_json) : NULL;

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
    /* Capture the caller's origin for event.notify(). strdup may fail → NULL,
     * which is non-fatal (notify simply won't have a channel to reach). The
     * caller's strings may be short-lived (an agent request, or the current
     * script's own registry-stashed origin), so we must own a copy: an async
     * job outlives the call that started it. */
    ac->channel = (origin_channel && origin_channel[0]) ? strdup(origin_channel) : NULL;
    ac->chat_id = (origin_chat && origin_chat[0]) ? strdup(origin_chat) : NULL;
    if (!ac->path) {
        rtos_mutex_give(s_jobs_lock);
        claw_cap_set_output(output, "{\"error\":\"out of memory\"}");
        async_ctx_free(ac);
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

    if (rtos_task_create(&slot->task, "lua_job", lua_async_task, ac,
                         CLAW_LUA_ASYNC_TASK_STACK, 1) != RTK_SUCCESS) {
        slot->job_id = 0;
        slot->status = LUA_JOB_FREE;
        rtos_mutex_give(s_jobs_lock);
        claw_cap_set_output(output, "{\"error\":\"failed to spawn job task\"}");
        async_ctx_free(ac);
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

int cap_lua_run_script_async(const char *path, const char *args_json, int timeout_ms,
                              const char *name, const char *exclusive, bool replace,
                              const char *origin_channel, const char *origin_chat,
                              char **output)
{
    return cap_lua_run_script_async_impl(path, args_json, timeout_ms, name, exclusive, replace,
                                          origin_channel, origin_chat, false, output);
}

int cap_lua_run_async(const char *input_json,
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
    char path[LUA_JOB_PATH_MAX];
    strncpy(path, jpath->valuestring, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    /* This JSON tool allows vfs:/tmp/ paths for direct LLM tool-calls only —
     * preserved exactly as before the D2 refactor. */
    bool allow_tmp = ctx && ctx->caller == CLAW_CAP_CALLER_LLM;
    if (!allow_tmp && strncmp(path, "vfs:/tmp/", 9) == 0) {
        cJSON_Delete(root);
        claw_cap_set_output(output,
            "{\"error\":\"vfs:/tmp/ scripts are wiped on reboot and cannot be used "
            "in scheduled or internal calls. Use vfs:/scripts/ for persistent scripts "
            "or vfs:/skills/ for skill-managed scripts.\"}");
        return RTK_FAIL;
    }

    int timeout_ms = 0;
    cJSON *jto = cJSON_GetObjectItem(root, "timeout_ms");
    if (jto && cJSON_IsNumber(jto) && jto->valueint > 0) timeout_ms = jto->valueint;

    char name[LUA_JOB_NAME_MAX], excl[LUA_JOB_NAME_MAX];
    copy_field(name, sizeof(name), root, "name");
    copy_field(excl, sizeof(excl), root, "exclusive");
    bool replace = false;
    cJSON *jrep = cJSON_GetObjectItem(root, "replace");
    if (jrep && cJSON_IsBool(jrep)) replace = cJSON_IsTrue(jrep);

    cJSON *args_obj = cJSON_DetachItemFromObject(root, "args");
    char *args_json = args_obj ? cJSON_PrintUnformatted(args_obj) : NULL;
    cJSON_Delete(args_obj);
    cJSON_Delete(root);

    int rc = cap_lua_run_script_async_impl(path, args_json, timeout_ms, name, excl, replace,
                                            ctx ? ctx->channel : NULL, ctx ? ctx->chat_id : NULL,
                                            allow_tmp, output);
    free(args_json);
    return rc;
}

/* ---- execute: lua_job_get -------------------------------------------------- */

int cap_lua_get_job(uint32_t id, uint32_t since, char **output)
{
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

    /* Append [TRUNCATED] marker so LLM can see truncation inline in the log. */
    if (truncated && logn + 11 <= LUA_JOB_LOG_SIZE) {
        memcpy(logbuf + logn, "[TRUNCATED]", 11);
        logbuf[logn + 11] = '\0';
        logn += 11;
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
    /* The log can contain a Lua error whose "[string \"...\"]" chunkid was
     * byte-truncated mid-UTF-8-character by luaO_chunkid, leaving a dangling
     * lead byte.  Repair it here so this tool result (and the AT+CLAW view of
     * the job log) is always valid UTF-8 regardless of the downstream sink. */
    claw_utf8_sanitize_inplace(esc);

    int set_rc = claw_cap_set_output(output,
        "{\"job_id\":%u,\"status\":\"%s\",\"path\":\"%s\",\"log_seq\":%u,"
        "\"log_truncated\":%s,\"started_ms\":%u,\"finished_ms\":%u,\"log\":\"%s\"}",
        (unsigned)id, status_s, path_s, (unsigned)log_seq,
        truncated ? "true" : "false", (unsigned)started, (unsigned)finished, esc);
    free(logbuf);
    free(esc);
    return set_rc;
}

int cap_lua_job_get(const char *input_json,
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
    return cap_lua_get_job(id, since, output);
}

/* ---- execute: lua_job_list ------------------------------------------------- */

int cap_lua_list_jobs(char **output)
{
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

int cap_lua_job_list(const char *input_json,
                     const claw_cap_call_context_t *ctx,
                     char **output)
{
    (void)input_json; (void)ctx;
    return cap_lua_list_jobs(output);
}

/* ---- execute: lua_job_stop ------------------------------------------------- */

int cap_lua_stop_job(uint32_t id, char **output)
{
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
    lua_job_status_t final = LUA_JOB_RUNNING;   /* live status if it never terminates */
    bool terminal = false;
    while (waited <= LUA_JOB_STOP_WAIT_MS) {
        rtos_time_delay_ms(50);
        waited += 50;
        if (rtos_mutex_take(s_jobs_lock, RTOS_MAX_DELAY) == RTK_SUCCESS) {
            lua_job_t *j = job_find_by_id(id);
            if (!j || job_status_terminal(j->status)) {
                terminal = true;
                final = j ? j->status : LUA_JOB_STOPPED;
            } else {
                final = j->status;   /* still QUEUED/RUNNING — report it honestly */
            }
            rtos_mutex_give(s_jobs_lock);
        }
        if (terminal) {
            break;
        }
    }

    if (!terminal) {
        /* The stop flag IS set, but the job has not reached a terminal state
         * within the grace period. Do NOT claim it stopped (the old code always
         * returned stopped:true/STOPPED, which contradicted lua_job_list and sent
         * the LLM into a stop→list→still-RUNNING loop). Tell the truth + give an
         * actionable next step. With cancel-aware pcall this should be rare — it
         * means the job is wedged in a long C call (e.g. a blocking network read)
         * that has no cancel checkpoint. */
        return claw_cap_set_output(output,
            "{\"job_id\":%u,\"stopped\":false,\"status\":\"%s\",\"stop_requested\":true,"
            "\"hint\":\"stop signalled but job still %s after %ums; it is likely "
            "blocked in a long C call with no cancel checkpoint. The flag is "
            "latched and it will exit at the next checkpoint. Re-check with "
            "lua_job_list in a moment instead of calling stop again.\"}",
            (unsigned)id, job_status_str(final), job_status_str(final),
            (unsigned)LUA_JOB_STOP_WAIT_MS);
    }

    return claw_cap_set_output(output,
        "{\"job_id\":%u,\"stopped\":true,\"status\":\"%s\"}",
        (unsigned)id, job_status_str(final));
}

int cap_lua_job_stop(const char *input_json,
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
    return cap_lua_stop_job(id, output);
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

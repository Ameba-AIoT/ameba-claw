/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * cap_lua — Lua execution primitive (improvement #12 Inc 3).
 *
 * Responsibility split (see design_spec/ for architecture details):
 *   - cap_skill  : skill MANAGEMENT (list/activate/save/delete/deactivate),
 *                  SKILL.md injection, {CUR_SKILL_DIR} expansion.
 *   - cap_lua    : Lua EXECUTION — strict sandbox, per-run lua_State,
 *                  timeout + cancel hook, run a script BY PATH.
 *
 * The single tool exposed is `lua_run(path, args[, timeout_ms])`. cap_lua only
 * knows about paths, never about skills — that is the clean decoupling.
 */
#pragma once
#include "claw_compat.h"
#include <stdbool.h>
#include <stdint.h>

int cap_lua_init(void);

/* ── Plain-C job API (D2, design_spec/lua/lua_module_thread_architecture.md) ──
 *
 * Core of the async job system, factored out of the LLM-facing JSON tools
 * (lua_run_async / lua_job_{get,list,stop} in cap_lua_cmd.c, whose execute()
 * bodies now just parse JSON and call these) so lua_module_thread's Lua
 * bindings (thread.run/start/list/get/stop) can drive the SAME job table
 * without going through claw_cap_call() — no faking a
 * claw_cap_call_context_t, no JSON round-trip on the way in.
 *
 * All `output` params are malloc'd JSON strings (same shape the JSON tools
 * already returned) — caller frees. Returns RTK_SUCCESS/RTK_FAIL.
 *
 * Unlike the JSON tool (cap_lua_run_async), which allows vfs:/tmp/ paths
 * for LLM callers only, this entry point always rejects vfs:/tmp/ — a
 * nested job launch from a running script is never the direct-LLM-tool-call
 * case that exception exists for. */

/* Run a script synchronously, blocking the caller until it finishes or hits
 * timeout_ms (<=0 -> LUA_EXEC_TIMEOUT_MS default). Shares the same
 * LUA_JOB_MAX_RUNNING budget as async jobs, so a script calling this from
 * inside its own job consumes a second concurrent slot for the duration. */
int cap_lua_run_script(const char *path, const char *args_json, int timeout_ms,
                       const char *origin_channel, const char *origin_chat,
                       char **output);

/* Start a background job. origin_channel/origin_chat may be NULL (no
 * event.notify() target) — a script orchestrating child jobs should pass
 * through its OWN origin (read via event.origin()) so children can still
 * event.notify() the same user. */
int cap_lua_run_script_async(const char *path, const char *args_json, int timeout_ms,
                              const char *name, const char *exclusive, bool replace,
                              const char *origin_channel, const char *origin_chat,
                              char **output);

/* List all live job slots. */
int cap_lua_list_jobs(char **output);

/* Get one job's status + ring log tail. since_seq=0 returns the whole log. */
int cap_lua_get_job(uint32_t job_id, uint32_t since_seq, char **output);

/* Request a job stop; waits up to the grace period for it to terminate. */
int cap_lua_stop_job(uint32_t job_id, char **output);

/* Stop all running lua_async jobs whose script path matches `path`.
 * Returns the number of jobs signalled. Waits up to ~400 ms for them to
 * reach a terminal state so WiFi sockets are released before the caller
 * continues. */
int cap_lua_stop_jobs_for_path(const char *path);

/* The correct way to delete a .lua file.
 * Stops any running lua_async job for `path` first, then calls remove().
 * Use this everywhere instead of remove() on .lua files — never call
 * remove() on a .lua path directly. Returns remove()'s return value. */
int cap_lua_file_remove(const char *path);

/* Wipe vfs:/tmp and re-create the directory — same as the boot-time init.
 * Call after session,clear so each independent test round starts clean. */
void cap_lua_scratch_reset(void);

/* Register a callback fired when the Lua job world goes fully QUIESCENT — the
 * moment the last active async job (or synchronous lua_run) terminates and
 * nothing else is running. At that instant no Lua code can be inside any wait,
 * so it is the one safe point to garbage-collect process-global Lua state.
 *
 * Used by lua_module_thread to reclaim job-scoped thread.sync objects (queues /
 * semaphores / locks): a script that creates them and is then stopped or crashes
 * cannot run its own cleanup, so without this the objects — and any leaked
 * semaphore count / held lock — would survive and poison the NEXT run that
 * reuses the same name. cap_lua stays agnostic about what gets reclaimed; it
 * only knows WHEN it is safe. Pass NULL to clear. One slot; last registration
 * wins (re-registering the same fn on each lua_State open is harmless). */
void cap_lua_set_quiescence_cb(void (*cb)(void));

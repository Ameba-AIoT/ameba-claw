/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * cap_lua_internal.h — private surface shared between the three cap_lua
 * translation units (M1-4 split; behaviour-preserving):
 *
 *   cap_lua_runtime.c — the Lua execution primitive: strict sandbox, restricted
 *                       require(), JSON->Lua arg decoding, per-run lua_State,
 *                       cancel hook, print capture, and the synchronous lua_run.
 *   cap_lua_async.c   — the bounded background job table: lua_run_async and the
 *                       lua_job_{get,list,stop} tools, ring log, and the shared
 *                       sync+async concurrency budget.
 *   cap_lua_cmd.c     — the cap descriptor table, group registration
 *                       (cap_lua_init) and the vfs:/tmp scratch lifecycle.
 *
 * The public API (cap_lua_init / cap_lua_stop_jobs_for_path /
 * cap_lua_file_remove) lives in cap_lua.h. This header is NOT installed for
 * other caps — only the three files above include it.
 */
#pragma once

#include "lua.h"
#include "lauxlib.h"
#include <cJSON.h>
#include <stddef.h>
#include <stdbool.h>
#include "claw_cap.h"   /* claw_cap_call_context_t */

/* ---- Shared compile-time limits (runtime + async) -------------------------- */

/* Maximum script size read from the filesystem (runtime read + async read). */
#define LUA_SCRIPT_MAX        65536
/* Cancel hook fires every N Lua instructions (sync + async). */
#define LUA_CANCEL_INSTR_FREQ 500
/* Shared concurrency cap: synchronous lua_run + async jobs together. */
#define LUA_JOB_MAX_RUNNING   2
/* Fixed async job table size (also reported by cap_lua_init's startup log). */
#define LUA_JOB_SLOTS         4
/* Per-job ring log buffer in bytes (reported by cap_lua_init's startup log). */
#define LUA_JOB_LOG_SIZE      4096

/* ============================================================================
 * cap_lua_runtime.c — Lua execution primitive
 * ==========================================================================*/

/* Validate `path` against the allowed roots, ".lua" suffix and no "..".
 * Returns NULL on success, or a static human-readable error string. */
const char *cap_lua_validate_path(const char *path);

/* Read file into a malloc'd buffer (caller frees). Returns NULL on error. */
char *cap_lua_read_file_alloc(const char *path, size_t max_size);

/* Open the strict sandbox on a fresh lua_State (safe stdlib subset + sandbox-
 * safe Ameba modules + restricted require). */
void cap_lua_install_sandbox(lua_State *L);

/* Count hook: enforces cooperative cancel (__cancel_ptr) and the optional
 * wall-clock deadline (__deadline_ms) stashed in the registry. */
void cap_lua_cancel_hook(lua_State *L, lua_Debug *ar);

/* Push the decoded args object as a Lua table (empty table if NULL/scalar). */
void cap_lua_push_args_table(lua_State *L, const cJSON *args_obj);

/* Unified print() capture installed as the sandbox's global print(). Routes to
 * the sync stdout buffer (__print_cap) or, failing that, delegates the line to
 * the async ring log via cap_lua_async_capture_print(). */
int cap_lua_capture_print(lua_State *L);

/* execute: lua_run (synchronous). Registered by cap_lua_cmd.c. */
int cap_lua_run(const char *input_json,
                const claw_cap_call_context_t *ctx, char **output);

/* ============================================================================
 * cap_lua_async.c — background job table + shared concurrency budget
 * ==========================================================================*/

/* Create the single mutex guarding the async job table + sync run counter.
 * Idempotent. Called once from cap_lua_init before group registration. */
int cap_lua_async_init_lock(void);

/* Reserve a slot in the shared sync+async budget for a synchronous lua_run.
 * Returns:
 *   0 — slot acquired (caller MUST cap_lua_sync_slot_release() on completion),
 *   1 — busy (already at LUA_JOB_MAX_RUNNING; caller should reject),
 *   2 — lock unavailable within the short timeout (caller proceeds anyway, as
 *       the pre-split code did — the end-of-run release is a no-op guard). */
int cap_lua_sync_slot_acquire(void);

/* Release a synchronous lua_run slot (decrements with an underflow guard).
 * Safe to call on every completion/early-error path, mirroring the original
 * unconditional release. */
void cap_lua_sync_slot_release(void);

/* Async branch of the print dispatcher: append `line`/`n` to the running job's
 * ring log (looked up via __job_slot). No-op if not in an async context. */
void cap_lua_async_capture_print(lua_State *L, const char *line, size_t n);

/* execute: async job tools. Registered by cap_lua_cmd.c. */
int cap_lua_run_async(const char *input_json,
                      const claw_cap_call_context_t *ctx, char **output);
int cap_lua_job_get(const char *input_json,
                    const claw_cap_call_context_t *ctx, char **output);
int cap_lua_job_list(const char *input_json,
                     const claw_cap_call_context_t *ctx, char **output);
int cap_lua_job_stop(const char *input_json,
                     const claw_cap_call_context_t *ctx, char **output);

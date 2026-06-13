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

int cap_lua_init(void);

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

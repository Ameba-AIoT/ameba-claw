/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * lua_module_thread_internal.h — shared declarations between the three
 * lua_module_thread translation units:
 *
 *   thread_sync.c        — the `thread.sync` queue/sem/lock registry.
 *   thread_job.c          — thread.run/start/list/get/stop job bindings.
 *   lua_module_thread.c  — luaopen_thread, assembles the `thread` table.
 *
 * Not installed for other modules — only these three .c files include it.
 */
#pragma once

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Push a fresh `thread.sync` table (queue_create/send/recv/delete,
 * sem_create/give/take/delete, lock_create/lock/unlock/lock_delete) onto the
 * stack. Returns 1 (matches lua_CFunction convention, though it is called
 * directly rather than as a CFunction). */
int lua_module_thread_push_sync(lua_State *L);

/* Idempotent: create the registry mutex guarding thread.sync's object list.
 * Safe to call from luaopen_thread on every new lua_State. */
int thread_sync_init(void);

/* Add run/start/list/get/stop to the table at the top of the stack (same
 * convention as luaL_setfuncs — the target table must already be on top). */
void lua_module_thread_register_job_funcs(lua_State *L);

#ifdef __cplusplus
}
#endif

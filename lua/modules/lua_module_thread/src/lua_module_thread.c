/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * lua_module_thread.c — require("thread"): job orchestration + cross-job
 * sync primitives for skill scripts. See
 * internal_project_ctrl/dev_schedule/core_lua_gap_plan.md 批次一 #1 and
 * design_spec/lua/lua_module_thread_architecture.md for the full plan.
 */
#include "lua_module_thread_internal.h"

#include "lauxlib.h"

LUAMOD_API int luaopen_thread(lua_State *L)
{
    if (thread_sync_init() != 0) {
        luaL_error(L, "thread: failed to init sync registry lock");
    }

    lua_newtable(L);

    lua_module_thread_register_job_funcs(L);

    lua_module_thread_push_sync(L);
    lua_setfield(L, -2, "sync");

    return 1;
}

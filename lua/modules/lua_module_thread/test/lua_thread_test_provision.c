/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * lua_thread_test_provision.c — Writes the lua_module_thread test scripts to
 * vfs:/skills/ at boot, so they can be run through the real skill sandbox
 * (which is where "thread" is actually installed) via e.g.:
 *   AT+CLAW=lua_execute_sync,vfs:/skills/test_thread_sync.lua
 *
 * Each embedded string is generated at configure time from its .lua source
 * by lua/CMakeLists.txt's _embed_lua() macro — edit the .lua sources, not
 * this file's generated headers.
 */

#include <stdio.h>
#include <string.h>

#include "test_thread_sync_lua.h"
#include "thread_full_flow_parent_lua.h"
#include "thread_full_flow_child_producer_lua.h"
#include "thread_full_flow_child_consumer_lua.h"
#include "test_thread_stop_lua.h"
#include "thread_stop_child_blocker_lua.h"
#include "test_thread_job_named_replace_lua.h"
#include "test_thread_job_exclusive_lua.h"

static void write_once(const char *path, const char *script)
{
    { FILE *_ck = fopen(path, "r"); if (_ck) { fclose(_ck); return; } }

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    fwrite(script, 1, strlen(script), f);
    fclose(f);
}

void lua_module_thread_provision(void)
{
    write_once("vfs:/skills/test_thread_sync.lua", s_thread_test_script);
    write_once("vfs:/skills/thread_full_flow_parent.lua", s_thread_full_flow_parent_script);
    write_once("vfs:/skills/thread_full_flow_child_producer.lua", s_thread_full_flow_child_producer_script);
    write_once("vfs:/skills/thread_full_flow_child_consumer.lua", s_thread_full_flow_child_consumer_script);
    write_once("vfs:/skills/test_thread_stop.lua", s_thread_stop_test_script);
    write_once("vfs:/skills/thread_stop_child_blocker.lua", s_thread_stop_child_blocker_script);
    write_once("vfs:/skills/test_thread_job_named_replace.lua", s_thread_job_named_replace_script);
    write_once("vfs:/skills/test_thread_job_exclusive.lua", s_thread_job_exclusive_script);
}

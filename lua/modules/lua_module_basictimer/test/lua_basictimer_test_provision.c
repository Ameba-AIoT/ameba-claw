/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_basictimer_test_provision.c — Writes the basic timer test script to VFS
** on first boot and provides lua_basictimer_run() for AT+CLAW=basic,timer.
**
** Source is embedded at configure time via basictimer_test_lua.h
** (generated from test_basictimer.lua by lua_to_cstr.py).
*/

#include <stdio.h>
#include <string.h>

#include "ameba_soc.h"
#include "os_wrapper.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "basictimer_test_lua.h"

void lua_driver_basictimer_provision(void)
{
    const char *path = "vfs:test_basictimer.lua";
    { FILE *_ck = fopen(path, "r"); if (_ck) { fclose(_ck); return; } }

    FILE *f = fopen(path, "w");
    if (f) {
        fwrite(s_basictimer_test_script, 1, strlen(s_basictimer_test_script), f);
        fclose(f);
    }
}

/* ── On-demand execution via AT+CLAW=basic,timer ── */

typedef struct {
    const char       *script;
    SemaphoreHandle_t done;
} basictimer_task_arg_t;

static void basictimer_lua_task(void *param)
{
    basictimer_task_arg_t *arg = (basictimer_task_arg_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[basictimer] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        if (luaL_loadstring(L, arg->script) != LUA_OK) {
            printf("[basictimer] parse error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            printf("[basictimer] runtime error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_close(L);
    }

    xSemaphoreGive(arg->done);
    rtos_task_delete(NULL);
}

void lua_basictimer_run(void)
{
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        printf("[basictimer] semaphore create failed\n");
        return;
    }

    basictimer_task_arg_t arg = { .script = s_basictimer_test_script, .done = done };

    if (rtos_task_create(NULL, "bt_lua_task", basictimer_lua_task, &arg,
                         8192, 1) != RTK_SUCCESS) {
        printf("[basictimer] task create failed\n");
        vSemaphoreDelete(done);
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}

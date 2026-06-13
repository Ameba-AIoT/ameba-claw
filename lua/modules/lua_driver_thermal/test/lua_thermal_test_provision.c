/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_thermal_test_provision.c — Writes the thermal test script to VFS on every boot.
**
**   test_thermal.lua — Thermal sensor read test (5 samples).
**                      Source embedded via thermal_test_lua.h.
**
** Trigger via AT command:
**   AT+CLAW=thermal   — run thermal test (power-on temp + 5 samples + max/min)
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

#include "thermal_test_lua.h"

void lua_driver_thermal_provision(void)
{
    const char *path = "vfs:test_thermal.lua";
    FILE *f = fopen(path, "w");
    if (f) {
        fwrite(s_thermal_test_script, 1, strlen(s_thermal_test_script), f);
        fclose(f);
    }
}

/* ── On-demand execution via AT+CLAW=thermal ── */

typedef struct {
    const char       *script;
    SemaphoreHandle_t done;
} thermal_task_arg_t;

static void thermal_lua_task(void *param)
{
    thermal_task_arg_t *arg = (thermal_task_arg_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[thermal] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        if (luaL_loadstring(L, arg->script) != LUA_OK) {
            printf("[thermal] parse error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            printf("[thermal] runtime error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_close(L);
    }

    xSemaphoreGive(arg->done);
    rtos_task_delete(NULL);
}

void lua_thermal_run(void)
{
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        printf("[thermal] semaphore create failed\n");
        return;
    }

    thermal_task_arg_t arg = { .script = s_thermal_test_script, .done = done };

    if (rtos_task_create(NULL, "thermal_lua_task", thermal_lua_task, &arg,
                         4096, 1) != RTK_SUCCESS) {
        printf("[thermal] task create failed\n");
        vSemaphoreDelete(done);
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}

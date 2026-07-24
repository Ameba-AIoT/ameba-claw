/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_thermal_test_provision.c — Writes the thermal test script to VFS on every boot.
**
**   test_thermal.lua — Thermal sensor read test.
**                      Source embedded via thermal_test_lua.h.
**
** Trigger via AT command:
**   AT+CLAW=thermal             -- run with defaults (5 reads, 200 ms interval)
**   AT+CLAW=thermal,<count>     -- override read count
**   AT+CLAW=thermal,<count>,<interval_ms>  -- override both
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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
    { FILE *_ck = fopen(path, "r"); if (_ck) { fclose(_ck); return; } }

    FILE *f = fopen(path, "w");
    if (f) {
        fwrite(s_thermal_test_script, 1, strlen(s_thermal_test_script), f);
        fclose(f);
    }
}

/* ── Shared task runner ── */

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

static void run_script(const char *tag, const char *script, size_t stack)
{
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        printf("[%s] semaphore create failed\n", tag);
        return;
    }

    thermal_task_arg_t arg = { .script = script, .done = done };

    if (rtos_task_create(NULL, tag, thermal_lua_task, &arg,
                         stack, 1) != RTK_SUCCESS) {
        printf("[%s] task create failed\n", tag);
        vSemaphoreDelete(done);
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}

/* ── AT+CLAW=thermal[,<count>[,<interval_ms>]] ──
 *
 * Pass -1 for a parameter to use the Lua script's built-in default.
 *   count       : number of temperature readings (default 5)
 *   interval_ms : delay between readings in ms   (default 200)
 */
void lua_thermal_run(int count, int interval_ms)
{
    char   prefix[80];
    int    plen     = 0;
    char  *combined = NULL;
    const char *script = s_thermal_test_script;

    if (count > 0 || interval_ms > 0) {
        int c = (count       >= 0) ? count       : 5;
        int d = (interval_ms >= 0) ? interval_ms : 200;
        plen = snprintf(prefix, sizeof(prefix),
                        "args={count=%d,interval_ms=%d}\n", c, d);
        size_t base_len = strlen(s_thermal_test_script);
        combined = (char *)malloc((size_t)plen + base_len + 1);
        if (!combined) {
            printf("[thermal] malloc failed\n");
            return;
        }
        memcpy(combined, prefix, (size_t)plen);
        memcpy(combined + plen, s_thermal_test_script, base_len + 1);
        script = combined;
    }

    run_script("thermal_lua_task", script, 4096);
    free(combined);
}

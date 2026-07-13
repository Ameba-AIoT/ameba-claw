/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_captouch_test_provision.c
**
** Trigger via AT command:
**   AT+CLAW=captouch        — interactive test (touch PA_17 with finger)
**   AT+CLAW=captouch,ext    — ext raw-monitor test (no touch required)
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

#include "captouch_test_lua.h"

void lua_driver_captouch_provision(void)
{
    const char *path = "vfs:test_captouch.lua";
    FILE *f = fopen(path, "w");
    if (f) {
        fwrite(s_captouch_test_script, 1, strlen(s_captouch_test_script), f);
        fclose(f);
    }
}

/* ── On-demand execution via AT+CLAW=captouch[,ext] ── */

typedef struct {
    const char       *script;
    const char       *mode;
    SemaphoreHandle_t done;
} captouch_task_arg_t;

static void captouch_lua_task(void *param)
{
    captouch_task_arg_t *arg = (captouch_task_arg_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[captouch] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        lua_pushstring(L, arg->mode);
        lua_setglobal(L, "CAPTOUCH_TEST_MODE");
        if (luaL_loadstring(L, arg->script) != LUA_OK) {
            printf("[captouch] parse error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            printf("[captouch] runtime error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_close(L);
    }

    xSemaphoreGive(arg->done);
    rtos_task_delete(NULL);
}

void lua_captouch_run(const char *mode)
{
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        printf("[captouch] semaphore create failed\n");
        return;
    }

    captouch_task_arg_t arg = {
        .script = s_captouch_test_script,
        .mode   = mode ? mode : "interactive",
        .done   = done,
    };

    if (rtos_task_create(NULL, "captouch_lua_task", captouch_lua_task, &arg,
                         8192, 1) != RTK_SUCCESS) {
        printf("[captouch] task create failed\n");
        vSemaphoreDelete(done);
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}

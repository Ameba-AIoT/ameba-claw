/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_touch_test_provision.c
**
** Trigger via AT command:
**   AT+CLAW=touch        — interactive test (touch PA_17 with finger)
**   AT+CLAW=touch,ext    — ext raw-monitor test (no touch required)
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

#include "touch_test_lua.h"

void lua_driver_touch_provision(void)
{
    const char *path = "vfs:test_touch.lua";
    FILE *f = fopen(path, "w");
    if (f) {
        fwrite(s_touch_test_script, 1, strlen(s_touch_test_script), f);
        fclose(f);
    }
}

/* ── On-demand execution via AT+CLAW=touch[,ext] ── */

typedef struct {
    const char       *script;
    const char       *mode;
    SemaphoreHandle_t done;
} touch_task_arg_t;

static void touch_lua_task(void *param)
{
    touch_task_arg_t *arg = (touch_task_arg_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[touch] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        lua_pushstring(L, arg->mode);
        lua_setglobal(L, "TOUCH_TEST_MODE");
        if (luaL_loadstring(L, arg->script) != LUA_OK) {
            printf("[touch] parse error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            printf("[touch] runtime error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_close(L);
    }

    xSemaphoreGive(arg->done);
    rtos_task_delete(NULL);
}

void lua_touch_run(const char *mode)
{
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        printf("[touch] semaphore create failed\n");
        return;
    }

    touch_task_arg_t arg = {
        .script = s_touch_test_script,
        .mode   = mode ? mode : "interactive",
        .done   = done,
    };

    if (rtos_task_create(NULL, "touch_lua_task", touch_lua_task, &arg,
                         8192, 1) != RTK_SUCCESS) {
        printf("[touch] task create failed\n");
        vSemaphoreDelete(done);
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}

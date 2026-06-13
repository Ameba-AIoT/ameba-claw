/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_adc_test_provision.c
**
** Trigger via AT command:
**   AT+CLAW=adc        — run loopback test (PA_13 <-> PA_25 wired)
**   AT+CLAW=adc,ext    — run external supply test (supply on PA_13)
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

#include "adc_test_lua.h"

void lua_driver_adc_provision(void)
{
    const char *path = "vfs:test_adc.lua";
    FILE *f = fopen(path, "w");
    if (f) {
        fwrite(s_adc_test_script, 1, strlen(s_adc_test_script), f);
        fclose(f);
    }
}

/* ── On-demand execution via AT+CLAW=adc[,ext] ── */

typedef struct {
    const char       *script;
    const char       *mode;
    SemaphoreHandle_t done;
} adc_task_arg_t;

static void adc_lua_task(void *param)
{
    adc_task_arg_t *arg = (adc_task_arg_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[adc] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        lua_pushstring(L, arg->mode);
        lua_setglobal(L, "ADC_TEST_MODE");
        if (luaL_loadstring(L, arg->script) != LUA_OK) {
            printf("[adc] parse error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            printf("[adc] runtime error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_close(L);
    }

    xSemaphoreGive(arg->done);
    rtos_task_delete(NULL);
}

void lua_adc_run(const char *mode)
{
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        printf("[adc] semaphore create failed\n");
        return;
    }

    adc_task_arg_t arg = {
        .script = s_adc_test_script,
        .mode   = mode ? mode : "loopback",
        .done   = done,
    };

    if (rtos_task_create(NULL, "adc_lua_task", adc_lua_task, &arg,
                         8192, 1) != RTK_SUCCESS) {
        printf("[adc] task create failed\n");
        vSemaphoreDelete(done);
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}

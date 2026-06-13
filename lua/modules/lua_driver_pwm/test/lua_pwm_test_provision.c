/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_pwm_test_provision.c — Writes the PWM test script to VFS on every boot.
**
**   test_pwm.lua   — PWM API smoke-test (PA_6, TIM4 ch0), auto-run on boot.
**                    Source embedded via pwm_test_lua.h.
**
** Kept separate from lua_driver_pwm.c so the driver stays free of test code.
** Overwrites on every boot so the script always matches the current firmware.
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

#include "pwm_test_lua.h"

void lua_driver_pwm_provision(void)
{
    const char *path = "vfs:test_pwm.lua";
    { FILE *_ck = fopen(path, "r"); if (_ck) { fclose(_ck); return; } }

    FILE *f = fopen(path, "w");
    if (f) {
        fwrite(s_pwm_test_script, 1, strlen(s_pwm_test_script), f);
        fclose(f);
    }
}

/* ── On-demand execution via AT+CLAW=pwm ── */

typedef struct {
    const char       *script;
    SemaphoreHandle_t done;
} pwm_task_arg_t;

static void pwm_lua_task(void *param)
{
    pwm_task_arg_t *arg = (pwm_task_arg_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[pwm] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        if (luaL_loadstring(L, arg->script) != LUA_OK) {
            printf("[pwm] parse error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            printf("[pwm] runtime error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_close(L);
    }

    xSemaphoreGive(arg->done);
    rtos_task_delete(NULL);
}

void lua_pwm_run(void)
{
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        printf("[pwm] semaphore create failed\n");
        return;
    }

    pwm_task_arg_t arg = { .script = s_pwm_test_script, .done = done };

    if (rtos_task_create(NULL, "pwm_lua_task", pwm_lua_task, &arg,
                         8192, 1) != RTK_SUCCESS) {
        printf("[pwm] task create failed\n");
        vSemaphoreDelete(done);
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}

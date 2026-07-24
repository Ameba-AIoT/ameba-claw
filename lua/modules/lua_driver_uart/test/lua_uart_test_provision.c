/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_uart_test_provision.c — Writes test_uart.lua to VFS at boot and
** provides lua_uart_run() for the AT+CLAW=uart command.
**
** Kept separate from lua_driver_uart.c so the driver stays free of test code.
** The embedded string is generated from test/test_uart.lua via CMake
** execute_process (uart_test_lua.h / s_uart_test_script).
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

/* s_uart_test_script is auto-generated from test_uart.lua at cmake configure time. */
#include "uart_test_lua.h"

void lua_driver_uart_provision(void)
{
    const char *path = "vfs:test_uart.lua";
    { FILE *_ck = fopen(path, "r"); if (_ck) { fclose(_ck); return; } }

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    fwrite(s_uart_test_script, 1, strlen(s_uart_test_script), f);
    fclose(f);
}

/* ── On-demand execution via AT+CLAW=uart ── */

typedef struct {
    const char       *script;
    const char       *mode;
    SemaphoreHandle_t done;
} uart_task_arg_t;

static void uart_lua_task(void *param)
{
    uart_task_arg_t *arg = (uart_task_arg_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[uart] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        lua_pushstring(L, (arg->mode && arg->mode[0]) ? arg->mode : "loopback");
        lua_setglobal(L, "MODE");
        if (luaL_loadstring(L, arg->script) != LUA_OK) {
            printf("[uart] parse error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            printf("[uart] runtime error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_close(L);
    }

    xSemaphoreGive(arg->done);
    rtos_task_delete(NULL);
}

void lua_uart_run(const char *mode)
{
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        printf("[uart] semaphore create failed\n");
        return;
    }

    uart_task_arg_t arg = { .script = s_uart_test_script, .mode = mode, .done = done };

    if (rtos_task_create(NULL, "uart_lua_task", uart_lua_task, &arg,
                         8192, 1) != RTK_SUCCESS) {
        printf("[uart] task create failed\n");
        vSemaphoreDelete(done);
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}

/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_light_sensor_test_provision.c
**
** Writes the light sensor test script to VFS at boot.
**
** Trigger via AT command:
**   AT+CLAW=light_sensor              -- pin from board.json "light_sensor" device
**   AT+CLAW=light_sensor,PA_26        -- explicit pin override
*/

#include <stdio.h>
#include <string.h>

#include "ameba_soc.h"
#include "os_wrapper.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "light_sensor_test_lua.h"

void lua_module_light_sensor_provision(void)
{
    const char *path = "vfs:test_light_sensor.lua";
    FILE *f = fopen(path, "w");
    if (f) {
        fwrite(s_light_sensor_test_script, 1,
               strlen(s_light_sensor_test_script), f);
        fclose(f);
    }
}

typedef struct {
    const char  *script;
    const char  *do_pin;
    int          count;
    rtos_sema_t  done;
} light_sensor_task_arg_t;

static void light_sensor_lua_task(void *param)
{
    light_sensor_task_arg_t *arg = (light_sensor_task_arg_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[light_sensor] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        lua_newtable(L);
        /* Only set do_pin when explicitly supplied; NULL means Lua should
         * look it up from board.json (the "light_sensor" device entry). */
        if (arg->do_pin) {
            lua_pushstring(L, arg->do_pin);
            lua_setfield(L, -2, "do_pin");
        }
        lua_pushinteger(L, (lua_Integer)arg->count);
        lua_setfield(L, -2, "count");
        lua_setglobal(L, "args");

        if (luaL_loadstring(L, arg->script) != LUA_OK) {
            printf("[light_sensor] parse error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            printf("[light_sensor] runtime error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_close(L);
    }

    rtos_sema_give(arg->done);
    rtos_task_delete(NULL);
}

void lua_module_light_sensor_run(const char *do_pin, int count)
{
    rtos_sema_t done = NULL;
    if (rtos_sema_create_binary(&done) != RTK_SUCCESS || !done) {
        printf("[light_sensor] semaphore create failed\n");
        return;
    }

    light_sensor_task_arg_t arg = {
        .script = s_light_sensor_test_script,
        .do_pin = do_pin,           /* NULL = let Lua read from board.json */
        .count  = count > 0 ? count : 10,
        .done   = done,
    };

    if (rtos_task_create(NULL, "light_sensor_lua", light_sensor_lua_task, &arg,
                         4096, 1) != RTK_SUCCESS) {
        printf("[light_sensor] task create failed\n");
        rtos_sema_delete(done);
        return;
    }

    rtos_sema_take(done, RTOS_MAX_DELAY);
    rtos_sema_delete(done);
}

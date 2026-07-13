/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_environmental_sensor_test_provision.c
**
** Writes the environmental sensor test script to VFS at boot.
**
** Trigger via AT command:
**   AT+CLAW=env,dht11          -- DHT11 on default pin PB_8
**   AT+CLAW=env,dht11,PB_8    -- explicit pin
*/

#include <stdio.h>
#include <string.h>

#include "ameba_soc.h"
#include "os_wrapper.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "environmental_sensor_test_lua.h"

void lua_module_environmental_sensor_provision(void)
{
    const char *path = "vfs:test_environmental_sensor.lua";
    FILE *f = fopen(path, "w");
    if (f) {
        fwrite(s_environmental_sensor_test_script, 1,
               strlen(s_environmental_sensor_test_script), f);
        fclose(f);
    }
}

/* ── On-demand execution via AT+CLAW=environmental_sensor ── */

typedef struct {
    const char  *script;
    const char  *pin;
    rtos_sema_t  done;
} env_sensor_task_arg_t;

static void env_sensor_lua_task(void *param)
{
    env_sensor_task_arg_t *arg = (env_sensor_task_arg_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[environmental_sensor] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        /* Pass pin as args table */
        lua_newtable(L);
        lua_pushstring(L, arg->pin);
        lua_setfield(L, -2, "pin");
        lua_setglobal(L, "args");

        if (luaL_loadstring(L, arg->script) != LUA_OK) {
            printf("[environmental_sensor] parse error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            printf("[environmental_sensor] runtime error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_close(L);
    }

    rtos_sema_give(arg->done);
    rtos_task_delete(NULL);
}

void lua_module_environmental_sensor_run(const char *pin)
{
    rtos_sema_t done = NULL;
    if (rtos_sema_create_binary(&done) != RTK_SUCCESS || !done) {
        printf("[environmental_sensor] semaphore create failed\n");
        return;
    }

    env_sensor_task_arg_t arg = {
        .script = s_environmental_sensor_test_script,
        .pin    = pin ? pin : "PB_8",
        .done   = done,
    };

    if (rtos_task_create(NULL, "env_sensor_lua", env_sensor_lua_task, &arg,
                         4096, 1) != RTK_SUCCESS) {
        printf("[environmental_sensor] task create failed\n");
        rtos_sema_delete(done);
        return;
    }

    rtos_sema_take(done, RTOS_MAX_DELAY);
    rtos_sema_delete(done);
}

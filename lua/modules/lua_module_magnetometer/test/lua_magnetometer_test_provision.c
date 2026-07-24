/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_magnetometer_test_provision.c
**
** Writes the BMM150 magnetometer test script to VFS at boot and provides the
** AT-command runner used by atcmd_hw_test.
**
** Trigger via AT command:
**   AT+CLAW=magnetometer                                    -- board.json defaults
**   AT+CLAW=magnetometer,PA_26,PA_25                        -- explicit sda,scl
**   AT+CLAW=magnetometer,PA_26,PA_25,0,0x10                 -- + i2c, addr
**   AT+CLAW=magnetometer,PA_26,PA_25,0,0x10,PB_8            -- + INT pin
**   AT+CLAW=magnetometer,PA_26,PA_25,0,0x10,PB_8,10,200     -- + count, interval_ms
**
** Nothing is hard-coded: all parameters come from board.json (via AT harness)
** or are overridden by the explicit AT command arguments.
*/

#include <stdio.h>
#include <string.h>

#include "ameba_soc.h"
#include "os_wrapper.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "magnetometer_test_lua.h"

void lua_module_magnetometer_provision(void)
{
    const char *path = "vfs:test_magnetometer.lua";
    FILE *f = fopen(path, "w");
    if (f) {
        fwrite(s_magnetometer_test_script, 1,
               strlen(s_magnetometer_test_script), f);
        fclose(f);
    }
}

/* ---- On-demand execution via AT+CLAW=magnetometer ---- */

typedef struct {
    const char  *script;
    const char  *chip;
    const char  *sda;
    const char  *scl;
    int          i2c;
    int          addr;
    const char  *int_gpio;  /* may be NULL */
    int          count;
    int          interval_ms;
    rtos_sema_t  done;
} mag_task_arg_t;

static void mag_lua_task(void *param)
{
    mag_task_arg_t *arg = (mag_task_arg_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[magnetometer] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        /* Pass configuration as the global `args` table. */
        lua_newtable(L);
        lua_pushstring(L, arg->chip);
        lua_setfield(L, -2, "chip");
        lua_pushstring(L, arg->sda);
        lua_setfield(L, -2, "sda");
        lua_pushstring(L, arg->scl);
        lua_setfield(L, -2, "scl");
        lua_pushinteger(L, arg->i2c);
        lua_setfield(L, -2, "i2c");
        lua_pushinteger(L, arg->addr);
        lua_setfield(L, -2, "addr");
        if (arg->int_gpio && arg->int_gpio[0]) {
            lua_pushstring(L, arg->int_gpio);
            lua_setfield(L, -2, "int_gpio");
        }
        lua_pushinteger(L, arg->count);
        lua_setfield(L, -2, "count");
        lua_pushinteger(L, arg->interval_ms);
        lua_setfield(L, -2, "interval");
        lua_setglobal(L, "args");

        if (luaL_loadstring(L, arg->script) != LUA_OK) {
            printf("[magnetometer] parse error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            printf("[magnetometer] runtime error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_close(L);
    }

    rtos_sema_give(arg->done);
    rtos_task_delete(NULL);
}

void lua_module_magnetometer_run(const char *sda, const char *scl,
                                  int i2c, int addr, const char *int_gpio,
                                  int count, int interval_ms)
{
    rtos_sema_t done = NULL;
    if (rtos_sema_create_binary(&done) != RTK_SUCCESS || !done) {
        printf("[magnetometer] semaphore create failed\n");
        return;
    }

    mag_task_arg_t arg = {
        .script      = s_magnetometer_test_script,
        .chip        = "bmm150",
        .sda         = (sda && sda[0])   ? sda   : "PA_26",
        .scl         = (scl && scl[0])   ? scl   : "PA_25",
        .i2c         = (i2c >= 0)        ? i2c   : 0,
        .addr        = (addr > 0)        ? addr  : 0x10,
        .int_gpio    = (int_gpio && int_gpio[0]) ? int_gpio : NULL,
        .count       = (count > 0)       ? count : 10,
        .interval_ms = (interval_ms >= 0) ? interval_ms : 200,
        .done        = done,
    };

    if (rtos_task_create(NULL, "mag_lua", mag_lua_task, &arg,
                         4096, 1) != RTK_SUCCESS) {
        printf("[magnetometer] task create failed\n");
        rtos_sema_delete(done);
        return;
    }

    rtos_sema_take(done, RTOS_MAX_DELAY);
    rtos_sema_delete(done);
}

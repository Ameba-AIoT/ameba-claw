/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_imu_test_provision.c
**
** Writes the MPU-6050 IMU test script to VFS at boot and provides the
** AT-command runner used by handle_cmd_hw_test.
**
** Trigger via AT command:
**   AT+CLAW=imu,mpu6050                             -- default pins from board.json
**   AT+CLAW=imu,mpu6050,PA_26,PA_25                 -- explicit sda,scl
**   AT+CLAW=imu,mpu6050,PA_26,PA_25,1,0x68          -- explicit sda,scl,i2c,addr
**   AT+CLAW=imu,mpu6050,PA_26,PA_25,0,0x68,20,200   -- + count=20, interval=200ms
**
** Nothing is hard-coded here: chip / sda / scl / i2c / addr / count / interval
** all arrive as parameters (from board.json via the AT harness or skill) and are
** handed to the Lua test through the `args` table.
*/

#include <stdio.h>
#include <string.h>

#include "ameba_soc.h"
#include "os_wrapper.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "imu_test_lua.h"

void lua_module_imu_provision(void)
{
    const char *path = "vfs:test_imu.lua";
    FILE *f = fopen(path, "w");
    if (f) {
        fwrite(s_imu_test_script, 1, strlen(s_imu_test_script), f);
        fclose(f);
    }
}

/* ── On-demand execution via AT+CLAW=imu,mpu6050 ── */

typedef struct {
    const char  *script;
    const char  *chip;
    const char  *sda;
    const char  *scl;
    int          i2c;
    int          addr;
    int          count;
    int          interval_ms;
    rtos_sema_t  done;
} imu_task_arg_t;

static void imu_lua_task(void *param)
{
    imu_task_arg_t *arg = (imu_task_arg_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[imu] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        /* Pass configuration as the `args` table (no pins hard-coded in C). */
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
        lua_pushinteger(L, arg->count);
        lua_setfield(L, -2, "count");
        lua_pushinteger(L, arg->interval_ms);
        lua_setfield(L, -2, "interval");
        lua_setglobal(L, "args");

        if (luaL_loadstring(L, arg->script) != LUA_OK) {
            printf("[imu] parse error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            printf("[imu] runtime error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_close(L);
    }

    rtos_sema_give(arg->done);
    rtos_task_delete(NULL);
}

void lua_module_imu_run(const char *chip, const char *sda, const char *scl,
                        int i2c, int addr, int count, int interval_ms)
{
    rtos_sema_t done = NULL;
    if (rtos_sema_create_binary(&done) != RTK_SUCCESS || !done) {
        printf("[imu] semaphore create failed\n");
        return;
    }

    imu_task_arg_t arg = {
        .script      = s_imu_test_script,
        .chip        = (chip && chip[0]) ? chip : "mpu6050",
        .sda         = (sda && sda[0]) ? sda : "PA_26",
        .scl         = (scl && scl[0]) ? scl : "PA_25",
        .i2c         = (i2c >= 0) ? i2c : 0,
        .addr        = (addr > 0) ? addr : 0x68,
        .count       = (count > 0) ? count : 10,
        .interval_ms = (interval_ms >= 0) ? interval_ms : 100,
        .done        = done,
    };

    /* MPU-6050 uses a 14-byte burst read plus float temp math; 4 KB matches the
     * other sensor test tasks and leaves headroom for the fresh lua_State. */
    if (rtos_task_create(NULL, "imu_lua", imu_lua_task, &arg,
                         4096, 1) != RTK_SUCCESS) {
        printf("[imu] task create failed\n");
        rtos_sema_delete(done);
        return;
    }

    rtos_sema_take(done, RTOS_MAX_DELAY);
    rtos_sema_delete(done);
}

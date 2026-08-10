/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * lua_led_strip_test_provision.c — AT+CLAW=led test runners.
 *
 *   lua_led_strip_run(count)   -- AT+CLAW=led[,<n>]       one-shot demo (blocks)
 *   lua_led_strip_loop(count)  -- AT+CLAW=led,loop[,<n>]  background animation
 *   lua_led_strip_stop()       -- AT+CLAW=led,off         request stop
 *
 * Runs the embedded test_led_strip.lua (s_led_strip_test_script). The loop-stop
 * flags are defined in lua_module_led_strip.c and read by its stop_requested().
 * Compiled only when CONFIG_CLAW_ENABLE_TESTS is set (see lua/CMakeLists.txt).
 */

#include <string.h>

#include "ameba_soc.h"
#include "os_wrapper.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "test_led_strip_lua.h"

/* Defined in lua_module_led_strip.c; led_strip_loop_stop is read by
 * led_strip.stop_requested(). */
extern volatile int led_strip_loop_stop;
extern volatile int led_strip_loop_running;

typedef struct {
    int          count;
    const char  *mode;
    rtos_sema_t  done; /* NULL for detached loop task */
} led_task_arg_t;

static void led_lua_task(void *param)
{
    led_task_arg_t *arg   = (led_task_arg_t *)param;
    int             count = arg->count;
    char            mode[8];
    rtos_sema_t     done  = arg->done;

    strlcpy(mode, arg->mode, sizeof(mode));
    /* Signal caller that arg fields have been copied; arg may go out of scope. */
    if (done) {
        rtos_sema_give(done);
    }

    lua_State *L = luaL_newstate();
    if (!L) {
        RTK_LOGS(NOTAG, RTK_LOG_ERROR, "[led] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        lua_pushinteger(L, count);
        lua_setglobal(L, "NUM_LEDS");
        lua_pushstring(L, mode);
        lua_setglobal(L, "MODE");

        if (luaL_loadstring(L, s_led_strip_test_script) != LUA_OK) {
            RTK_LOGS(NOTAG, RTK_LOG_ERROR, "[led] parse error: %s\n",
                     lua_tostring(L, -1));
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            RTK_LOGS(NOTAG, RTK_LOG_ERROR, "[led] runtime error: %s\n",
                     lua_tostring(L, -1));
        }
        lua_close(L);
    }

    led_strip_loop_running = 0;
    rtos_task_delete(NULL);
}

/* AT+CLAW=led[,<count>] — one-shot demo (blocks caller until done). */
void lua_led_strip_run(int count)
{
    if (count < 1) {
        count = 15;
    }
    if (led_strip_loop_running) {
        RTK_LOGS(NOTAG, RTK_LOG_WARN,
                 "[led] loop running — send AT+CLAW=led,off first\n");
        return;
    }

    rtos_sema_t sync = NULL;
    if (rtos_sema_create_binary(&sync) != RTK_SUCCESS || !sync) {
        RTK_LOGS(NOTAG, RTK_LOG_ERROR, "[led] sema create failed\n");
        return;
    }

    led_task_arg_t arg = { .count = count, .mode = "demo", .done = sync };
    led_strip_loop_running = 1;
    led_strip_loop_stop    = 0;
    if (rtos_task_create(NULL, "led_demo", led_lua_task, &arg, 32768, 1)
            != RTK_SUCCESS) {
        RTK_LOGS(NOTAG, RTK_LOG_ERROR, "[led] task create failed\n");
        led_strip_loop_running = 0;
        rtos_sema_delete(sync);
        return;
    }
    /* Block until the task has copied arg fields. */
    rtos_sema_take(sync, RTOS_MAX_DELAY);
    rtos_sema_delete(sync);

    /* Now wait for the task to finish by polling led_strip_loop_running. */
    while (led_strip_loop_running) {
        rtos_time_delay_ms(20);
    }
}

/* AT+CLAW=led,loop[,<count>] — background animation; returns immediately. */
void lua_led_strip_loop(int count)
{
    if (count < 1) {
        count = 15;
    }
    if (led_strip_loop_running) {
        RTK_LOGS(NOTAG, RTK_LOG_WARN, "[led] already running\n");
        return;
    }

    led_strip_loop_stop    = 0;
    led_strip_loop_running = 1;

    rtos_sema_t sync = NULL;
    if (rtos_sema_create_binary(&sync) != RTK_SUCCESS || !sync) {
        RTK_LOGS(NOTAG, RTK_LOG_ERROR, "[led] sema create failed\n");
        led_strip_loop_running = 0;
        return;
    }

    led_task_arg_t arg = { .count = count, .mode = "loop", .done = sync };
    if (rtos_task_create(NULL, "led_loop", led_lua_task, &arg, 32768, 1)
            != RTK_SUCCESS) {
        RTK_LOGS(NOTAG, RTK_LOG_ERROR, "[led] task create failed\n");
        led_strip_loop_running = 0;
        rtos_sema_delete(sync);
        return;
    }
    /* Wait until the task has copied arg before we return (arg is on stack). */
    rtos_sema_take(sync, RTOS_MAX_DELAY);
    rtos_sema_delete(sync);

    RTK_LOGS(NOTAG, RTK_LOG_INFO, "[led] loop started (%d pixels)\n", count);
}

/* AT+CLAW=led,off — request stop and wait up to 3 s. */
void lua_led_strip_stop(void)
{
    if (!led_strip_loop_running) {
        RTK_LOGS(NOTAG, RTK_LOG_WARN, "[led] loop not running\n");
        return;
    }
    led_strip_loop_stop = 1;
    int waited = 0;
    while (led_strip_loop_running && waited < 3000) {
        rtos_time_delay_ms(20);
        waited += 20;
    }
    RTK_LOGS(NOTAG, RTK_LOG_INFO, "[led] stopped\n");
}

/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_pwm_test_provision.c — Writes the PWM test script to VFS on every boot.
**
**   test_pwm.lua   — PWM API smoke-test (PA_26, TIM4 ch3).
**                    Source embedded via pwm_test_lua.h.
**   test_servo.lua — SG90 servo sweep (PA_26, TIM4 ch3, 50 Hz).
**                    Source embedded via servo_test_lua.h.
**
** Kept separate from lua_driver_pwm.c so the driver stays free of test code.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ameba_soc.h"
#include "os_wrapper.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "pwm_test_lua.h"
#include "servo_test_lua.h"
#include "pwm_doc_md.h"

static void provision_once(const char *path, const char *content, size_t len)
{
    FILE *ck = fopen(path, "r");
    if (ck) { fclose(ck); return; }
    FILE *f = fopen(path, "w");
    if (f) { fwrite(content, 1, len, f); fclose(f); }
}

void lua_driver_pwm_provision(void)
{
    provision_once("vfs:test_pwm.lua", s_pwm_test_script, strlen(s_pwm_test_script));
    provision_once("vfs:pwm.md",       s_pwm_doc_md,       strlen(s_pwm_doc_md));
}

/* ── Shared task: runs whatever script pointer is in arg ── */

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

/* ── Run helper: spawn task, block until done, free combined script ── */

static void run_script(const char *tag, const char *script, size_t stack)
{
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        printf("[%s] semaphore create failed\n", tag);
        return;
    }

    pwm_task_arg_t arg = { .script = script, .done = done };

    if (rtos_task_create(NULL, tag, pwm_lua_task, &arg,
                         stack, 1) != RTK_SUCCESS) {
        printf("[%s] task create failed\n", tag);
        vSemaphoreDelete(done);
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}

/* ── AT+CLAW=pwm ── */

void lua_pwm_run(void)
{
    run_script("pwm_lua_task", s_pwm_test_script, 8192);
}

/* ── AT+CLAW=servo[,<start>,<end>[,<step>[,<delay_ms>]]] ──
 *
 * Parameters (-1 = use script default):
 *   start_angle   deg  default 45
 *   end_angle     deg  default 135
 *   step          deg  default 10
 *   delay_ms      ms   default 80
 *
 * Injects an `args` table in front of the servo script so the Lua
 * defaults are overridden without modifying the script source.
 */
/* lua_servo_run — run the servo sweep script with optional overrides.
 *   Pass -1 for any int param to use the Lua script's built-in default.
 *   start_angle == end_angle  →  "go to angle and hold" mode.
 */
void lua_servo_run(int start_angle, int end_angle, int step,
                   int delay_ms, int edge_hold_ms)
{
    char  prefix[160];
    int   plen = 0;
    char *combined = NULL;
    const char *script = s_servo_test_script;

    /* Build the args table only when at least one param was specified. */
    if (start_angle >= 0 || end_angle >= 0 || step > 0 ||
        delay_ms >= 0 || edge_hold_ms >= 0) {
        int sa = (start_angle  >= 0) ? start_angle  : 45;
        int ea = (end_angle    >= 0) ? end_angle    : 135;
        int st = (step         >  0) ? step          : 10;
        int dm = (delay_ms     >= 0) ? delay_ms      : 80;
        int eh = (edge_hold_ms >= 0) ? edge_hold_ms  : 500;

        plen = snprintf(prefix, sizeof(prefix),
                        "args={start_angle=%d,end_angle=%d,step=%d"
                        ",step_delay_ms=%d,edge_hold_ms=%d}\n",
                        sa, ea, st, dm, eh);

        size_t base_len = strlen(s_servo_test_script);
        combined = (char *)malloc((size_t)plen + base_len + 1);
        if (!combined) {
            printf("[servo] malloc failed\n");
            return;
        }
        memcpy(combined, prefix, (size_t)plen);
        memcpy(combined + plen, s_servo_test_script, base_len + 1);
        script = combined;
    }

    run_script("servo_lua_task", script, 8192);

    free(combined);   /* NULL-safe */
}

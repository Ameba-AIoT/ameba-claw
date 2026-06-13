/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_rtc_test_provision.c — Writes RTC test script to VFS on boot.
**
** Script is written to VFS but NOT auto-run on boot.
** Trigger via AT command:
**   AT+CLAW=rtc,test    — run RTC init / set_time / get_time / alarm test
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

static const char s_rtc_test_script[] =
    "local rtc = require(\"rtc\")\n"
    "local sys = require(\"sys\")\n"
    "\n"
    "rtc.init()\n"
    "\n"
    "print(\"[RTC] timing accuracy test start\")\n"
    "rtc.set_time(2024, 1, 15, 10, 0, 0)\n"
    "sys.sleep_ms(200)\n"
    "\n"
    "local t0 = rtc.get_time()\n"
    "print(string.format(\"[RTC] T+0s  : %02d:%02d:%02d\", t0.hour, t0.min, t0.sec))\n"
    "\n"
    "print(\"[RTC] waiting 10s ...\")\n"
    "sys.sleep_ms(10000)\n"
    "\n"
    "local t1 = rtc.get_time()\n"
    "print(string.format(\"[RTC] T+10s : %02d:%02d:%02d\", t1.hour, t1.min, t1.sec))\n"
    "\n"
    "local ok1 = (t1.hour == 10 and t1.min == 0 and t1.sec >= 9 and t1.sec <= 12)\n"
    "if ok1 then\n"
    "    print(\"[RTC] T+10s pass\")\n"
    "else\n"
    "    print(string.format(\"[FAIL] T+10s expected 10:00:10+-2, got %02d:%02d:%02d\","
    " t1.hour, t1.min, t1.sec))\n"
    "end\n"
    "\n"
    "print(\"[RTC] waiting 50 more seconds (total ~60s) ...\")\n"
    "sys.sleep_ms(50000)\n"
    "\n"
    "local t2 = rtc.get_time()\n"
    "print(string.format(\"[RTC] T+60s : %02d:%02d:%02d\", t2.hour, t2.min, t2.sec))\n"
    "\n"
    "local ok2 = (t2.hour == 10 and t2.min == 1 and t2.sec >= 0 and t2.sec <= 3)\n"
    "if ok2 then\n"
    "    print(\"[RTC] T+60s pass\")\n"
    "else\n"
    "    print(string.format(\"[FAIL] T+60s expected ~10:01:00, got %02d:%02d:%02d\","
    " t2.hour, t2.min, t2.sec))\n"
    "end\n"
    "\n"
    "print(\"[RTC] alarm test start\")\n"
    "rtc.disable_alarm()\n"
    "rtc.set_time(2024, 1, 15, 10, 0, 0)\n"
    "sys.sleep_ms(200)\n"
    "rtc.set_alarm(10, 0, 5)\n"
    "sys.sleep_ms(6000)\n"
    "\n"
    "local ok3 = rtc.alarm_fired()\n"
    "if ok3 then\n"
    "    print(\"[RTC] alarm pass\")\n"
    "else\n"
    "    print(\"[FAIL] alarm did not fire after 6s\")\n"
    "end\n"
    "rtc.clear_alarm()\n"
    "rtc.disable_alarm()\n"
    "\n"
    "print(\"[RTC] minute rollover test\")\n"
    "rtc.set_time(2024, 1, 15, 10, 0, 55)\n"
    "sys.sleep_ms(200)\n"
    "sys.sleep_ms(6000)\n"
    "\n"
    "local t4 = rtc.get_time()\n"
    "print(string.format(\"[RTC] minute rollover: %02d:%02d:%02d\", t4.hour, t4.min, t4.sec))\n"
    "\n"
    "local ok4 = (t4.hour == 10 and t4.min == 1 and t4.sec >= 0 and t4.sec <= 3)\n"
    "if ok4 then\n"
    "    print(\"[RTC] minute rollover pass\")\n"
    "else\n"
    "    print(string.format(\"[FAIL] minute rollover expected ~10:01:01, got %02d:%02d:%02d\","
    " t4.hour, t4.min, t4.sec))\n"
    "end\n"
    "\n"
    "print(\"[RTC] hour rollover test\")\n"
    "rtc.set_time(2024, 1, 15, 10, 59, 55)\n"
    "sys.sleep_ms(200)\n"
    "sys.sleep_ms(6000)\n"
    "\n"
    "local t5 = rtc.get_time()\n"
    "print(string.format(\"[RTC] hour rollover: %02d:%02d:%02d\", t5.hour, t5.min, t5.sec))\n"
    "\n"
    "local ok5 = (t5.hour == 11 and t5.min == 0 and t5.sec >= 0 and t5.sec <= 3)\n"
    "if ok5 then\n"
    "    print(\"[RTC] hour rollover pass\")\n"
    "else\n"
    "    print(string.format(\"[FAIL] hour rollover expected ~11:00:01, got %02d:%02d:%02d\","
    " t5.hour, t5.min, t5.sec))\n"
    "end\n"
    "\n"
    "print(\"[RTC] wakeup test\")\n"
    "rtc.disable_wakeup()\n"
    "rtc.set_wakeup(3)\n"
    "sys.sleep_ms(4000)\n"
    "\n"
    "local ok6 = rtc.wakeup_fired()\n"
    "if ok6 then\n"
    "    print(\"[RTC] wakeup pass\")\n"
    "else\n"
    "    print(\"[FAIL] wakeup did not fire after 4s\")\n"
    "end\n"
    "rtc.clear_wakeup()\n"
    "rtc.disable_wakeup()\n"
    "\n"
    "print(\"[RTC] resource recycle test\")\n"
    "rtc.disable_alarm()\n"
    "rtc.disable_wakeup()\n"
    "rtc.init()\n"
    "rtc.set_time(2024, 6, 11, 8, 30, 0)\n"
    "sys.sleep_ms(200)\n"
    "\n"
    "local t7 = rtc.get_time()\n"
    "print(string.format(\"[RTC] recycle read-back: %04d-%02d-%02d %02d:%02d:%02d\","
    " t7.year, t7.mon, t7.mday, t7.hour, t7.min, t7.sec))\n"
    "\n"
    "rtc.set_alarm(8, 30, 30)\n"
    "rtc.disable_alarm()\n"
    "rtc.set_wakeup(10)\n"
    "rtc.disable_wakeup()\n"
    "\n"
    "local ok7 = (t7.year == 2024 and t7.mon == 6 and t7.mday == 11 and\n"
    "             t7.hour == 8 and t7.min == 30 and t7.sec >= 0 and t7.sec <= 2)\n"
    "if ok7 then\n"
    "    print(\"[RTC] resource recycle pass\")\n"
    "else\n"
    "    print(string.format(\"[FAIL] recycle expected 2024-06-11 08:30:00+-2, got"
    " %04d-%02d-%02d %02d:%02d:%02d\","
    " t7.year, t7.mon, t7.mday, t7.hour, t7.min, t7.sec))\n"
    "end\n"
    "\n"
    "if ok1 and ok2 and ok3 and ok4 and ok5 and ok6 and ok7 then\n"
    "    print(\"[RTC] all pass\")\n"
    "else\n"
    "    print(\"[RTC] fail\")\n"
    "end\n";

static void write_vfs(const char *path, const char *content)
{
    { FILE *_ck = fopen(path, "r"); if (_ck) { fclose(_ck); return; } }

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

void lua_driver_rtc_provision(void)
{
    /* Write script to VFS for manual use (REPL / AT command).
     * Do NOT write main.lua — RTC test is not auto-run on boot. */
    write_vfs("vfs:rtc_test.lua", s_rtc_test_script);
}

/* ── On-demand execution via AT+CLAW=rtc,<mode> ── */

typedef struct {
    const char       *script;
    SemaphoreHandle_t done;
} rtc_task_arg_t;

static void rtc_lua_task(void *param)
{
    rtc_task_arg_t *arg = (rtc_task_arg_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[rtc] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        if (luaL_loadstring(L, arg->script) != LUA_OK) {
            printf("[rtc] parse error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            printf("[rtc] runtime error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_close(L);
    }

    xSemaphoreGive(arg->done);
    rtos_task_delete(NULL);
}

void lua_rtc_run(const char *mode)
{
    const char *script = NULL;

    if (strcmp(mode, "test") == 0) {
        script = s_rtc_test_script;
    } else {
        printf("[rtc] unknown mode: %s (use test)\n", mode);
        return;
    }

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        printf("[rtc] semaphore create failed\n");
        return;
    }

    rtc_task_arg_t arg = { .script = script, .done = done };

    if (rtos_task_create(NULL, "rtc_lua_task", rtc_lua_task, &arg,
                         8192, 1) != RTK_SUCCESS) {
        printf("[rtc] task create failed\n");
        vSemaphoreDelete(done);
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}

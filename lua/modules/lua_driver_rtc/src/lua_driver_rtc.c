/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_driver_rtc.c — Lua RTC driver for Ameba RTOS (RTL8721F).
**
** Provides require("rtc"):
**   rtc.init()
**   rtc.set_time(year, mon, mday, hour, min, sec)
**   t = rtc.get_time()   → {year, mon, mday, hour, min, sec, yday}
**   rtc.set_alarm(hour, min, sec)
**   rtc.disable_alarm()
**   rtc.alarm_fired()    → bool
**   rtc.clear_alarm()
**   rtc.set_wakeup(seconds)
**   rtc.disable_wakeup()
**   rtc.wakeup_fired()   → bool
**   rtc.clear_wakeup()
**
** Uses fwlib raw API (ameba_rtc.h) directly — no HAL layer dependency.
**
** ── Concurrency & resources ────────────────────────────────────────────────
** The RTC is a single, global, shared peripheral with no per-handle resource:
** require("rtc") returns a flat function table, not an owned object.  rtc is
** loaded (luaL_requiref) into multiple Lua states — the REPL, the timer
** sandbox, and the skill sandbox — so several lua_run jobs plus timer callbacks
** can call into it concurrently.  Without a guard, one job running set_time()
** (or init(), which re-runs RTC_Init and resets prescalers) while another reads
** get_time() can interleave register writes and corrupt the calendar.
**
** Fix (mirrors lua_driver_i2c):
**   1. One process-wide rtos_mutex; every Lua API holds it for the whole
**      hardware sequence.  Created once in lua_driver_rtc_init() during the
**      single-threaded boot phase, before any concurrent Lua execution.
**   2. init() is idempotent: the clock is (re-)enabled every call (a no-op if
**      already on) but RTC_Init() runs only once, so a late init() can never
**      reset registers underneath an in-flight set_time/alarm in another job.
**   3. There is no hardware deinit — the RTC clock stays on for the lifetime of
**      the boot.  "Release" means disable_alarm()/disable_wakeup(), which stop
**      the corresponding timer and clear its flag; the time registers persist.
*/

#define LUA_LIB
#include "lua.h"
#include "lauxlib.h"

#include "lua_driver_rtc.h"
#include "ameba_soc.h"

/* ── Concurrency guard (see header comment) ──────────────────────────────────
 * Created in lua_driver_rtc_init() at boot; NULL-safe so a missing init never
 * faults (it just degrades to no locking).  Validate Lua args and ranges BEFORE
 * taking the lock — luaL_error()/luaL_checkinteger() longjmp out and would
 * otherwise leak the mutex; no code path raises between LOCK and UNLOCK. */
static rtos_mutex_t s_rtc_lock;
static int          s_rtc_inited; /* 1 after the first RTC_Init() */

#define RTC_LOCK()    do { if (s_rtc_lock) { rtos_mutex_take(s_rtc_lock, 0xFFFFFFFFUL); } } while (0)
#define RTC_UNLOCK()  do { if (s_rtc_lock) { rtos_mutex_give(s_rtc_lock); } } while (0)

/* Days per month: index 0=Jan … 11=Dec; 0 = February (computed from year). */
static const uint8_t s_dim[12] = {31, 0, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static uint8_t rtc_days_in_month(int mon0, int year)
{
    uint8_t d = s_dim[mon0 % 12];
    if (d == 0) {
        d = (((!(year % 4)) && (year % 100)) || (!(year % 400))) ? 29 : 28;
    }
    return d;
}

/* Returns 0-based day-of-year from 1-based mon and 1-based mday. */
static uint16_t rtc_yday(int year, int mon, int mday)
{
    uint16_t yday = 0;
    for (int m = 1; m < mon; m++) {
        yday += rtc_days_in_month(m - 1, year);
    }
    yday += (uint16_t)(mday - 1);
    return yday;
}

/* Converts 0-based yday to 1-based mon and 1-based mday. */
static void rtc_mday(int year, int yday, int *mon, int *mday)
{
    int rem = yday + 1;
    int m   = 0;
    while (rem > 0) {
        int d = rtc_days_in_month(m, year);
        if (rem <= d) {
            break;
        }
        rem -= d;
        m++;
    }
    *mon  = m + 1;
    *mday = rem;
}

/* ── Lua API ──────────────────────────────────────────────────────────────── */

static int lrtc_init(lua_State *L)
{
    (void)L;
    RTC_LOCK();
    /* Clock enable is idempotent; RTC_Init() resets prescalers/registers so it
     * runs only once — a later init() must not disturb an in-flight set_time
     * in another concurrent job. */
    RCC_PeriphClockCmd(NULL, APBPeriph_RTC_CLOCK, ENABLE);
    RTC_Enable(ENABLE);

    if (!s_rtc_inited) {
        RTC_InitTypeDef cfg;
        RTC_StructInit(&cfg);
        cfg.RTC_HourFormat = RTC_HourFormat_24;
        RTC_Init(&cfg);
        s_rtc_inited = 1;
    }
    RTC_UNLOCK();
    return 0;
}

static int lrtc_set_time(lua_State *L)
{
    int year  = (int)luaL_checkinteger(L, 1);
    int mon   = (int)luaL_checkinteger(L, 2);
    int mday  = (int)luaL_checkinteger(L, 3);
    int hour  = (int)luaL_checkinteger(L, 4);
    int min   = (int)luaL_checkinteger(L, 5);
    int sec   = (int)luaL_checkinteger(L, 6);

    if (year < 1900 || year > 2155) {
        return luaL_error(L, "rtc: year must be 1900-2155");
    }
    if (mon < 1 || mon > 12) {
        return luaL_error(L, "rtc: mon must be 1-12");
    }
    if (mday < 1 || mday > 31) {
        return luaL_error(L, "rtc: mday must be 1-31");
    }
    if (hour < 0 || hour > 23) {
        return luaL_error(L, "rtc: hour must be 0-23");
    }
    if (min < 0 || min > 59) {
        return luaL_error(L, "rtc: min must be 0-59");
    }
    if (sec < 0 || sec > 59) {
        return luaL_error(L, "rtc: sec must be 0-59");
    }

    RTC_TimeTypeDef t;
    RTC_TimeStructInit(&t);
    t.RTC_Year     = (u16)year;
    t.RTC_Days     = rtc_yday(year, mon, mday);
    t.RTC_Hours    = (u8)hour;
    t.RTC_Minutes  = (u8)min;
    t.RTC_Seconds  = (u8)sec;
    t.RTC_H12_PMAM = RTC_H12_AM;

    RTC_LOCK();
    RTC_SetTime(RTC_Format_BIN, &t);
    RTC_UNLOCK();
    return 0;
}

static int lrtc_get_time(lua_State *L)
{
    RTC_TimeTypeDef t;
    RTC_LOCK();
    RTC_GetTime(RTC_Format_BIN, &t);
    RTC_UNLOCK();

    int mon  = 1;
    int mday = 1;
    rtc_mday((int)t.RTC_Year, (int)t.RTC_Days, &mon, &mday);

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)t.RTC_Year);
    lua_setfield(L, -2, "year");
    lua_pushinteger(L, (lua_Integer)mon);
    lua_setfield(L, -2, "mon");
    lua_pushinteger(L, (lua_Integer)mday);
    lua_setfield(L, -2, "mday");
    lua_pushinteger(L, (lua_Integer)t.RTC_Hours);
    lua_setfield(L, -2, "hour");
    lua_pushinteger(L, (lua_Integer)t.RTC_Minutes);
    lua_setfield(L, -2, "min");
    lua_pushinteger(L, (lua_Integer)t.RTC_Seconds);
    lua_setfield(L, -2, "sec");
    lua_pushinteger(L, (lua_Integer)t.RTC_Days);
    lua_setfield(L, -2, "yday");
    return 1;
}

static int lrtc_set_alarm(lua_State *L)
{
    int hour = (int)luaL_checkinteger(L, 1);
    int min  = (int)luaL_checkinteger(L, 2);
    int sec  = (int)luaL_checkinteger(L, 3);

    if (hour < 0 || hour > 23) {
        return luaL_error(L, "rtc: alarm hour must be 0-23");
    }
    if (min < 0 || min > 59) {
        return luaL_error(L, "rtc: alarm min must be 0-59");
    }
    if (sec < 0 || sec > 59) {
        return luaL_error(L, "rtc: alarm sec must be 0-59");
    }

    RTC_AlarmTypeDef alm;
    RTC_AlarmStructInit(&alm);
    alm.RTC_AlarmTime.RTC_H12_PMAM = RTC_H12_AM;
    alm.RTC_AlarmTime.RTC_Hours    = (u8)hour;
    alm.RTC_AlarmTime.RTC_Minutes  = (u8)min;
    alm.RTC_AlarmTime.RTC_Seconds  = (u8)sec;
    alm.RTC_AlarmMask              = RTC_AlarmMask_None;  /* match H:M:S */
    alm.RTC_Alarm2Mask             = RTC_Alarm2Mask_Days; /* day: don't care */

    RTC_LOCK();
    RTC_SetAlarm(RTC_Format_BIN, &alm);
    RTC_AlarmCmd(ENABLE);
    RTC_UNLOCK();
    return 0;
}

static int lrtc_disable_alarm(lua_State *L)
{
    (void)L;
    RTC_LOCK();
    RTC_AlarmCmd(DISABLE);
    RTC_AlarmClear();
    RTC_UNLOCK();
    return 0;
}

static int lrtc_alarm_fired(lua_State *L)
{
    RTC_TypeDef *rtc = RTC_DEV;
    RTC_LOCK();
    u32 isr = rtc->RTC_ISR;
    RTC_UNLOCK();
    lua_pushboolean(L, (isr & RTC_BIT_ALMF) ? 1 : 0);
    return 1;
}

static int lrtc_clear_alarm(lua_State *L)
{
    (void)L;
    RTC_LOCK();
    RTC_AlarmClear();
    RTC_UNLOCK();
    return 0;
}

static int lrtc_set_wakeup(lua_State *L)
{
    lua_Integer seconds = luaL_checkinteger(L, 1);

    if (seconds < 1 || seconds > 131072) {
        return luaL_error(L, "rtc: wakeup seconds must be 1-131072");
    }

    /* WUTR value n fires every (n+1) clock cycles at 1 Hz */
    RTC_LOCK();
    RTC_SetWakeup((u32)(seconds - 1));
    RTC_WakeupCmd(ENABLE);
    RTC_UNLOCK();
    return 0;
}

static int lrtc_disable_wakeup(lua_State *L)
{
    (void)L;
    RTC_LOCK();
    RTC_WakeupCmd(DISABLE);
    RTC_WakeupClear();
    RTC_UNLOCK();
    return 0;
}

static int lrtc_wakeup_fired(lua_State *L)
{
    RTC_TypeDef *rtc = RTC_DEV;
    RTC_LOCK();
    u32 isr = rtc->RTC_ISR;
    RTC_UNLOCK();
    lua_pushboolean(L, (isr & RTC_BIT_WUTF) ? 1 : 0);
    return 1;
}

static int lrtc_clear_wakeup(lua_State *L)
{
    (void)L;
    RTC_LOCK();
    RTC_WakeupClear();
    RTC_UNLOCK();
    return 0;
}

static const luaL_Reg lrtc_funcs[] = {
    {"init",           lrtc_init},
    {"set_time",       lrtc_set_time},
    {"get_time",       lrtc_get_time},
    {"set_alarm",      lrtc_set_alarm},
    {"disable_alarm",  lrtc_disable_alarm},
    {"alarm_fired",    lrtc_alarm_fired},
    {"clear_alarm",    lrtc_clear_alarm},
    {"set_wakeup",     lrtc_set_wakeup},
    {"disable_wakeup", lrtc_disable_wakeup},
    {"wakeup_fired",   lrtc_wakeup_fired},
    {"clear_wakeup",   lrtc_clear_wakeup},
    {NULL, NULL}
};

/* ── Driver-level init (called once from lua_module_registry_provision_all,
 *      single-threaded boot phase, before any concurrent Lua execution). ──── */
void lua_driver_rtc_init(void)
{
    if (s_rtc_lock == NULL) {
        rtos_mutex_create(&s_rtc_lock);
    }
    s_rtc_inited = 0;
}

LUAMOD_API int luaopen_rtc(lua_State *L)
{
    luaL_newlib(L, lrtc_funcs);
    return 1;
}

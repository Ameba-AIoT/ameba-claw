/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 * SPDX-License-Identifier: Apache-2.0
 *
 * luswtimer.c — Generic software timer with Lua string callbacks.
 *
 * Provides require("timer"):
 *   id = timer.start(interval_ms, lua_code_string, repeat)
 *   timer.stop(id)
 *   timer.list() -> table
 *
 * Security:
 *   - callback Lua state applies the same kill-list as the skill sandbox
 *     (load/dofile/rawget/rawset/rawequal/rawlen stripped from base)
 *   - code strings are capped at SWTIMER_CODE_MAX bytes
 *   - UAF-safe: task holds a strdup copy of the code before releasing the lock
 *
 * Thread safety:
 *   - s_lock protects the timer table
 *   - swtimer_task is the only writer to s_L; no concurrent Lua execution
 *   - init is eager (luaopen_timer), not lazy, so no double-init race
 */

#define LUA_LIB
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lua_module_registry.h"

#include "ameba_soc.h"
#include "os_wrapper.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define SWTIMER_MAX       8
#define SWTIMER_CODE_MAX  8192   /* max bytes per code string */
#define TICK_MS          10
#define TAG              "luswtimer"

typedef struct {
    int      id;
    uint32_t interval_ms;
    uint32_t remaining_ms;
    int      repeat;
    int      active;
    char    *code;
} sw_timer_t;

static sw_timer_t   s_timers[SWTIMER_MAX];
static int          s_next_id = 1;
static rtos_mutex_t s_lock;
static rtos_task_t  s_task    = NULL;
static lua_State   *s_L       = NULL;

/* ---- Timer callback deadline hook ----
 *
 * Installed on s_L so a runaway callback (e.g. "while true do end") cannot
 * hang swtimer_task indefinitely.  Before each lua_pcall the task sets
 * __deadline_ms in the registry to (now + TIMER_CALLBACK_MAX_MS); the hook
 * raises an error when that wall-clock deadline is exceeded.  The hook is
 * cleared (deadline=0) after every call so it does not trip between callbacks.
 */
#define TIMER_CALLBACK_MAX_MS  5000

static void timer_deadline_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    lua_getfield(L, LUA_REGISTRYINDEX, "__deadline_ms");
    lua_Integer dl = lua_tointeger(L, -1);
    lua_pop(L, 1);
    if (dl != 0 &&
        (lua_Integer)rtos_time_get_current_system_time_ms() >= dl) {
        luaL_error(L, "timer callback timed out (> %dms)", TIMER_CALLBACK_MAX_MS);
    }
}

/* ---- Callback Lua state (initialized once at module load) ---- */

static lua_State *cb_state_init(void)
{
    lua_State *L = luaL_newstate();
    if (!L) return NULL;

    /* Install standard safe libs (same subset as the skill sandbox). */
    luaL_requiref(L, "_G",     luaopen_base,   1); lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, "math",   luaopen_math,   1); lua_pop(L, 1);
    luaL_requiref(L, "table",  luaopen_table,  1); lua_pop(L, 1);

    /* Install timer-safe Ameba modules via the registry (TIMER flag = lightweight
     * subset: gpio/i2c/rtc/sys/cjson/cap/file).  Heavy or blocking modules
     * (audio/udp/wifi/usb) are excluded to keep cb_state_init() stack-safe. */
    lua_module_registry_install(L, LUA_MOD_ENV_TIMER);

    /* Apply the same kill-list as the skill sandbox so timer.start() cannot
     * be used to bypass sandbox restrictions via dynamic code loading. */
    static const char *s_kill[] = {
        "load", "loadfile", "dofile",
        "rawget", "rawset", "rawequal", "rawlen",
        NULL
    };
    for (int i = 0; s_kill[i]; i++) {
        lua_pushnil(L);
        lua_setglobal(L, s_kill[i]);
    }

    /* Install deadline hook so runaway callbacks cannot hang swtimer_task.
     * __deadline_ms is set to 0 here; swtimer_task sets it before each call. */
    lua_pushinteger(L, 0);
    lua_setfield(L, LUA_REGISTRYINDEX, "__deadline_ms");
    lua_sethook(L, timer_deadline_hook, LUA_MASKCOUNT, 500);

    return L;
}

/* ---- Timer task ---- */

static void swtimer_task(void *arg)
{
    (void)arg;

    while (1) {
        rtos_time_delay_ms(TICK_MS);

        rtos_mutex_take(s_lock, 0xFFFFFFFFUL);
        for (int i = 0; i < SWTIMER_MAX; i++) {
            sw_timer_t *t = &s_timers[i];
            if (!t->active || !t->code) continue;

            if (t->remaining_ms > TICK_MS) {
                t->remaining_ms -= TICK_MS;
                /* Note: if a callback takes longer than TICK_MS the
                 * next fire is delayed accordingly — timing is approximate. */
                continue;
            }

            /* Timer fired — take ownership of code string before releasing lock.
             * This prevents UAF if timer.stop() frees t->code concurrently. */
            char *code = strdup(t->code);

            if (t->repeat) {
                t->remaining_ms = t->interval_ms;
            } else {
                t->active = 0;
                free(t->code);
                t->code = NULL;
            }

            rtos_mutex_give(s_lock);

            if (s_L && code) {
                /* Set wall-clock deadline before executing the callback so the
                 * timer_deadline_hook can abort a runaway loop. */
                lua_pushinteger(s_L,
                    (lua_Integer)(rtos_time_get_current_system_time_ms()
                                  + TIMER_CALLBACK_MAX_MS));
                lua_setfield(s_L, LUA_REGISTRYINDEX, "__deadline_ms");

                if (luaL_loadstring(s_L, code) == LUA_OK) {
                    if (lua_pcall(s_L, 0, 0, 0) != LUA_OK) {
                        RTK_LOGW(TAG, "timer cb error: %s\n",
                                 lua_tostring(s_L, -1));
                        lua_pop(s_L, 1);
                    }
                } else {
                    RTK_LOGW(TAG, "timer cb load error: %s\n",
                             lua_tostring(s_L, -1));
                    lua_pop(s_L, 1);
                }

                /* Clear deadline so hook does not fire between callbacks. */
                lua_pushinteger(s_L, 0);
                lua_setfield(s_L, LUA_REGISTRYINDEX, "__deadline_ms");

                lua_settop(s_L, 0);
            }
            free(code);

            rtos_mutex_take(s_lock, 0xFFFFFFFFUL);
        }
        rtos_mutex_give(s_lock);
    }
}

/* ---- Lua API ---- */

/* timer.start(interval_ms, lua_code, repeat) -> id */
static int lswtimer_start(lua_State *L)
{
    uint32_t    ms   = (uint32_t)luaL_checkinteger(L, 1);
    const char *code = luaL_checkstring(L, 2);
    int         rep  = lua_toboolean(L, 3);

    if (ms < TICK_MS) ms = TICK_MS;

    if (strlen(code) > SWTIMER_CODE_MAX)
        return luaL_error(L, "timer.start: code too large (max %d bytes)",
                          SWTIMER_CODE_MAX);

    rtos_mutex_take(s_lock, 0xFFFFFFFFUL);
    int idx = -1;
    for (int i = 0; i < SWTIMER_MAX; i++) {
        if (!s_timers[i].active) { idx = i; break; }
    }
    if (idx < 0) {
        rtos_mutex_give(s_lock);
        return luaL_error(L, "timer.start: no free slots (max %d)", SWTIMER_MAX);
    }

    char *code_copy = strdup(code);
    if (!code_copy) {
        rtos_mutex_give(s_lock);
        return luaL_error(L, "timer.start: out of memory");
    }

    sw_timer_t *t   = &s_timers[idx];
    t->id           = s_next_id++;
    t->interval_ms  = ms;
    t->remaining_ms = ms;
    t->repeat       = rep;
    t->active       = 1;
    t->code         = code_copy;
    int ret_id      = t->id;
    rtos_mutex_give(s_lock);

    lua_pushinteger(L, ret_id);
    return 1;
}

/* timer.stop(id) */
static int lswtimer_stop(lua_State *L)
{
    int id = (int)luaL_checkinteger(L, 1);

    rtos_mutex_take(s_lock, 0xFFFFFFFFUL);
    for (int i = 0; i < SWTIMER_MAX; i++) {
        if (s_timers[i].active && s_timers[i].id == id) {
            s_timers[i].active = 0;
            free(s_timers[i].code);
            s_timers[i].code = NULL;
            rtos_mutex_give(s_lock);
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    rtos_mutex_give(s_lock);
    lua_pushboolean(L, 0);
    return 1;
}

/* timer.list() -> array of {id, interval_ms, repeat, remaining_ms} */
static int lswtimer_list(lua_State *L)
{
    lua_newtable(L);
    int n = 0;
    rtos_mutex_take(s_lock, 0xFFFFFFFFUL);
    for (int i = 0; i < SWTIMER_MAX; i++) {
        if (!s_timers[i].active) continue;
        n++;
        lua_newtable(L);
        lua_pushinteger(L, s_timers[i].id);           lua_setfield(L, -2, "id");
        lua_pushinteger(L, s_timers[i].interval_ms);  lua_setfield(L, -2, "interval_ms");
        lua_pushinteger(L, s_timers[i].remaining_ms); lua_setfield(L, -2, "remaining_ms");
        lua_pushboolean(L, s_timers[i].repeat);       lua_setfield(L, -2, "repeat");
        lua_rawseti(L, -2, n);
    }
    rtos_mutex_give(s_lock);
    return 1;
}

/* Stop all active timers — callable from C (e.g. AT+CLAW=i2c,sh1106). */
void swtimer_stop_all(void)
{
    if (!s_lock) return;
    rtos_mutex_take(s_lock, 0xFFFFFFFFUL);
    for (int i = 0; i < SWTIMER_MAX; i++) {
        if (s_timers[i].active) {
            s_timers[i].active = 0;
            free(s_timers[i].code);
            s_timers[i].code = NULL;
        }
    }
    rtos_mutex_give(s_lock);
}

static const luaL_Reg lswtimer_funcs[] = {
    {"start", lswtimer_start},
    {"stop",  lswtimer_stop},
    {"list",  lswtimer_list},
    {NULL, NULL}
};

LUAMOD_API int luaopen_timer(lua_State *L)
{
    /* Eager init: mutex and task created once when module is first loaded.
     * Avoids the double-init race that lazy (on-demand) init has. */
    if (!s_task) {
        if (rtos_mutex_create(&s_lock) == RTK_SUCCESS) {
            s_L = cb_state_init();
            rtos_task_create(&s_task, "swtimer", swtimer_task, NULL, 16384, 3);
        }
    }

    luaL_newlib(L, lswtimer_funcs);
    return 1;
}

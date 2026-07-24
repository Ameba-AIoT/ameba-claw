/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_driver_thermal.h"

#include "ameba_soc.h"
#include "lauxlib.h"
#include "os_wrapper.h"

#define LUA_DRIVER_THERMAL_METATABLE "thermal.handle"

typedef struct {
    int closed;
} lua_driver_thermal_ud_t;

static rtos_mutex_t s_thermal_lock;
static int          s_thermal_refcount;
static int          s_thermal_initialized;

static lua_driver_thermal_ud_t *lua_driver_thermal_get_ud(lua_State *L, int idx)
{
    lua_driver_thermal_ud_t *ud = (lua_driver_thermal_ud_t *)luaL_checkudata(
        L, idx, LUA_DRIVER_THERMAL_METATABLE);
    if (!ud || ud->closed) {
        luaL_error(L, "thermal: invalid or closed handle");
    }
    return ud;
}

static int lua_driver_thermal_new(lua_State *L)
{
    rtos_mutex_take(s_thermal_lock, MUTEX_WAIT_TIMEOUT);

    if (!s_thermal_initialized) {
        RCC_PeriphClockCmd(APBPeriph_THM, APBPeriph_THM_CLOCK, ENABLE);
        TM_InitTypeDef tm;
        TM_StructInit(&tm);
        TM_Init(&tm);
        s_thermal_initialized = 1;
    }
    s_thermal_refcount++;

    rtos_mutex_give(s_thermal_lock);

    lua_driver_thermal_ud_t *ud = (lua_driver_thermal_ud_t *)lua_newuserdata(
        L, sizeof(*ud));
    ud->closed = 0;
    luaL_getmetatable(L, LUA_DRIVER_THERMAL_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

static int lua_driver_thermal_read(lua_State *L)
{
    lua_driver_thermal_get_ud(L, 1);
    u32 raw = TM_GetTempResult();
    if (raw == TM_INVALID_VALUE) {
        return luaL_error(L, "thermal: read timeout");
    }
    lua_pushnumber(L, (lua_Number)TM_GetCdegree(raw));
    return 1;
}

static int lua_driver_thermal_max_temp(lua_State *L)
{
    lua_driver_thermal_get_ud(L, 1);
    u32 raw = TM_GetMaxTemp();
    if (raw == TM_INVALID_VALUE) {
        return luaL_error(L, "thermal: read timeout");
    }
    lua_pushnumber(L, (lua_Number)TM_GetCdegree(raw));
    return 1;
}

static int lua_driver_thermal_min_temp(lua_State *L)
{
    lua_driver_thermal_get_ud(L, 1);
    u32 raw = TM_GetMinTemp();
    if (raw == TM_INVALID_VALUE) {
        return luaL_error(L, "thermal: read timeout");
    }
    lua_pushnumber(L, (lua_Number)TM_GetCdegree(raw));
    return 1;
}

static int lua_driver_thermal_power_on_temp(lua_State *L)
{
    lua_driver_thermal_get_ud(L, 1);
    u32 raw = TM_GetPowOnTemp();
    if (raw == TM_INVALID_VALUE) {
        return luaL_error(L, "thermal: read timeout");
    }
    lua_pushnumber(L, (lua_Number)TM_GetCdegree(raw));
    return 1;
}

static int lua_driver_thermal_clear_max(lua_State *L)
{
    lua_driver_thermal_get_ud(L, 1);
    TM_MaxTempClr();
    return 0;
}

static int lua_driver_thermal_clear_min(lua_State *L)
{
    lua_driver_thermal_get_ud(L, 1);
    TM_MinTempClr();
    return 0;
}

/* read_f() → current temperature in Fahrenheit */
static int lua_driver_thermal_read_f(lua_State *L)
{
    lua_driver_thermal_get_ud(L, 1);
    u32 raw = TM_GetTempResult();
    if (raw == TM_INVALID_VALUE) {
        return luaL_error(L, "thermal: read timeout");
    }
    lua_pushnumber(L, (lua_Number)TM_GetFdegree(raw));
    return 1;
}

static void lua_driver_thermal_do_close(lua_driver_thermal_ud_t *ud)
{
    if (ud->closed) {
        return;
    }
    ud->closed = 1;

    rtos_mutex_take(s_thermal_lock, MUTEX_WAIT_TIMEOUT);
    if (s_thermal_refcount > 0) {
        s_thermal_refcount--;
    }
    if (s_thermal_refcount == 0 && s_thermal_initialized) {
        TM_Cmd(DISABLE);
        RCC_PeriphClockCmd(APBPeriph_THM, APBPeriph_THM_CLOCK, DISABLE);
        s_thermal_initialized = 0;
    }
    rtos_mutex_give(s_thermal_lock);
}

static int lua_driver_thermal_close(lua_State *L)
{
    lua_driver_thermal_ud_t *ud = (lua_driver_thermal_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_THERMAL_METATABLE);
    lua_driver_thermal_do_close(ud);
    return 0;
}

static int lua_driver_thermal_gc(lua_State *L)
{
    lua_driver_thermal_ud_t *ud = (lua_driver_thermal_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_THERMAL_METATABLE);
    if (ud) {
        lua_driver_thermal_do_close(ud);
    }
    return 0;
}

LUAMOD_API int luaopen_thermal(lua_State *L)
{
    if (!s_thermal_lock) {
        rtos_mutex_create(&s_thermal_lock);
    }

    if (luaL_newmetatable(L, LUA_DRIVER_THERMAL_METATABLE)) {
        lua_pushcfunction(L, lua_driver_thermal_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_driver_thermal_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_driver_thermal_read_f);
        lua_setfield(L, -2, "read_f");
        lua_pushcfunction(L, lua_driver_thermal_max_temp);
        lua_setfield(L, -2, "max_temp");
        lua_pushcfunction(L, lua_driver_thermal_min_temp);
        lua_setfield(L, -2, "min_temp");
        lua_pushcfunction(L, lua_driver_thermal_power_on_temp);
        lua_setfield(L, -2, "power_on_temp");
        lua_pushcfunction(L, lua_driver_thermal_clear_max);
        lua_setfield(L, -2, "clear_max");
        lua_pushcfunction(L, lua_driver_thermal_clear_min);
        lua_setfield(L, -2, "clear_min");
        lua_pushcfunction(L, lua_driver_thermal_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_driver_thermal_new);
    lua_setfield(L, -2, "new");
    return 1;
}

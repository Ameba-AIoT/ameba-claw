/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_module_light_sensor.c — LM393 + LDR light sensor Lua driver for
** Ameba RTOS (RTL8721F / AmebaGreen2).
**
** Provides require("light_sensor"):
**   light_sensor.new({do_pin=<pin>}) -> handle   -- pin from board.json or caller
**
** Handle methods:
**   handle:read()       -> 0 (bright) or 1 (dark)
**   handle:is_bright()  -> bool  (DO == 0)
**   handle:is_dark()    -> bool  (DO == 1)
**   handle:name()       -> "lm393_ldr"
**   handle:close()
**
** ── Hardware notes ───────────────────────────────────────────────────────────
** The LM393 comparator drives DO LOW when light intensity exceeds the
** potentiometer threshold, and HIGH when below it:
**   DO = 0 → bright (light above threshold)
**   DO = 1 → dark   (light below threshold)
**
** This is a purely digital interface — no ADC required.  The module has no
** AO (analog output) pin on the 3-pin variant shown.  If a 4-pin variant
** with AO is used, require("adc") on an ADC-capable pin (PA12-PA20) can read
** the raw voltage.
**
** ── Concurrency ──────────────────────────────────────────────────────────────
** A module-level mutex serialises all read() calls.  The GPIO clock stays on
** after first use (RCC_PeriphClockCmd is idempotent).  close() marks the
** handle invalid; no persistent hardware resources are held between reads.
*/

#define LUA_LIB
#include "lua.h"
#include "lauxlib.h"

#include "lua_module_light_sensor.h"
#include "luhw.h"
#include "ameba_soc.h"
#include "os_wrapper.h"
#include <string.h>

#define LIGHT_SENSOR_METATABLE  "light_sensor.handle"

typedef struct {
    PinName do_pin;
    int     closed;
} light_sensor_ud_t;

static rtos_mutex_t s_light_lock;

/* ── GPIO helpers ──────────────────────────────────────────────────────────── */

static void light_sensor_gpio_init(u32 pin)
{
    RCC_PeriphClockCmd(APBPeriph_GPIO, APBPeriph_GPIO_CLOCK, ENABLE);
    GPIO_InitTypeDef cfg;
    cfg.GPIO_Pin  = pin;
    cfg.GPIO_Mode = GPIO_Mode_IN;
    cfg.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(&cfg);
}

/* ── Handle helper ───────────────────────────────────────────────────────────  */

static light_sensor_ud_t *light_sensor_get_ud(lua_State *L, int idx)
{
    light_sensor_ud_t *ud =
        (light_sensor_ud_t *)luaL_checkudata(L, idx, LIGHT_SENSOR_METATABLE);
    if (!ud || ud->closed) {
        luaL_error(L, "light_sensor: invalid or closed handle");
    }
    return ud;
}

/* ── Lua API ─────────────────────────────────────────────────────────────────  */

static int light_sensor_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "do_pin");
    PinName do_pin = luhw_check_pin(L, -1);
    lua_pop(L, 1);

    light_sensor_gpio_init((u32)do_pin);

    light_sensor_ud_t *ud =
        (light_sensor_ud_t *)lua_newuserdata(L, sizeof(*ud));
    ud->do_pin = do_pin;
    ud->closed = 0;

    luaL_getmetatable(L, LIGHT_SENSOR_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

static int light_sensor_read(lua_State *L)
{
    light_sensor_ud_t *ud = light_sensor_get_ud(L, 1);

    rtos_mutex_take(s_light_lock, MUTEX_WAIT_TIMEOUT);
    int level = (int)GPIO_ReadDataBit((u32)ud->do_pin);
    rtos_mutex_give(s_light_lock);

    lua_pushinteger(L, (lua_Integer)level);
    return 1;
}

static int light_sensor_is_bright(lua_State *L)
{
    light_sensor_ud_t *ud = light_sensor_get_ud(L, 1);

    rtos_mutex_take(s_light_lock, MUTEX_WAIT_TIMEOUT);
    int level = (int)GPIO_ReadDataBit((u32)ud->do_pin);
    rtos_mutex_give(s_light_lock);

    lua_pushboolean(L, level == 0 ? 1 : 0);
    return 1;
}

static int light_sensor_is_dark(lua_State *L)
{
    light_sensor_ud_t *ud = light_sensor_get_ud(L, 1);

    rtos_mutex_take(s_light_lock, MUTEX_WAIT_TIMEOUT);
    int level = (int)GPIO_ReadDataBit((u32)ud->do_pin);
    rtos_mutex_give(s_light_lock);

    lua_pushboolean(L, level == 1 ? 1 : 0);
    return 1;
}

static int light_sensor_name(lua_State *L)
{
    light_sensor_get_ud(L, 1);
    lua_pushstring(L, "lm393_ldr");
    return 1;
}

static int light_sensor_close(lua_State *L)
{
    light_sensor_ud_t *ud =
        (light_sensor_ud_t *)luaL_checkudata(L, 1, LIGHT_SENSOR_METATABLE);
    ud->closed = 1;
    return 0;
}

static int light_sensor_gc(lua_State *L)
{
    light_sensor_ud_t *ud =
        (light_sensor_ud_t *)luaL_testudata(L, 1, LIGHT_SENSOR_METATABLE);
    if (ud) {
        ud->closed = 1;
    }
    return 0;
}

/* ── Module open ─────────────────────────────────────────────────────────────  */

LUAMOD_API int luaopen_light_sensor(lua_State *L)
{
    if (!s_light_lock) {
        rtos_mutex_create(&s_light_lock);
    }

    if (luaL_newmetatable(L, LIGHT_SENSOR_METATABLE)) {
        lua_pushcfunction(L, light_sensor_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, light_sensor_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, light_sensor_is_bright);
        lua_setfield(L, -2, "is_bright");
        lua_pushcfunction(L, light_sensor_is_dark);
        lua_setfield(L, -2, "is_dark");
        lua_pushcfunction(L, light_sensor_name);
        lua_setfield(L, -2, "name");
        lua_pushcfunction(L, light_sensor_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, light_sensor_new);
    lua_setfield(L, -2, "new");
    return 1;
}

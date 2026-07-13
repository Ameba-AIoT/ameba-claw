/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_module_environmental_sensor.c — Lua DHT11 1-wire temperature/humidity
** sensor driver for Ameba RTOS (RTL8721F / AmebaGreen2).
**
** Provides require("environmental_sensor"):
**   environmental_sensor.new({pin="PB_8"}) -> handle
**
** Handle methods:
**   handle:read()             -> {temperature=N, humidity=N}  (floats, C / %)
**   handle:read_temperature() -> number  (degrees Celsius)
**   handle:read_humidity()    -> number  (relative humidity %)
**   handle:read_raw()         -> temp_raw, humidity_raw  (integers x10)
**   handle:name()             -> "dht11"
**   handle:close()
**
** ── DHT11 1-wire protocol ──────────────────────────────────────────────────
**   1. Host: pull data line low >= 18 ms, then release to input (pull-up).
**   2. Sensor: ACK low ~80 us, then ACK high ~80 us.
**   3. Sensor: transmit 40 bits MSB-first. Each bit:
**        50 us low start pulse; bit=0: ~26-28 us high; bit=1: ~70 us high.
**   4. Payload: [RH_int, RH_dec, T_int, T_dec, checksum] (all 0..255).
**      RH_dec and T_dec are always 0 for DHT11.
**
** ── Concurrency & resources ────────────────────────────────────────────────
** A module-level mutex serializes all read() calls across Lua tasks.
** IRQs are disabled only for the timing-critical ~5 ms 40-bit read phase
** (bit discrimination needs ~1 us precision; any preemption would corrupt it).
** The mutex is always taken BEFORE disabling IRQs (mutex take may yield;
** IRQ-disable must not).  close() marks the handle invalid; no persistent
** hardware resources are held between reads.  The GPIO clock stays on after
** the first read (RCC_PeriphClockCmd is idempotent).
*/

#define LUA_LIB
#include "lua.h"
#include "lauxlib.h"

#include "lua_module_environmental_sensor.h"
#include "luhw.h"
#include "ameba_soc.h"
#include "os_wrapper.h"
#include <string.h>
#include <stdint.h>

/* ── Constants ────────────────────────────────────────────────────────────── */
#define ENV_SENSOR_METATABLE    "environmental_sensor.handle"

/* Timing (microseconds) */
#define DHT11_START_LOW_US      20000U  /* host start: pull low 20 ms */
#define DHT11_START_RELEASE_US  30U     /* host release before listening */
#define DHT11_ACK_TIMEOUT_US    300U    /* max wait for each ACK phase */
#define DHT11_BIT_TIMEOUT_US    200U    /* max wait per bit low/high */
#define DHT11_BIT_THRESHOLD_US  40U     /* midpoint: sample after 40 us; HIGH→bit=1, LOW→bit=0 */

/* ── Handle type ──────────────────────────────────────────────────────────── */
typedef struct {
    PinName pin;
    int     closed;
} env_sensor_ud_t;

/* ── Module-level serialisation lock ─────────────────────────────────────── */
static rtos_mutex_t s_env_lock;

/* ── Low-level 1-wire bit-bang ───────────────────────────────────────────── */

/*
** env_sensor_read_bytes() — execute the DHT11 1-wire read sequence.
** Must be called under s_env_lock.
** Returns  0  : success; out_temp and out_humi set (values x10).
**         -1  : timeout / no sensor response.
**         -2  : checksum mismatch.
*/
static int env_sensor_read_bytes(u32 pin, int16_t *out_temp, int16_t *out_humi)
{
    uint8_t data[5] = {0, 0, 0, 0, 0};

    /* ── Host start (interrupts ON — 20 ms is safe for the OS) ─────── */
    RCC_PeriphClockCmd(APBPeriph_GPIO, APBPeriph_GPIO_CLOCK, ENABLE);

    GPIO_InitTypeDef cfg;
    cfg.GPIO_Pin  = pin;
    cfg.GPIO_Mode = GPIO_Mode_OUT;
    cfg.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(&cfg);
    GPIO_WriteBit(pin, 0);
    DelayUs(DHT11_START_LOW_US);

    /* Release: switch to input with internal pull-up */
    cfg.GPIO_Mode = GPIO_Mode_IN;
    cfg.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(&cfg);
    DelayUs(DHT11_START_RELEASE_US);

    /* ── Timing-critical 40-bit read (IRQs disabled) ─────────────────── */
    u32 saved = __get_PRIMASK();
    __disable_irq();

    /* Sensor ACK low (~80 us) */
    u32 t = 0;
    while (GPIO_ReadDataBit(pin) != 0) {
        if (++t > DHT11_ACK_TIMEOUT_US) { __set_PRIMASK(saved); return -1; }
        DelayUs(1);
    }

    /* Sensor ACK high (~80 us) */
    t = 0;
    while (GPIO_ReadDataBit(pin) != 1) {
        if (++t > DHT11_ACK_TIMEOUT_US) { __set_PRIMASK(saved); return -1; }
        DelayUs(1);
    }

    /* ACK high ends; we are now at the start of bit 0's ~50 us LOW */
    t = 0;
    while (GPIO_ReadDataBit(pin) != 0) {
        if (++t > DHT11_ACK_TIMEOUT_US) { __set_PRIMASK(saved); return -1; }
        DelayUs(1);
    }

    /* 40 data bits, MSB first — midpoint sampling.
     * Each bit: ~50 us LOW start, then HIGH ~26 us (bit=0) or ~70 us (bit=1).
     * Strategy: wait for LOW (drain previous HIGH), then wait for HIGH, then
     * sample after DHT11_BIT_THRESHOLD_US (40 us).
     * At 40 us: bit-0 HIGH already ended → reads LOW=0; bit-1 still HIGH → reads 1.
     * This approach is robust against DelayUs() being slower than 1 us/call. */
    for (int i = 0; i < 40; i++) {
        /* Wait for LOW (drain any remaining HIGH from the previous bit) */
        t = 0;
        while (GPIO_ReadDataBit(pin) != 0) {
            if (++t > DHT11_BIT_TIMEOUT_US) { __set_PRIMASK(saved); return -1; }
            DelayUs(1);
        }

        /* Wait for HIGH (skip the ~50 us LOW start pulse) */
        t = 0;
        while (GPIO_ReadDataBit(pin) != 1) {
            if (++t > DHT11_BIT_TIMEOUT_US) { __set_PRIMASK(saved); return -1; }
            DelayUs(1);
        }

        /* Midpoint sample at 40 us */
        DelayUs(DHT11_BIT_THRESHOLD_US);
        data[i / 8] = (uint8_t)(data[i / 8] << 1);
        if (GPIO_ReadDataBit(pin) != 0) {
            data[i / 8] |= 1U;
        }
    }

    __set_PRIMASK(saved);

    /* ── Checksum ─────────────────────────────────────────────────────── */
    uint8_t sum = (uint8_t)((uint32_t)data[0] + data[1] + data[2] + data[3]);
    if (sum != data[4]) {
        return -2;
    }

    /* DHT11 raw: integer bytes only; multiply by 10 for unified x10 format */
    *out_humi = (int16_t)((int16_t)data[0] * 10);
    *out_temp = (int16_t)((int16_t)data[2] * 10);
    return 0;
}

/* ── Handle helper ───────────────────────────────────────────────────────── */

static env_sensor_ud_t *env_sensor_get_ud(lua_State *L, int idx)
{
    env_sensor_ud_t *ud =
        (env_sensor_ud_t *)luaL_checkudata(L, idx, ENV_SENSOR_METATABLE);
    if (!ud || ud->closed) {
        luaL_error(L, "environmental_sensor: invalid or closed handle");
    }
    return ud;
}

/* ── Internal: locked read ───────────────────────────────────────────────── */

static void env_sensor_do_read(lua_State *L, env_sensor_ud_t *ud,
                               int16_t *out_temp, int16_t *out_humi)
{
    if (rtos_mutex_take(s_env_lock, MUTEX_WAIT_TIMEOUT) != RTK_SUCCESS) {
        luaL_error(L, "environmental_sensor: failed to acquire lock");
    }
    int rc = env_sensor_read_bytes((u32)ud->pin, out_temp, out_humi);
    rtos_mutex_give(s_env_lock);

    if (rc == -1) {
        luaL_error(L, "environmental_sensor: no response (check wiring/pull-up)");
    } else if (rc == -2) {
        luaL_error(L, "environmental_sensor: checksum error");
    }
}

/* ── Lua API ──────────────────────────────────────────────────────────────── */

static int env_sensor_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "pin");
    PinName pin = luhw_check_pin(L, -1);
    lua_pop(L, 1);

    /* Enable GPIO clock and assert pull-up immediately so the data line is
     * not floating between new() and the first read(). */
    RCC_PeriphClockCmd(APBPeriph_GPIO, APBPeriph_GPIO_CLOCK, ENABLE);
    GPIO_InitTypeDef cfg;
    cfg.GPIO_Pin  = (u32)pin;
    cfg.GPIO_Mode = GPIO_Mode_IN;
    cfg.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(&cfg);

    env_sensor_ud_t *ud =
        (env_sensor_ud_t *)lua_newuserdata(L, sizeof(*ud));
    ud->pin    = pin;
    ud->closed = 0;

    luaL_getmetatable(L, ENV_SENSOR_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

static int env_sensor_read(lua_State *L)
{
    env_sensor_ud_t *ud = env_sensor_get_ud(L, 1);
    int16_t temp = 0, humi = 0;
    env_sensor_do_read(L, ud, &temp, &humi);

    lua_newtable(L);
    lua_pushnumber(L, (lua_Number)temp / 10.0);
    lua_setfield(L, -2, "temperature");
    lua_pushnumber(L, (lua_Number)humi / 10.0);
    lua_setfield(L, -2, "humidity");
    return 1;
}

static int env_sensor_read_temperature(lua_State *L)
{
    env_sensor_ud_t *ud = env_sensor_get_ud(L, 1);
    int16_t temp = 0, humi = 0;
    env_sensor_do_read(L, ud, &temp, &humi);
    lua_pushnumber(L, (lua_Number)temp / 10.0);
    return 1;
}

static int env_sensor_read_humidity(lua_State *L)
{
    env_sensor_ud_t *ud = env_sensor_get_ud(L, 1);
    int16_t temp = 0, humi = 0;
    env_sensor_do_read(L, ud, &temp, &humi);
    lua_pushnumber(L, (lua_Number)humi / 10.0);
    return 1;
}

static int env_sensor_read_raw(lua_State *L)
{
    env_sensor_ud_t *ud = env_sensor_get_ud(L, 1);
    int16_t temp = 0, humi = 0;
    env_sensor_do_read(L, ud, &temp, &humi);
    lua_pushinteger(L, (lua_Integer)temp);
    lua_pushinteger(L, (lua_Integer)humi);
    return 2;
}

static int env_sensor_name(lua_State *L)
{
    env_sensor_get_ud(L, 1);
    lua_pushstring(L, "dht11");
    return 1;
}

static int env_sensor_close(lua_State *L)
{
    env_sensor_ud_t *ud =
        (env_sensor_ud_t *)luaL_checkudata(L, 1, ENV_SENSOR_METATABLE);
    ud->closed = 1;
    return 0;
}

static int env_sensor_gc(lua_State *L)
{
    env_sensor_ud_t *ud =
        (env_sensor_ud_t *)luaL_testudata(L, 1, ENV_SENSOR_METATABLE);
    if (ud) { ud->closed = 1; }
    return 0;
}

/* ── Module open ──────────────────────────────────────────────────────────── */

LUAMOD_API int luaopen_environmental_sensor(lua_State *L)
{
    if (!s_env_lock) {
        rtos_mutex_create(&s_env_lock);
    }

    if (luaL_newmetatable(L, ENV_SENSOR_METATABLE)) {
        lua_pushcfunction(L, env_sensor_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, env_sensor_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, env_sensor_read_temperature);
        lua_setfield(L, -2, "read_temperature");
        lua_pushcfunction(L, env_sensor_read_humidity);
        lua_setfield(L, -2, "read_humidity");
        lua_pushcfunction(L, env_sensor_read_raw);
        lua_setfield(L, -2, "read_raw");
        lua_pushcfunction(L, env_sensor_name);
        lua_setfield(L, -2, "name");
        lua_pushcfunction(L, env_sensor_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, env_sensor_new);
    lua_setfield(L, -2, "new");
    return 1;
}

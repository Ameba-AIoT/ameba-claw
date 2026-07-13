/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_module_imu.c — Lua IMU driver for Ameba RTOS (RTL8721F / AmebaGreen2).
**
** Chip-agnostic front end: this file only parses the new() options, acquires
** the shared I2C controller and dispatches to a per-chip backend
** (src/backend/<chip>/…) — one Lua API, several chips.  Today the
** only backend is the InvenSense MPU-6050 (GY-521, WHO_AM_I 0x68); adding more
** IMUs means adding a backend file + one registry entry, nothing here changes.
**
** ── I2C sharing ─────────────────────────────────────────────────────────────
** All bus traffic goes through the shared lua_driver_i2c C bus API
** (lua_i2c_bus_acquire / _read_regs / _write_regs / _release).  That means the
** IMU and any Lua `i2c` code on the SAME controller take the SAME per-controller
** lock, so their transactions can never interleave and neither can stomp the
** other's config.  (The old version kept a private mutex on the same physical
** controller — two locks on one bus — which is exactly the conflict this fixes.)
**
** Provides require("imu"):
**   imu.new({sda="PA_26", scl="PA_25", i2c=0, addr=0x68,
**            freq=400000 [, chip="mpu6050"]}) -> handle
**
**   No pin is hard-coded here: sda/scl/i2c/addr/freq/chip all come from the
**   caller (populated from board.json by the test harness / skill).  i2c may be
**   0 or 1 (default 0); only field defaults are applied when a field is omitted.
**
** Handle methods:
**   handle:read()             -> {accel={x,y,z}, gyro={x,y,z}, temp=raw,
**                                 status=INT_STATUS}   (one synchronised burst)
**   handle:read_accel()       -> ax, ay, az           (raw signed 16-bit)
**   handle:read_gyro()        -> gx, gy, gz            (raw signed 16-bit)
**   handle:read_temperature() -> celsius              (float)
**   handle:read_int_status()  -> integer              (INT_STATUS bits)
**   handle:who_am_i()         -> integer              (0x68 on a healthy part)
**   handle:name()             -> backend name, e.g. "mpu6050"
**   handle:close()
**
** Scaling for the MPU-6050 (apply in Lua when physical units are wanted):
**   accel_g   = raw / 2048.0    (+/-16 g   => 2048 LSB/g)
**   gyro_dps  = raw / 16.4      (+/-2000 dps => 16.4 LSB/dps)
*/

#define LUA_LIB
#include "lua.h"
#include "lauxlib.h"

#include "lua_module_imu.h"
#include "backend/imu_backend.h"
#include "lua_driver_i2c.h"
#include "luhw.h"

#include <stdint.h>

/* ── Constants ────────────────────────────────────────────────────────────── */
/* Only truly chip-agnostic limits live here.  Per-chip defaults (address, bus
 * speed) come from the selected backend's vtable, and the i2c0 clock ceiling is
 * enforced once, in the lua_driver_i2c bus layer — not duplicated here. */
#define IMU_METATABLE      "imu.handle"
#define IMU_NUM_CTRL       2          /* I2C0 + I2C1 */
#define IMU_ADDR_MAX       0x7F       /* 7-bit address space */

/* ── Handle userdata ─────────────────────────────────────────────────────── */
typedef struct {
    imu_bus_t            bus;       /* controller index + slave address */
    const imu_backend_t *be;        /* selected chip backend (vtable) */
    int                  acquired;  /* 1 while holding a bus reference */
    int                  closed;
} imu_ud_t;

/* ── Handle helpers ──────────────────────────────────────────────────────── */

static imu_ud_t *imu_get_ud(lua_State *L, int idx)
{
    imu_ud_t *ud = (imu_ud_t *)luaL_checkudata(L, idx, IMU_METATABLE);
    if (!ud || ud->closed) {
        luaL_error(L, "imu: invalid or closed handle");
    }
    return ud;
}

/* Drop the shared bus reference exactly once (from close() and __gc). */
static void imu_release_bus(imu_ud_t *ud)
{
    if (ud->acquired) {
        lua_i2c_bus_release(ud->bus.i2c_idx);
        ud->acquired = 0;
    }
}

/* ── imu.new(opts) ───────────────────────────────────────────────────────── */

static int imu_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    /* ---- Parse parameters (no resources held; luaL_error is safe) -------- */
    lua_getfield(L, 1, "sda");
    if (lua_isnil(L, -1)) {
        return luaL_error(L, "imu.new: 'sda' is required");
    }
    PinName sda = luhw_check_pin(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "scl");
    if (lua_isnil(L, -1)) {
        return luaL_error(L, "imu.new: 'scl' is required");
    }
    PinName scl = luhw_check_pin(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "i2c");
    int idx = lua_isnil(L, -1) ? 0 : (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    if (idx < 0 || idx >= IMU_NUM_CTRL) {
        return luaL_error(L, "imu.new: 'i2c' must be 0 or 1, got %d", idx);
    }

    /* Resolve the backend first: its vtable supplies the per-chip address and
     * bus-speed defaults used when the caller omits 'addr' / 'freq'. */
    lua_getfield(L, 1, "chip");
    const char *chip = lua_isnil(L, -1) ? NULL : luaL_checkstring(L, -1);
    const imu_backend_t *be = imu_backend_find(chip);
    lua_pop(L, 1);
    if (be == NULL) {
        return luaL_error(L, "imu.new: unknown chip '%s'", chip ? chip : "");
    }

    lua_getfield(L, 1, "addr");
    lua_Integer addr = lua_isnil(L, -1) ? be->default_addr
                                        : luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    if (addr < 0 || addr > IMU_ADDR_MAX) {
        return luaL_error(L, "imu.new: 'addr' must be 0-%d", IMU_ADDR_MAX);
    }

    lua_getfield(L, 1, "freq");
    lua_Integer freq = lua_isnil(L, -1) ? be->default_freq
                                        : luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    if (freq <= 0) {
        return luaL_error(L, "imu.new: 'freq' must be > 0");
    }
    /* The i2c0 fast-mode ceiling is validated by lua_i2c_bus_acquire below;
     * no need to duplicate that controller limit in the front end. */

    /* ---- Allocate the handle BEFORE acquiring any resource --------------- */
    imu_ud_t *ud = (imu_ud_t *)lua_newuserdata(L, sizeof(*ud));
    ud->bus.i2c_idx = idx;
    ud->bus.addr    = (uint16_t)addr;
    ud->be          = be;
    ud->acquired    = 0;
    ud->closed      = 0;
    luaL_getmetatable(L, IMU_METATABLE);
    lua_setmetatable(L, -2);

    /* ---- Acquire the shared controller (refcounted, one lock per bus) ---- */
    int rc = lua_i2c_bus_acquire(idx, (uint8_t)sda, (uint8_t)scl, (uint32_t)freq);
    if (rc == LUA_I2C_ERR_CONFIG) {
        ud->closed = 1;
        return luaL_error(L,
            "imu.new: i2c%d already open with different pins/frequency", idx);
    }
    if (rc == LUA_I2C_ERR_ARG) {
        ud->closed = 1;
        return luaL_error(L,
            "imu.new: i2c%d rejected freq %d Hz (out of range for this bus)",
            idx, (int)freq);
    }
    if (rc != LUA_I2C_OK) {
        ud->closed = 1;
        return luaL_error(L, "imu.new: i2c%d acquire failed (%d)", idx, rc);
    }
    ud->acquired = 1;

    /* ---- Probe + configure the device ------------------------------------ */
    if (be->probe_init(&ud->bus) != 0) {
        uint8_t id = 0;
        be->who_am_i(&ud->bus, &id);
        imu_release_bus(ud);
        ud->closed = 1;
        return luaL_error(L,
            "imu.new: %s not found at i2c%d addr 0x%x (WHO_AM_I=0x%x, "
            "check wiring/address)", be->name, idx, (int)addr, (int)id);
    }

    return 1;
}

/* ── handle:read() -> {accel, gyro, temp, status} ────────────────────────── */

static int imu_read(lua_State *L)
{
    imu_ud_t   *ud = imu_get_ud(L, 1);
    imu_sample_t s;

    if (ud->be->read_sample(&ud->bus, &s) != 0) {
        return luaL_error(L, "imu: read failed");
    }

    lua_newtable(L);

    lua_newtable(L);
    lua_pushinteger(L, s.accel.x); lua_setfield(L, -2, "x");
    lua_pushinteger(L, s.accel.y); lua_setfield(L, -2, "y");
    lua_pushinteger(L, s.accel.z); lua_setfield(L, -2, "z");
    lua_setfield(L, -2, "accel");

    lua_newtable(L);
    lua_pushinteger(L, s.gyro.x); lua_setfield(L, -2, "x");
    lua_pushinteger(L, s.gyro.y); lua_setfield(L, -2, "y");
    lua_pushinteger(L, s.gyro.z); lua_setfield(L, -2, "z");
    lua_setfield(L, -2, "gyro");

    lua_pushinteger(L, s.temp);
    lua_setfield(L, -2, "temp");

    lua_pushinteger(L, s.int_status);
    lua_setfield(L, -2, "status");

    return 1;
}

/* ── handle:read_accel() -> ax, ay, az ───────────────────────────────────── */

static int imu_read_accel(lua_State *L)
{
    imu_ud_t   *ud = imu_get_ud(L, 1);
    imu_sample_t s;

    if (ud->be->read_sample(&ud->bus, &s) != 0) {
        return luaL_error(L, "imu: read_accel failed");
    }
    lua_pushinteger(L, s.accel.x);
    lua_pushinteger(L, s.accel.y);
    lua_pushinteger(L, s.accel.z);
    return 3;
}

/* ── handle:read_gyro() -> gx, gy, gz ─────────────────────────────────────── */

static int imu_read_gyro(lua_State *L)
{
    imu_ud_t   *ud = imu_get_ud(L, 1);
    imu_sample_t s;

    if (ud->be->read_sample(&ud->bus, &s) != 0) {
        return luaL_error(L, "imu: read_gyro failed");
    }
    lua_pushinteger(L, s.gyro.x);
    lua_pushinteger(L, s.gyro.y);
    lua_pushinteger(L, s.gyro.z);
    return 3;
}

/* ── handle:read_temperature() -> celsius (float) ────────────────────────── */

static int imu_read_temperature(lua_State *L)
{
    imu_ud_t *ud = imu_get_ud(L, 1);
    int16_t   raw = 0;

    if (ud->be->read_temp_raw(&ud->bus, &raw) != 0) {
        return luaL_error(L, "imu: read_temperature failed");
    }
    lua_pushnumber(L, (lua_Number)ud->be->temp_to_celsius(raw));
    return 1;
}

/* ── handle:read_int_status() -> integer ─────────────────────────────────── */

static int imu_read_int_status(lua_State *L)
{
    imu_ud_t *ud = imu_get_ud(L, 1);
    uint8_t   status = 0;

    if (ud->be->read_int_status(&ud->bus, &status) != 0) {
        return luaL_error(L, "imu: read_int_status failed");
    }
    lua_pushinteger(L, status);
    return 1;
}

/* ── handle:who_am_i() -> integer ────────────────────────────────────────── */

static int imu_who_am_i(lua_State *L)
{
    imu_ud_t *ud = imu_get_ud(L, 1);
    uint8_t   id = 0;

    if (ud->be->who_am_i(&ud->bus, &id) != 0) {
        return luaL_error(L, "imu: who_am_i failed");
    }
    lua_pushinteger(L, id);
    return 1;
}

/* ── handle:name() -> backend name ───────────────────────────────────────── */

static int imu_name(lua_State *L)
{
    imu_ud_t *ud = imu_get_ud(L, 1);
    lua_pushstring(L, ud->be->name);
    return 1;
}

/* ── handle:close() / __gc ───────────────────────────────────────────────── */

static int imu_close(lua_State *L)
{
    imu_ud_t *ud = (imu_ud_t *)luaL_checkudata(L, 1, IMU_METATABLE);
    if (!ud->closed) {
        imu_release_bus(ud);   /* drop our controller reference */
        ud->closed = 1;
    }
    return 0;
}

static int imu_gc(lua_State *L)
{
    imu_ud_t *ud = (imu_ud_t *)luaL_testudata(L, 1, IMU_METATABLE);
    if (ud && !ud->closed) {
        imu_release_bus(ud);
        ud->closed = 1;
    }
    return 0;
}

/* ── Driver init (once, single-threaded boot phase) ──────────────────────── */

void lua_module_imu_init(void)
{
    /* Nothing to initialise: the shared I2C controller locks/refcounts live in
     * lua_driver_i2c (lua_driver_i2c_init).  Kept for a uniform module ABI. */
}

/* ── Module open ─────────────────────────────────────────────────────────── */

static const luaL_Reg imu_methods[] = {
    { "read",             imu_read             },
    { "read_accel",       imu_read_accel       },
    { "read_gyro",        imu_read_gyro        },
    { "read_temperature", imu_read_temperature },
    { "read_int_status",  imu_read_int_status  },
    { "who_am_i",         imu_who_am_i         },
    { "name",             imu_name             },
    { "close",            imu_close            },
    { NULL, NULL }
};

LUAMOD_API int luaopen_imu(lua_State *L)
{
    if (luaL_newmetatable(L, IMU_METATABLE)) {
        lua_pushcfunction(L, imu_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        luaL_setfuncs(L, imu_methods, 0);
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, imu_new);
    lua_setfield(L, -2, "new");
    return 1;
}

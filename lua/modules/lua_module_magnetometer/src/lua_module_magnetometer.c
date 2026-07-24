/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_module_magnetometer.c — Lua magnetometer driver for Ameba RTOS (RTL8721F).
**
** Chip-agnostic front end.  Parses new() options, acquires the shared I2C
** controller via lua_driver_i2c, and dispatches to a per-chip backend vtable.
** Currently the only backend is the Bosch BMM150; adding more chips means adding
** a backend file + one registry entry in mag_backend.c, nothing here changes.
**
** I2C sharing:
**   All bus traffic goes through lua_driver_i2c's per-controller lock.  An imu
**   handle and this module on the SAME controller share one mutex, so their
**   transactions can never interleave.
**
** INT pin (optional):
**   If int_gpio is supplied, the pin is configured as a GPIO input.  Users may
**   read its level with the gpio module.  read_int_status() reads the I2C
**   interrupt-status register regardless of whether the INT pin is wired.
**
** Provides require("magnetometer"):
**   magnetometer.new({
**       sda       = "PA_26",   -- required
**       scl       = "PA_25",   -- required
**       i2c       = 0,         -- default 0
**       addr      = 0x10,      -- default from chip backend
**       freq      = 100000,    -- default from chip backend
**       chip      = "bmm150",  -- default first registered backend
**       int_gpio  = "PB_8",    -- optional INT pin (string or PinName int)
**   }) -> handle
**
**   No pin is hard-coded: all parameters come from the caller (board.json).
**
** Handle methods:
**   handle:read()             -> {magnetic={x,y,z},
**                                 temperature=0, status=n, calibrated=false}
**   handle:read_temperature() -> 0.0   (BMM150 has no temperature sensor)
**   handle:read_int_status()  -> integer  (interrupt status register)
**   handle:name()             -> "bmm150"
**   handle:close()
*/

#define LUA_LIB
#include "lua.h"
#include "lauxlib.h"

#include "lua_module_magnetometer.h"
#include "mag_backend.h"
#include "lua_driver_i2c.h"
#include "luhw.h"

#include "ameba_soc.h"

#include <stdlib.h>
#include <string.h>

/* ---- Constants ------------------------------------------------------------- */
#define MAG_METATABLE   "magnetometer.handle"
#define MAG_NUM_CTRL    2       /* I2C0 + I2C1 */
#define MAG_ADDR_MAX    0x7F

/* ---- Handle userdata ------------------------------------------------------- */
typedef struct {
    mag_bus_t            bus;       /* i2c_idx + addr */
    const mag_backend_t *be;        /* selected chip backend vtable */
    void                *state;     /* backend-private state (heap-allocated) */
    PinName              int_pin;   /* INT GPIO or NC if not used */
    int                  acquired;  /* 1 while holding I2C bus reference */
    int                  closed;
} mag_ud_t;

/* ---- Handle helpers -------------------------------------------------------- */

static mag_ud_t *mag_get_ud(lua_State *L, int idx)
{
    mag_ud_t *ud = (mag_ud_t *)luaL_checkudata(L, idx, MAG_METATABLE);
    if (!ud || ud->closed) {
        luaL_error(L, "magnetometer: invalid or closed handle");
    }
    return ud;
}

static void mag_release(mag_ud_t *ud)
{
    if (ud->acquired) {
        lua_i2c_bus_release(ud->bus.i2c_idx);
        ud->acquired = 0;
    }
    if (ud->state) {
        free(ud->state);
        ud->state = NULL;
    }
}

/* ---- INT GPIO setup -------------------------------------------------------- */

static void mag_setup_int_pin(PinName pin)
{
    if (pin == NC) {
        return;
    }
    Pinmux_Config(pin, PINMUX_FUNCTION_GPIO);
    GPIO_InitTypeDef init;
    init.GPIO_Pin  = pin;
    init.GPIO_Mode = GPIO_Mode_IN;
    init.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(&init);
}

/* ---- magnetometer.new(opts) ------------------------------------------------ */

static int mag_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    /* Parse SDA — required */
    lua_getfield(L, 1, "sda");
    if (lua_isnil(L, -1)) {
        return luaL_error(L, "magnetometer.new: 'sda' is required");
    }
    PinName sda = luhw_check_pin(L, -1);
    lua_pop(L, 1);

    /* Parse SCL — required */
    lua_getfield(L, 1, "scl");
    if (lua_isnil(L, -1)) {
        return luaL_error(L, "magnetometer.new: 'scl' is required");
    }
    PinName scl = luhw_check_pin(L, -1);
    lua_pop(L, 1);

    /* Parse i2c index */
    lua_getfield(L, 1, "i2c");
    int idx = lua_isnil(L, -1) ? 0 : (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    if (idx < 0 || idx >= MAG_NUM_CTRL) {
        return luaL_error(L, "magnetometer.new: 'i2c' must be 0 or 1, got %d", idx);
    }

    /* Resolve backend — supplies per-chip defaults */
    lua_getfield(L, 1, "chip");
    const char *chip_name = lua_isnil(L, -1) ? NULL : luaL_checkstring(L, -1);
    const mag_backend_t *be = mag_backend_find(chip_name);
    lua_pop(L, 1);
    if (!be) {
        return luaL_error(L, "magnetometer.new: unknown chip '%s'",
                          chip_name ? chip_name : "");
    }

    /* Parse addr (default from backend) */
    lua_getfield(L, 1, "addr");
    lua_Integer addr = lua_isnil(L, -1)
                       ? (lua_Integer)be->default_addr()
                       : luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    if (addr < 0 || addr > MAG_ADDR_MAX) {
        return luaL_error(L, "magnetometer.new: 'addr' out of range (0-0x7f)");
    }

    /* Parse freq (default 100 kHz) */
    lua_getfield(L, 1, "freq");
    lua_Integer freq = lua_isnil(L, -1) ? 100000 : luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    if (freq <= 0) {
        return luaL_error(L, "magnetometer.new: 'freq' must be > 0");
    }

    /* Parse optional INT pin */
    lua_getfield(L, 1, "int_gpio");
    PinName int_pin = NC;
    if (!lua_isnil(L, -1)) {
        int_pin = luhw_check_pin(L, -1);
    }
    lua_pop(L, 1);

    /* Allocate userdata BEFORE acquiring any resource */
    mag_ud_t *ud = (mag_ud_t *)lua_newuserdata(L, sizeof(*ud));
    ud->bus.i2c_idx = idx;
    ud->bus.addr    = (uint16_t)addr;
    ud->be          = be;
    ud->state       = NULL;
    ud->int_pin     = int_pin;
    ud->acquired    = 0;
    ud->closed      = 0;
    luaL_getmetatable(L, MAG_METATABLE);
    lua_setmetatable(L, -2);

    /* Allocate chip-private state */
    if (be->state_size > 0) {
        ud->state = calloc(1, be->state_size);
        if (!ud->state) {
            ud->closed = 1;
            return luaL_error(L, "magnetometer.new: out of memory");
        }
    }

    /* Acquire shared I2C controller */
    int rc = lua_i2c_bus_acquire(idx, (uint8_t)sda, (uint8_t)scl, (uint32_t)freq);
    if (rc == LUA_I2C_ERR_CONFIG) {
        mag_release(ud);
        ud->closed = 1;
        return luaL_error(L, "magnetometer.new: i2c%d already open with different pins/freq", idx);
    }
    if (rc != LUA_I2C_OK) {
        mag_release(ud);
        ud->closed = 1;
        return luaL_error(L, "magnetometer.new: i2c%d acquire failed (%d)", idx, rc);
    }
    ud->acquired = 1;

    /* Configure INT GPIO (input, no ISR) */
    mag_setup_int_pin(int_pin);

    /* Probe and configure the chip */
    int probe_rc = be->probe(&ud->bus, ud->state, (uint8_t)addr);
    if (probe_rc != 0 && be->probe_alternates) {
        probe_rc = be->probe_alternates(&ud->bus, ud->state, (uint8_t)addr);
    }
    if (probe_rc != 0) {
        mag_release(ud);
        ud->closed = 1;
        return luaL_error(L, "magnetometer.new: %s not found at i2c%d addr 0x%02x "
                          "(check wiring, I2C mode solder bridge, and SDO strap)",
                          be->chip_name, idx, (int)addr);
    }

    RTK_LOGS(NOTAG, RTK_LOG_INFO, "[magnetometer] %s ready on i2c%d addr=%d\n",
             be->chip_name, idx, (int)ud->bus.addr);
    return 1;
}

/* ---- handle:read() --------------------------------------------------------- */

static void push_mag_xyz(lua_State *L, float x, float y, float z)
{
    lua_newtable(L);
    lua_pushnumber(L, (lua_Number)x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, (lua_Number)y);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, (lua_Number)z);
    lua_setfield(L, -2, "z");
}

static int mag_read(lua_State *L)
{
    mag_ud_t   *ud  = mag_get_ud(L, 1);
    mag_sample_t s  = { 0 };

    if (ud->be->read_sample(&ud->bus, ud->state, &s) != 0) {
        return luaL_error(L, "magnetometer: read failed");
    }

    lua_newtable(L);

    push_mag_xyz(L, s.x, s.y, s.z);
    lua_setfield(L, -2, "magnetic");

    lua_pushnumber(L, (lua_Number)s.temperature);
    lua_setfield(L, -2, "temperature");

    lua_pushinteger(L, (lua_Integer)s.status);
    lua_setfield(L, -2, "status");

    lua_pushboolean(L, 0); /* calibrated = false (no calibration implemented) */
    lua_setfield(L, -2, "calibrated");

    return 1;
}

/* ---- handle:read_temperature() -------------------------------------------- */

static int mag_read_temperature(lua_State *L)
{
    mag_ud_t *ud = mag_get_ud(L, 1);
    mag_sample_t s = { 0 };
    /* Read a full sample; temperature field is 0 for BMM150. */
    if (ud->be->read_sample(&ud->bus, ud->state, &s) != 0) {
        return luaL_error(L, "magnetometer: read_temperature failed");
    }
    lua_pushnumber(L, (lua_Number)s.temperature);
    return 1;
}

/* ---- handle:read_int_status() ---------------------------------------------- */

static int mag_read_int_status(lua_State *L)
{
    mag_ud_t *ud = mag_get_ud(L, 1);
    uint8_t  status = 0;
    if (ud->be->read_status(&ud->bus, ud->state, &status) != 0) {
        return luaL_error(L, "magnetometer: read_int_status failed");
    }
    lua_pushinteger(L, (lua_Integer)status);
    return 1;
}

/* ---- handle:name() --------------------------------------------------------- */

static int mag_name(lua_State *L)
{
    mag_ud_t *ud = mag_get_ud(L, 1);
    lua_pushstring(L, ud->be->chip_name);
    return 1;
}

/* ---- handle:close() / __gc ------------------------------------------------- */

static int mag_close(lua_State *L)
{
    mag_ud_t *ud = (mag_ud_t *)luaL_checkudata(L, 1, MAG_METATABLE);
    if (!ud->closed) {
        mag_release(ud);
        ud->closed = 1;
    }
    return 0;
}

static int mag_gc(lua_State *L)
{
    mag_ud_t *ud = (mag_ud_t *)luaL_testudata(L, 1, MAG_METATABLE);
    if (ud && !ud->closed) {
        mag_release(ud);
        ud->closed = 1;
    }
    return 0;
}

/* ---- Module init (once, single-threaded boot) ----------------------------- */

void lua_module_magnetometer_init(void)
{
    /* No module-level state to initialise: the shared I2C controller locks
     * live in lua_driver_i2c (lua_driver_i2c_init).  Kept for uniform ABI. */
}

/* ---- Module open ----------------------------------------------------------- */

static const luaL_Reg mag_methods[] = {
    { "read",             mag_read             },
    { "read_temperature", mag_read_temperature },
    { "read_int_status",  mag_read_int_status  },
    { "name",             mag_name             },
    { "close",            mag_close            },
    { NULL, NULL }
};

LUAMOD_API int luaopen_magnetometer(lua_State *L)
{
    if (luaL_newmetatable(L, MAG_METATABLE)) {
        lua_pushcfunction(L, mag_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        luaL_setfuncs(L, mag_methods, 0);
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, mag_new);
    lua_setfield(L, -2, "new");
    return 1;
}

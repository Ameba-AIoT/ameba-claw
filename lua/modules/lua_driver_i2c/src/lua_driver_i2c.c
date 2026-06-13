/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_driver_i2c.h"

#include <string.h>

#include "ameba_soc.h"
#include "lauxlib.h"
#include "luhw.h"

/* Defined in fwlib ram_common/ameba_i2c.c but missing from ameba_i2c.h. */
extern u32 I2C_SlaveReadTimeOut(I2C_TypeDef *I2Cx, u8 *pBuf, u32 len, u32 ms);

#define LUA_DRIVER_I2C_BUS_MT       "i2c.bus"
#define LUA_DRIVER_I2C_DEV_MT       "i2c.device"
#define LUA_DRIVER_I2C_SLAVE_MT     "i2c.slave"
#define LUA_DRIVER_I2C_MAX_DATA     4096
#define LUA_DRIVER_I2C_DEFAULT_FREQ 100000  /* Hz */
#define LUA_DRIVER_I2C_SCAN_TIMEOUT 10      /* ms per address probe */

/* ---- Per-controller concurrency guard -----------------------------------
 *
 * Multiple lua_run calls may execute concurrently (cap_lua allows up to 2
 * parallel jobs; timer callbacks via cap.call add a third execution path).
 * If two calls both do i2c.new → write → close on the same physical I2C
 * controller, close() shuts down the shared peripheral clock while the
 * other call is still mid-transaction, corrupting SDK driver state and
 * eventually the heap (bus fault, R3=0xdeadbeef).
 *
 * Fix (per debug_lua_i2c_concurrency_crash.md §7.1):
 *   1. One rtos_mutex per controller; held for the entire new/write/close
 *      sequence.  Created once in lua_driver_i2c_init() (called from
 *      lua_module_registry_provision_all() in the single-threaded boot
 *      phase) so it is always valid before any concurrent Lua execution.
 *   2. close()/__gc no longer disables the peripheral clock — they only
 *      mark the Lua handle as closed.  The clock stays on permanently once
 *      the controller has been opened; re-enabling it on the next new() is
 *      a no-op that costs nothing.
 *   3. i2c.new() is idempotent: a per-controller 'inited' flag prevents
 *      repeated I2C_Init() calls that would reset registers mid-transfer.
 * -----------------------------------------------------------------------*/

#define I2C_NUM_CONTROLLERS 2

static rtos_mutex_t s_i2c_lock[I2C_NUM_CONTROLLERS];
static int          s_i2c_inited[I2C_NUM_CONTROLLERS]; /* 1 after first I2C_Init */

static const u32 s_i2c_ip_clk[2] = {I2C0_1_IPCLK, I2C0_1_IPCLK};

static const u32 s_i2c_scl_pinmux[2] = {
    PINMUX_FUNCTION_I2C0_SCL,
    PINMUX_FUNCTION_I2C1_SCL,
};

static const u32 s_i2c_sda_pinmux[2] = {
    PINMUX_FUNCTION_I2C0_SDA,
    PINMUX_FUNCTION_I2C1_SDA,
};

typedef struct {
    I2C_TypeDef *dev;
    int          idx;
    int          closed;
} lua_driver_i2c_bus_ud_t;

typedef struct {
    I2C_TypeDef *dev;
    int          bus_ref;  /* LUA_REGISTRYINDEX ref keeping bus alive */
    u16          addr;
    int          closed;
} lua_driver_i2c_dev_ud_t;

typedef struct {
    I2C_TypeDef *dev;
    int          idx;
    u16          addr;
    int          closed;
} lua_driver_i2c_slave_ud_t;

/* ---- Helpers ---- */

static lua_driver_i2c_bus_ud_t *lua_driver_i2c_bus_get_ud(lua_State *L, int stack_idx)
{
    lua_driver_i2c_bus_ud_t *ud = (lua_driver_i2c_bus_ud_t *)luaL_checkudata(
        L, stack_idx, LUA_DRIVER_I2C_BUS_MT);
    if (!ud || ud->closed) {
        luaL_error(L, "i2c: invalid or closed bus");
    }
    return ud;
}

static lua_driver_i2c_dev_ud_t *lua_driver_i2c_dev_get_ud(lua_State *L, int stack_idx)
{
    lua_driver_i2c_dev_ud_t *ud = (lua_driver_i2c_dev_ud_t *)luaL_checkudata(
        L, stack_idx, LUA_DRIVER_I2C_DEV_MT);
    if (!ud || ud->closed) {
        luaL_error(L, "i2c: invalid or closed device");
    }
    return ud;
}

/* ---- i2c.new(idx, sda, scl [, freq_hz]) → bus ---- */
static int lua_driver_i2c_new(lua_State *L)
{
    lua_Integer idx     = luaL_checkinteger(L, 1);
    PinName     sda_pin = luhw_check_pin(L, 2);
    PinName     scl_pin = luhw_check_pin(L, 3);
    lua_Integer freq_hz = luaL_optinteger(L, 4, LUA_DRIVER_I2C_DEFAULT_FREQ);

    if (idx < 0 || idx > 1) {
        return luaL_error(L, "i2c idx must be 0 or 1");
    }
    if (freq_hz <= 0 || freq_hz > 3400000) {
        return luaL_error(L, "i2c freq_hz must be 1-3400000");
    }
    if (idx == 0 && freq_hz > 400000) {
        return luaL_error(L, "i2c0 only supports up to 400000 Hz");
    }

    int i = (int)idx;

    rtos_mutex_take(s_i2c_lock[i], 0xFFFFFFFFUL);

    /* Always re-enable the clock (idempotent if already on). */
    RCC_PeriphClockCmd(
        i == 0 ? APBPeriph_I2C0 : APBPeriph_I2C1,
        i == 0 ? APBPeriph_I2C0_CLOCK : APBPeriph_I2C1_CLOCK,
        ENABLE);

    Pinmux_Config((u8)sda_pin, s_i2c_sda_pinmux[i]);
    Pinmux_Config((u8)scl_pin, s_i2c_scl_pinmux[i]);
    PAD_PullCtrl((u8)sda_pin, GPIO_PuPd_UP);
    PAD_PullCtrl((u8)scl_pin, GPIO_PuPd_UP);

    /* Only call I2C_Init the first time — re-initialising a live controller
     * mid-transfer from a concurrent execution would reset its registers. */
    if (!s_i2c_inited[i]) {
        I2C_InitTypeDef init;
        I2C_StructInit(&init);
        init.I2CIdx     = (u32)i;
        init.I2CMaster  = I2C_MASTER_MODE;
        init.I2CAddrMod = I2C_ADDR_7BIT;
        init.I2CIPClk   = s_i2c_ip_clk[i];
        init.I2CClk     = (u32)(freq_hz / 1000);  /* fwlib field is in kHz */

        if (freq_hz <= 100000) {
            init.I2CSpdMod = I2C_SS_MODE;
        } else if (freq_hz <= 400000) {
            init.I2CSpdMod = I2C_FS_MODE;
        } else {
            init.I2CSpdMod = I2C_HS_MODE;
        }

        I2C_TypeDef *dev = I2C_DEV_TABLE[i].I2Cx;
        I2C_Init(dev, &init);
        I2C_Cmd(dev, ENABLE);
        s_i2c_inited[i] = 1;
    }

    I2C_TypeDef *dev = I2C_DEV_TABLE[i].I2Cx;

    rtos_mutex_give(s_i2c_lock[i]);

    lua_driver_i2c_bus_ud_t *ud = (lua_driver_i2c_bus_ud_t *)lua_newuserdata(
        L, sizeof(*ud));
    ud->dev    = dev;
    ud->idx    = i;
    ud->closed = 0;
    luaL_getmetatable(L, LUA_DRIVER_I2C_BUS_MT);
    lua_setmetatable(L, -2);
    return 1;
}

/* ---- bus:scan() → table of detected 7-bit addresses ---- */
static int lua_driver_i2c_bus_scan(lua_State *L)
{
    lua_driver_i2c_bus_ud_t *ud = lua_driver_i2c_bus_get_ud(L, 1);

    lua_newtable(L);
    int count = 0;

    rtos_mutex_take(s_i2c_lock[ud->idx], 0xFFFFFFFFUL);
    for (int addr = 1; addr < 128; addr++) {
        I2C_SetSlaveAddress(ud->dev, (u16)addr);
        s32 ret = I2C_MasterSendNullData_TimeOut(ud->dev, addr,
                                                  LUA_DRIVER_I2C_SCAN_TIMEOUT);
        if (ret == 0) {
            count++;
            lua_pushinteger(L, addr);
            lua_rawseti(L, -2, count);
        }
    }
    rtos_mutex_give(s_i2c_lock[ud->idx]);

    return 1;
}

/* ---- bus:device(addr7) → device object ---- */
static int lua_driver_i2c_bus_device(lua_State *L)
{
    lua_driver_i2c_bus_ud_t *bus_ud = lua_driver_i2c_bus_get_ud(L, 1);
    lua_Integer              addr   = luaL_checkinteger(L, 2);

    if (addr < 0 || addr > 0x7F) {
        return luaL_error(L, "i2c device addr must be 0-127");
    }

    lua_driver_i2c_dev_ud_t *dev_ud = (lua_driver_i2c_dev_ud_t *)lua_newuserdata(
        L, sizeof(*dev_ud));
    dev_ud->dev    = bus_ud->dev;
    dev_ud->addr   = (u16)addr;
    dev_ud->closed = 0;

    /* Anchor the bus in the registry so it cannot be GC'd before the device */
    lua_pushvalue(L, 1);
    dev_ud->bus_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    luaL_getmetatable(L, LUA_DRIVER_I2C_DEV_MT);
    lua_setmetatable(L, -2);
    return 1;
}

/* ---- bus:close() ---- */
static int lua_driver_i2c_bus_close(lua_State *L)
{
    lua_driver_i2c_bus_ud_t *ud = (lua_driver_i2c_bus_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_I2C_BUS_MT);
    /* Do NOT disable the peripheral clock — other concurrent Lua executions
     * may still be using the same physical controller.  Just mark closed. */
    ud->closed = 1;
    return 0;
}

static int lua_driver_i2c_bus_gc(lua_State *L)
{
    lua_driver_i2c_bus_ud_t *ud = (lua_driver_i2c_bus_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_I2C_BUS_MT);
    if (ud) {
        ud->closed = 1;
    }
    return 0;
}

/* ---- dev:read_byte([mem_addr]) → integer ---- */
static int lua_driver_i2c_dev_read_byte(lua_State *L)
{
    lua_driver_i2c_dev_ud_t *ud = lua_driver_i2c_dev_get_ud(L, 1);
    u8 result = 0;

    /* Determine idx from the underlying I2C peripheral pointer. */
    int i = (ud->dev == I2C_DEV_TABLE[0].I2Cx) ? 0 : 1;

    int has_mem = !lua_isnoneornil(L, 2);
    u8  mem     = 0;
    if (has_mem) {
        lua_Integer m = luaL_checkinteger(L, 2);
        if (m < 0 || m > 0xFF) {
            return luaL_error(L, "i2c mem_addr must be 0-255");
        }
        mem = (u8)m;
    }

    rtos_mutex_take(s_i2c_lock[i], 0xFFFFFFFFUL);
    I2C_SetSlaveAddress(ud->dev, ud->addr);
    if (has_mem) {
        I2C_MasterRepeatRead(ud->dev, &mem, 1, &result, 1);
    } else {
        I2C_MasterRead(ud->dev, &result, 1);
    }
    rtos_mutex_give(s_i2c_lock[i]);

    lua_pushinteger(L, (lua_Integer)result);
    return 1;
}

/* ---- dev:read(len [, mem_addr]) → string ---- */
static int lua_driver_i2c_dev_read(lua_State *L)
{
    lua_driver_i2c_dev_ud_t *ud  = lua_driver_i2c_dev_get_ud(L, 1);
    lua_Integer              len = luaL_checkinteger(L, 2);

    if (len <= 0 || len > LUA_DRIVER_I2C_MAX_DATA) {
        return luaL_error(L, "i2c read len must be 1-%d", LUA_DRIVER_I2C_MAX_DATA);
    }

    /* Check optional mem_addr BEFORE pushing the buffer userdata onto the stack,
     * otherwise lua_newuserdata shifts index 3 and lua_isnoneornil(L,3) would
     * see the buffer instead of the argument. */
    int has_mem = !lua_isnoneornil(L, 3);
    u8  mem     = 0;
    if (has_mem) {
        lua_Integer m = luaL_checkinteger(L, 3);
        if (m < 0 || m > 0xFF) {
            return luaL_error(L, "i2c mem_addr must be 0-255");
        }
        mem = (u8)m;
    }

    u8 *buf = (u8 *)lua_newuserdata(L, (size_t)len);
    int i = (ud->dev == I2C_DEV_TABLE[0].I2Cx) ? 0 : 1;

    rtos_mutex_take(s_i2c_lock[i], 0xFFFFFFFFUL);
    I2C_SetSlaveAddress(ud->dev, ud->addr);
    if (has_mem) {
        I2C_MasterRepeatRead(ud->dev, &mem, 1, buf, (u32)len);
    } else {
        I2C_MasterRead(ud->dev, buf, (u32)len);
    }
    rtos_mutex_give(s_i2c_lock[i]);

    lua_pushlstring(L, (const char *)buf, (size_t)len);
    return 1;
}

/* ---- dev:write_byte(value [, mem_addr]) ---- */
static int lua_driver_i2c_dev_write_byte(lua_State *L)
{
    lua_driver_i2c_dev_ud_t *ud  = lua_driver_i2c_dev_get_ud(L, 1);
    lua_Integer              val = luaL_checkinteger(L, 2);

    if (val < 0 || val > 0xFF) {
        return luaL_error(L, "i2c write_byte value must be 0-255");
    }

    int has_mem = !lua_isnoneornil(L, 3);
    u8  m       = 0;
    if (has_mem) {
        lua_Integer mv = luaL_checkinteger(L, 3);
        if (mv < 0 || mv > 0xFF) {
            return luaL_error(L, "i2c mem_addr must be 0-255");
        }
        m = (u8)mv;
    }

    int i = (ud->dev == I2C_DEV_TABLE[0].I2Cx) ? 0 : 1;

    rtos_mutex_take(s_i2c_lock[i], 0xFFFFFFFFUL);
    I2C_SetSlaveAddress(ud->dev, ud->addr);
    if (has_mem) {
        u8 buf[2] = {m, (u8)val};
        I2C_MasterWrite(ud->dev, buf, 2);
    } else {
        u8 buf[1] = {(u8)val};
        I2C_MasterWrite(ud->dev, buf, 1);
    }
    rtos_mutex_give(s_i2c_lock[i]);

    return 0;
}

/* ---- dev:write(data [, mem_addr])  data: string or table of bytes ---- */
static int lua_driver_i2c_dev_write(lua_State *L)
{
    lua_driver_i2c_dev_ud_t *ud = lua_driver_i2c_dev_get_ud(L, 1);

    int has_mem_addr  = !lua_isnoneornil(L, 3);
    u8  mem_addr_byte = 0;
    if (has_mem_addr) {
        lua_Integer m = luaL_checkinteger(L, 3);
        if (m < 0 || m > 0xFF) {
            return luaL_error(L, "i2c mem_addr must be 0-255");
        }
        mem_addr_byte = (u8)m;
    }

    const u8 *raw_data = NULL;
    size_t    raw_len  = 0;

    int type = lua_type(L, 2);
    if (type == LUA_TSTRING) {
        raw_data = (const u8 *)lua_tolstring(L, 2, &raw_len);
    } else if (type == LUA_TTABLE) {
        lua_Integer n = luaL_len(L, 2);
        if (n < 0 || n > LUA_DRIVER_I2C_MAX_DATA) {
            return luaL_error(L, "i2c write table too large (max %d)",
                              LUA_DRIVER_I2C_MAX_DATA);
        }
        u8 *tbuf = (u8 *)lua_newuserdata(L, (size_t)(n > 0 ? n : 1));
        for (lua_Integer i = 0; i < n; i++) {
            lua_rawgeti(L, 2, i + 1);
            lua_Integer byte = luaL_checkinteger(L, -1);
            if (byte < 0 || byte > 0xFF) {
                return luaL_error(L, "i2c write byte #%d out of range 0-255",
                                  (int)(i + 1));
            }
            tbuf[i] = (u8)byte;
            lua_pop(L, 1);
        }
        raw_data = tbuf;
        raw_len  = (size_t)n;
    } else {
        return luaL_error(L, "i2c write expects a string or table");
    }

    int i = (ud->dev == I2C_DEV_TABLE[0].I2Cx) ? 0 : 1;

    rtos_mutex_take(s_i2c_lock[i], 0xFFFFFFFFUL);
    I2C_SetSlaveAddress(ud->dev, ud->addr);
    if (has_mem_addr) {
        u8 *combined = (u8 *)lua_newuserdata(L, raw_len + 1);
        combined[0] = mem_addr_byte;
        if (raw_len > 0) {
            memcpy(combined + 1, raw_data, raw_len);
        }
        I2C_MasterWrite(ud->dev, combined, (u32)(raw_len + 1));
    } else if (raw_len > 0) {
        I2C_MasterWrite(ud->dev, (u8 *)raw_data, (u32)raw_len);
    }
    rtos_mutex_give(s_i2c_lock[i]);

    return 0;
}

/* ---- dev:address() → integer ---- */
static int lua_driver_i2c_dev_address(lua_State *L)
{
    lua_driver_i2c_dev_ud_t *ud = lua_driver_i2c_dev_get_ud(L, 1);
    lua_pushinteger(L, (lua_Integer)ud->addr);
    return 1;
}

/* ---- dev:close() ---- */
static int lua_driver_i2c_dev_close(lua_State *L)
{
    lua_driver_i2c_dev_ud_t *ud = (lua_driver_i2c_dev_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_I2C_DEV_MT);
    if (!ud->closed) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->bus_ref);
        ud->bus_ref = LUA_NOREF;
        ud->closed  = 1;
    }
    return 0;
}

static int lua_driver_i2c_dev_gc(lua_State *L)
{
    lua_driver_i2c_dev_ud_t *ud = (lua_driver_i2c_dev_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_I2C_DEV_MT);
    if (ud && !ud->closed) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->bus_ref);
        ud->bus_ref = LUA_NOREF;
        ud->closed  = 1;
    }
    return 0;
}

/* ================================================================== */
/* Slave-side interface                                               */
/*                                                                    */
/* A thin Lua wrapper over fwlib I2C_SlaveRead / I2C_SlaveWrite so    */
/* test code can run an I2C slave without ever touching IC_STATUS,    */
/* IC_DATA_CMD, IC_RAW_INTR_STAT or other low-level registers.        */
/* ================================================================== */

static lua_driver_i2c_slave_ud_t *lua_driver_i2c_slave_get_ud(lua_State *L, int stack_idx)
{
    lua_driver_i2c_slave_ud_t *ud = (lua_driver_i2c_slave_ud_t *)luaL_checkudata(
        L, stack_idx, LUA_DRIVER_I2C_SLAVE_MT);
    if (!ud || ud->closed) {
        luaL_error(L, "i2c: invalid or closed slave");
    }
    return ud;
}

/* ---- i2c.new_slave(idx, sda, scl, addr7 [, freq_hz]) → slave ---- */
static int lua_driver_i2c_new_slave(lua_State *L)
{
    lua_Integer idx     = luaL_checkinteger(L, 1);
    PinName     sda_pin = luhw_check_pin(L, 2);
    PinName     scl_pin = luhw_check_pin(L, 3);
    lua_Integer addr    = luaL_checkinteger(L, 4);
    lua_Integer freq_hz = luaL_optinteger(L, 5, LUA_DRIVER_I2C_DEFAULT_FREQ);

    if (idx < 0 || idx > 1) {
        return luaL_error(L, "i2c idx must be 0 or 1");
    }
    if (addr < 0 || addr > 0x7F) {
        return luaL_error(L, "i2c slave addr must be 0-127");
    }
    if (freq_hz <= 0 || freq_hz > 3400000) {
        return luaL_error(L, "i2c freq_hz must be 1-3400000");
    }
    if (idx == 0 && freq_hz > 400000) {
        return luaL_error(L, "i2c0 only supports up to 400000 Hz");
    }

    int i = (int)idx;

    rtos_mutex_take(s_i2c_lock[i], 0xFFFFFFFFUL);

    RCC_PeriphClockCmd(
        i == 0 ? APBPeriph_I2C0 : APBPeriph_I2C1,
        i == 0 ? APBPeriph_I2C0_CLOCK : APBPeriph_I2C1_CLOCK,
        ENABLE);

    Pinmux_Config((u8)sda_pin, s_i2c_sda_pinmux[i]);
    Pinmux_Config((u8)scl_pin, s_i2c_scl_pinmux[i]);
    PAD_PullCtrl((u8)sda_pin, GPIO_PuPd_UP);
    PAD_PullCtrl((u8)scl_pin, GPIO_PuPd_UP);

    if (!s_i2c_inited[i]) {
        I2C_InitTypeDef init;
        I2C_StructInit(&init);
        init.I2CIdx     = (u32)i;
        init.I2CMaster  = I2C_SLAVE_MODE;
        init.I2CAddrMod = I2C_ADDR_7BIT;
        init.I2CAckAddr = (u16)addr;
        init.I2CIPClk   = s_i2c_ip_clk[i];
        init.I2CClk     = (u32)(freq_hz / 1000);  /* fwlib field is in kHz */

        if (freq_hz <= 100000) {
            init.I2CSpdMod = I2C_SS_MODE;
        } else if (freq_hz <= 400000) {
            init.I2CSpdMod = I2C_FS_MODE;
        } else {
            init.I2CSpdMod = I2C_HS_MODE;
        }

        I2C_TypeDef *dev = I2C_DEV_TABLE[i].I2Cx;
        I2C_Init(dev, &init);
        I2C_Cmd(dev, ENABLE);
        s_i2c_inited[i] = 1;
    }

    I2C_TypeDef *dev = I2C_DEV_TABLE[i].I2Cx;
    rtos_mutex_give(s_i2c_lock[i]);

    lua_driver_i2c_slave_ud_t *ud = (lua_driver_i2c_slave_ud_t *)lua_newuserdata(
        L, sizeof(*ud));
    ud->dev    = dev;
    ud->idx    = i;
    ud->addr   = (u16)addr;
    ud->closed = 0;
    luaL_getmetatable(L, LUA_DRIVER_I2C_SLAVE_MT);
    lua_setmetatable(L, -2);
    return 1;
}

/* ---- slave:read(len [, timeout_ms]) → string (received bytes) ----
 * Blocks until len bytes are received from the master, or the (optional)
 * timeout elapses. Returns whatever was actually received, which may be
 * shorter than len (or empty) on timeout. */
static int lua_driver_i2c_slave_read(lua_State *L)
{
    lua_driver_i2c_slave_ud_t *ud  = lua_driver_i2c_slave_get_ud(L, 1);
    lua_Integer                len = luaL_checkinteger(L, 2);

    if (len <= 0 || len > LUA_DRIVER_I2C_MAX_DATA) {
        return luaL_error(L, "i2c slave read len must be 1-%d", LUA_DRIVER_I2C_MAX_DATA);
    }

    int has_timeout = !lua_isnoneornil(L, 3);
    u32 timeout_ms  = 0;
    if (has_timeout) {
        lua_Integer t = luaL_checkinteger(L, 3);
        if (t < 0) {
            return luaL_error(L, "i2c slave read timeout_ms must be >= 0");
        }
        timeout_ms = (u32)t;
    }

    u8 *buf = (u8 *)lua_newuserdata(L, (size_t)len);

    rtos_mutex_take(s_i2c_lock[ud->idx], 0xFFFFFFFFUL);
    u32 got;
    if (has_timeout) {
        got = I2C_SlaveReadTimeOut(ud->dev, buf, (u32)len, timeout_ms);
    } else {
        got = I2C_SlaveRead(ud->dev, buf, (u32)len);
    }
    rtos_mutex_give(s_i2c_lock[ud->idx]);

    lua_pushlstring(L, (const char *)buf, (size_t)got);
    return 1;
}

/* ---- slave:write(data) → integer (bytes sent)  data: string or table ----
 * Blocks until the master issues a read request, then serves the bytes.
 * Returns the number of bytes actually accepted by the master. */
static int lua_driver_i2c_slave_write(lua_State *L)
{
    lua_driver_i2c_slave_ud_t *ud = lua_driver_i2c_slave_get_ud(L, 1);

    const u8 *raw_data = NULL;
    size_t    raw_len  = 0;

    int type = lua_type(L, 2);
    if (type == LUA_TSTRING) {
        raw_data = (const u8 *)lua_tolstring(L, 2, &raw_len);
    } else if (type == LUA_TTABLE) {
        lua_Integer n = luaL_len(L, 2);
        if (n < 0 || n > LUA_DRIVER_I2C_MAX_DATA) {
            return luaL_error(L, "i2c slave write table too large (max %d)",
                              LUA_DRIVER_I2C_MAX_DATA);
        }
        u8 *tbuf = (u8 *)lua_newuserdata(L, (size_t)(n > 0 ? n : 1));
        for (lua_Integer i = 0; i < n; i++) {
            lua_rawgeti(L, 2, i + 1);
            lua_Integer byte = luaL_checkinteger(L, -1);
            if (byte < 0 || byte > 0xFF) {
                return luaL_error(L, "i2c slave write byte #%d out of range 0-255",
                                  (int)(i + 1));
            }
            tbuf[i] = (u8)byte;
            lua_pop(L, 1);
        }
        raw_data = tbuf;
        raw_len  = (size_t)n;
    } else {
        return luaL_error(L, "i2c slave write expects a string or table");
    }

    rtos_mutex_take(s_i2c_lock[ud->idx], 0xFFFFFFFFUL);
    u32 sent = 0;
    if (raw_len > 0) {
        sent = I2C_SlaveWrite(ud->dev, (u8 *)raw_data, (u32)raw_len);
    }
    rtos_mutex_give(s_i2c_lock[ud->idx]);

    lua_pushinteger(L, (lua_Integer)sent);
    return 1;
}

/* ---- slave:address() → integer ---- */
static int lua_driver_i2c_slave_address(lua_State *L)
{
    lua_driver_i2c_slave_ud_t *ud = lua_driver_i2c_slave_get_ud(L, 1);
    lua_pushinteger(L, (lua_Integer)ud->addr);
    return 1;
}

static void lua_driver_i2c_slave_teardown(lua_driver_i2c_slave_ud_t *ud)
{
    /* Do not disable the peripheral clock — mark closed only. */
    if (ud) {
        ud->closed = 1;
    }
}

/* ---- slave:close() ---- */
static int lua_driver_i2c_slave_close(lua_State *L)
{
    lua_driver_i2c_slave_ud_t *ud = (lua_driver_i2c_slave_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_I2C_SLAVE_MT);
    lua_driver_i2c_slave_teardown(ud);
    return 0;
}

static int lua_driver_i2c_slave_gc(lua_State *L)
{
    lua_driver_i2c_slave_ud_t *ud = (lua_driver_i2c_slave_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_I2C_SLAVE_MT);
    lua_driver_i2c_slave_teardown(ud);
    return 0;
}

/* ---- Driver-level init (called once from lua_module_registry_provision_all,
 *      single-threaded boot phase, before any concurrent Lua execution). ---- */
void lua_driver_i2c_init(void)
{
    for (int i = 0; i < I2C_NUM_CONTROLLERS; i++) {
        if (s_i2c_lock[i] == NULL) {
            rtos_mutex_create(&s_i2c_lock[i]);
        }
        s_i2c_inited[i] = 0;
    }
}

/* ---- Module open ---- */
int luaopen_i2c(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_DRIVER_I2C_BUS_MT)) {
        lua_pushcfunction(L, lua_driver_i2c_bus_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_driver_i2c_bus_scan);
        lua_setfield(L, -2, "scan");
        lua_pushcfunction(L, lua_driver_i2c_bus_device);
        lua_setfield(L, -2, "device");
        lua_pushcfunction(L, lua_driver_i2c_bus_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    if (luaL_newmetatable(L, LUA_DRIVER_I2C_DEV_MT)) {
        lua_pushcfunction(L, lua_driver_i2c_dev_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_driver_i2c_dev_read_byte);
        lua_setfield(L, -2, "read_byte");
        lua_pushcfunction(L, lua_driver_i2c_dev_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_driver_i2c_dev_write_byte);
        lua_setfield(L, -2, "write_byte");
        lua_pushcfunction(L, lua_driver_i2c_dev_write);
        lua_setfield(L, -2, "write");
        lua_pushcfunction(L, lua_driver_i2c_dev_address);
        lua_setfield(L, -2, "address");
        lua_pushcfunction(L, lua_driver_i2c_dev_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    if (luaL_newmetatable(L, LUA_DRIVER_I2C_SLAVE_MT)) {
        lua_pushcfunction(L, lua_driver_i2c_slave_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_driver_i2c_slave_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_driver_i2c_slave_write);
        lua_setfield(L, -2, "write");
        lua_pushcfunction(L, lua_driver_i2c_slave_address);
        lua_setfield(L, -2, "address");
        lua_pushcfunction(L, lua_driver_i2c_slave_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_driver_i2c_new);
    lua_setfield(L, -2, "new");
    lua_pushcfunction(L, lua_driver_i2c_new_slave);
    lua_setfield(L, -2, "new_slave");
    return 1;
}

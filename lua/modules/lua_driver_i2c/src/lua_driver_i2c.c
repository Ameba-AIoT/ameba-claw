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
#define LUA_DRIVER_I2C_MAX_FREQ     3400000 /* Hz, high-speed ceiling */
#define LUA_DRIVER_I2C_FS_MAX_FREQ  400000  /* Hz, fast-mode ceiling */
#define LUA_DRIVER_I2C_SS_MAX_FREQ  100000  /* Hz, standard-mode ceiling */
#define LUA_DRIVER_I2C_ADDR_MAX     0x7F    /* 7-bit address space */
#define LUA_DRIVER_I2C_MEM_MAX      0xFF    /* 8-bit register offset */
#define LUA_DRIVER_I2C_NUM_CTRL     2       /* I2C0 + I2C1 */

/* Bounded wait when acquiring a controller lock.  concurrency.md forbids the
 * "wait-forever take + blocking I/O while holding the lock" pattern (no escape
 * path); a finite timeout guarantees a stuck controller surfaces as an error
 * instead of hanging every caller. */
#define LUA_DRIVER_I2C_LOCK_TIMEOUT_MS 5000

/* Maximum time slave:read() holds the controller lock per call when no timeout
 * is passed by the caller.  Must be finite: if two lua_run tasks both open the
 * same slave controller (refcnt=2) and one calls slave:read() with no timeout,
 * the other task's GC (ctrl_release, RTOS_MAX_DELAY) would deadlock waiting for
 * the lock.  A bounded poll window guarantees the lock is always given back;
 * callers wanting "wait forever" should loop on the empty-string return. */
#define LUA_DRIVER_I2C_SLAVE_READ_POLL_MS 2000

/* ---- Concurrency design (per concurrency.md STEP 1 classification) -------
 *
 * Peripheral class : BUS type (I2C) — one wire transaction is a multi-step
 *                    sequence; multiple Lua devices share one physical channel.
 * Called from ISR? : NO — every exported function runs in a Lua task context
 *                    (cap_lua jobs / timer callbacks), never from an ISR.
 *                    => rtos_mutex is the correct primitive (decision tree).
 *
 * cap_lua allows several lua_run jobs to execute concurrently, and timer /
 * cap.call callbacks add more execution paths, so the SAME controller may be
 * driven by 2-3 flows at once.  The driver follows template A (bus class):
 *
 *   1. One rtos_mutex per controller, held for the WHOLE transaction
 *      (new / scan / read* / write* / slave read|write), so two flows can
 *      never interleave on the same bus.  Created once in lua_driver_i2c_init()
 *      from the single-threaded boot phase, before any concurrent execution.
 *   2. Reference counting on init/deinit: the first handle on a controller
 *      runs I2C_Init(); the config slot is released only when the last handle
 *      is garbage-collected (refcnt == 0).  The peripheral clock is left on
 *      permanently — disabling it could cut off a controller another flow is
 *      about to open, and re-enabling it costs nothing (CONC-03).
 *   3. Conflicting config is rejected: while a controller is live, opening it
 *      again with a different mode (master vs slave), frequency, pins or slave
 *      address raises an error instead of silently reusing the old config
 *      (CONC-04).  A compatible re-open just adds a reference.
 *
 * Critical-section purity (CONC-02 / red-line 1): all parameter validation and
 * memory allocation (lua_newuserdata) happen BEFORE rtos_mutex_take, and the
 * Lua result is built AFTER rtos_mutex_give.  Nothing that can longjmp
 * (luaL_error / luaL_check* / lua_newuserdata) is ever called while the lock is
 * held, otherwise the give would be skipped and the controller would deadlock.
 * -----------------------------------------------------------------------*/

/* I2C0_1_IPCLK expands to XTAL_ClkGet() which is not a compile-time constant */
#define S_I2C_IP_CLK() (I2C0_1_IPCLK)

#define I2C_DRV_LOG "I2CDRV"

/* Per-controller shared state (the single owner of all mutable config). */
typedef struct {
    rtos_mutex_t lock;       /* whole-transaction guard, one per controller */
    int          refcnt;     /* live bus/slave handles holding this controller */
    u8           inited;     /* 1 after I2C_Init; cleared when refcnt hits 0 */
    u8           is_slave;   /* current mode while inited: 1 slave, 0 master */
    u8           sda_pin;    /* configured SDA pad while inited */
    u8           scl_pin;    /* configured SCL pad while inited */
    u16          slave_addr; /* configured slave ACK address (slave mode) */
    u32          freq_hz;    /* configured clock while inited */
} lua_driver_i2c_ctrl_t;

static lua_driver_i2c_ctrl_t s_i2c_ctrl[LUA_DRIVER_I2C_NUM_CTRL];

static const u32 s_i2c_scl_pinmux[LUA_DRIVER_I2C_NUM_CTRL] = {
    PINMUX_FUNCTION_I2C0_SCL,
    PINMUX_FUNCTION_I2C1_SCL,
};

static const u32 s_i2c_sda_pinmux[LUA_DRIVER_I2C_NUM_CTRL] = {
    PINMUX_FUNCTION_I2C0_SDA,
    PINMUX_FUNCTION_I2C1_SDA,
};

typedef struct {
    I2C_TypeDef *dev;
    int          idx;
    int          closed;
    int          counted;  /* 1 if this handle holds a controller reference */
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
    int          counted;  /* 1 if this handle holds a controller reference */
} lua_driver_i2c_slave_ud_t;

/* ---- Controller lock helpers --------------------------------------------- */

/* Acquire a controller lock with a bounded wait.  On timeout the lock is NOT
 * held, so raising luaL_error here is safe (no give is owed).  Must only be
 * called before entering the critical section. */
static void lua_driver_i2c_lock(lua_State *L, int idx)
{
    if (rtos_mutex_take(s_i2c_ctrl[idx].lock,
                        LUA_DRIVER_I2C_LOCK_TIMEOUT_MS) != RTK_SUCCESS) {
        luaL_error(L, "i2c%d: controller busy (lock timeout)", idx);
    }
}

/* Drop one controller reference.  Runs only from __gc (Lua task context, never
 * ISR).  Uses an unconditional wait: if another task is mid-transaction and
 * holds the lock, we must still decrement refcnt correctly — skipping would
 * leak the reference and prevent the config slot from ever being reused.
 * Safe because all I/O paths that hold this lock are bounded: master ops use
 * LUA_DRIVER_I2C_LOCK_TIMEOUT_MS, and slave:read() without a caller-supplied
 * timeout uses LUA_DRIVER_I2C_SLAVE_READ_POLL_MS — so the lock is always
 * released within a finite time.  The GC task itself cannot be the lock holder
 * (critical-section purity: no longjmp-capable API inside the lock).
 * Touches no longjmp-capable API. */
static void lua_driver_i2c_ctrl_release(int idx)
{
    lua_driver_i2c_ctrl_t *c = &s_i2c_ctrl[idx];
    rtos_mutex_take(c->lock, RTOS_MAX_DELAY);
    if (c->refcnt > 0) {
        c->refcnt--;
    }
    int released = (c->refcnt == 0);
    if (released) {
        /* Last user gone: release the config slot so a later new() may pick a
         * different mode/speed/pins.  Clock stays on intentionally (CONC-03). */
        c->inited = 0;
    }
    rtos_mutex_give(c->lock);

    if (released) {
        RTK_LOGI(I2C_DRV_LOG, "I2C%d released\n", idx);
    }
}

/* Bounded C-return-code lock take (mirrors lua_driver_i2c_lock but never
 * longjmps).  For the shared C bus API and the open-ctrl core, which have no
 * lua_State to raise through.  Returns 0 or LUA_I2C_ERR_BUSY. */
static int i2c_lock_c(int idx)
{
    if (rtos_mutex_take(s_i2c_ctrl[idx].lock,
                        LUA_DRIVER_I2C_LOCK_TIMEOUT_MS) != RTK_SUCCESS) {
        return LUA_I2C_ERR_BUSY;
    }
    return LUA_I2C_OK;
}

/* Pure-C core of open/reference: no lua_State, returns an error code instead of
 * raising.  Shared by the Lua wrapper (lua_driver_i2c_open_ctrl) and the public
 * C bus API (lua_i2c_bus_acquire).  On success the controller is configured (if
 * first user), refcnt is incremented and the lock released. */
static int i2c_open_ctrl_c(int idx, int want_slave, u32 freq_hz, u16 slave_addr,
                           u8 sda_pin, u8 scl_pin)
{
    lua_driver_i2c_ctrl_t *c = &s_i2c_ctrl[idx];

    int rc = i2c_lock_c(idx);
    if (rc != LUA_I2C_OK) {
        return rc;
    }

    if (c->inited && c->refcnt > 0) {
        /* A live handle already owns this controller; its configuration must
         * match or reconfiguring would corrupt the in-flight bus. */
        int conflict = (c->is_slave != (u8)want_slave) ||
                       (c->freq_hz != freq_hz) ||
                       (c->sda_pin != sda_pin) ||
                       (c->scl_pin != scl_pin) ||
                       (want_slave && c->slave_addr != slave_addr);
        if (conflict) {
            rtos_mutex_give(c->lock);
            return LUA_I2C_ERR_CONFIG;
        }
        c->refcnt++;                   /* compatible: just add a reference */
        rtos_mutex_give(c->lock);
        return LUA_I2C_OK;
    }

    /* First (or sole) user: configure the hardware from scratch. */
    RCC_PeriphClockCmd(
        idx == 0 ? APBPeriph_I2C0 : APBPeriph_I2C1,
        idx == 0 ? APBPeriph_I2C0_CLOCK : APBPeriph_I2C1_CLOCK,
        ENABLE);

    Pinmux_Config(sda_pin, s_i2c_sda_pinmux[idx]);
    Pinmux_Config(scl_pin, s_i2c_scl_pinmux[idx]);
    PAD_PullCtrl(sda_pin, GPIO_PuPd_UP);
    PAD_PullCtrl(scl_pin, GPIO_PuPd_UP);

    I2C_InitTypeDef init;
    I2C_StructInit(&init);
    init.I2CIdx     = (u32)idx;
    init.I2CMaster  = want_slave ? I2C_SLAVE_MODE : I2C_MASTER_MODE;
    init.I2CAddrMod = I2C_ADDR_7BIT;
    if (want_slave) {
        init.I2CAckAddr = slave_addr;
    }
    init.I2CIPClk   = S_I2C_IP_CLK();
    init.I2CClk     = freq_hz / 1000;  /* fwlib field is in kHz */

    if (freq_hz <= LUA_DRIVER_I2C_SS_MAX_FREQ) {
        init.I2CSpdMod = I2C_SS_MODE;
    } else if (freq_hz <= LUA_DRIVER_I2C_FS_MAX_FREQ) {
        init.I2CSpdMod = I2C_FS_MODE;
    } else {
        init.I2CSpdMod = I2C_HS_MODE;
    }

    I2C_Init(I2C_DEV_TABLE[idx].I2Cx, &init);
    I2C_Cmd(I2C_DEV_TABLE[idx].I2Cx, ENABLE);

    c->inited     = 1;
    c->is_slave   = (u8)want_slave;
    c->freq_hz    = freq_hz;
    c->sda_pin    = sda_pin;
    c->scl_pin    = scl_pin;
    c->slave_addr = slave_addr;
    c->refcnt++;

    rtos_mutex_give(c->lock);

    char sda_str[8], scl_str[8];
    RTK_LOGI(I2C_DRV_LOG,
             "I2C%d initialized: sda=%s scl=%s freq=%dHz.\n",
             idx, luhw_pin_to_str((PinName)sda_pin, sda_str, sizeof(sda_str)),
             luhw_pin_to_str((PinName)scl_pin, scl_str, sizeof(scl_str)),
             (int)freq_hz);

    return LUA_I2C_OK;
}

/* Open / reference a controller from Lua.  Thin wrapper over i2c_open_ctrl_c
 * that maps error codes to luaL_error (does not return on error).  All
 * longjmp-capable work by the caller (userdata allocation) must already be done
 * before calling this. */
static void lua_driver_i2c_open_ctrl(lua_State *L, int idx, int want_slave,
                                     u32 freq_hz, u16 slave_addr,
                                     PinName sda_pin, PinName scl_pin)
{
    int rc = i2c_open_ctrl_c(idx, want_slave, freq_hz, slave_addr,
                             (u8)sda_pin, (u8)scl_pin);
    if (rc == LUA_I2C_ERR_BUSY) {
        luaL_error(L, "i2c%d: controller busy (lock timeout)", idx);
    } else if (rc == LUA_I2C_ERR_CONFIG) {
        luaL_error(L, "i2c%d: already open with a different "
                      "mode/frequency/pins", idx);
    }
}

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

/* Map a device's underlying peripheral pointer back to its controller index. */
static int lua_driver_i2c_dev_idx(I2C_TypeDef *dev)
{
    return (dev == I2C_DEV_TABLE[0].I2Cx) ? 0 : 1;
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
    if (freq_hz <= 0 || freq_hz > LUA_DRIVER_I2C_MAX_FREQ) {
        return luaL_error(L, "i2c freq_hz must be 1-%d", LUA_DRIVER_I2C_MAX_FREQ);
    }
    if (idx == 0 && freq_hz > LUA_DRIVER_I2C_FS_MAX_FREQ) {
        return luaL_error(L, "i2c0 only supports up to %d Hz",
                          LUA_DRIVER_I2C_FS_MAX_FREQ);
    }

    int i = (int)idx;

    /* Allocate and stamp the handle BEFORE touching the controller lock so no
     * longjmp-capable allocation happens inside the critical section. */
    lua_driver_i2c_bus_ud_t *ud = (lua_driver_i2c_bus_ud_t *)lua_newuserdata(
        L, sizeof(*ud));
    ud->dev     = I2C_DEV_TABLE[i].I2Cx;
    ud->idx     = i;
    ud->closed  = 0;
    ud->counted = 0;
    luaL_getmetatable(L, LUA_DRIVER_I2C_BUS_MT);
    lua_setmetatable(L, -2);

    /* Raises luaL_error (after releasing the lock) on a conflicting config. */
    lua_driver_i2c_open_ctrl(L, i, 0 /*master*/, (u32)freq_hz, 0,
                             sda_pin, scl_pin);
    ud->counted = 1;  /* refcnt was incremented for this handle */

    return 1;
}

/* ---- bus:scan() → table of detected 7-bit addresses ---- */
static int lua_driver_i2c_bus_scan(lua_State *L)
{
    lua_driver_i2c_bus_ud_t *ud = lua_driver_i2c_bus_get_ud(L, 1);

    /* Collect ACKing addresses into a plain C array inside the critical
     * section; the Lua table (which may allocate / longjmp) is built only
     * after the lock is released. */
    u8  found[LUA_DRIVER_I2C_ADDR_MAX];
    int count = 0;

    lua_driver_i2c_lock(L, ud->idx);
    for (int addr = 1; addr <= LUA_DRIVER_I2C_ADDR_MAX; addr++) {
        I2C_SetSlaveAddress(ud->dev, (u16)addr);
        s32 ret = I2C_MasterSendNullData_TimeOut(ud->dev, addr,
                                                 LUA_DRIVER_I2C_SCAN_TIMEOUT);
        if (ret == 0) {
            found[count++] = (u8)addr;
        }
    }
    rtos_mutex_give(s_i2c_ctrl[ud->idx].lock);

    lua_createtable(L, count, 0);
    for (int i = 0; i < count; i++) {
        lua_pushinteger(L, found[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* ---- bus:device(addr7) → device object ---- */
static int lua_driver_i2c_bus_device(lua_State *L)
{
    lua_driver_i2c_bus_ud_t *bus_ud = lua_driver_i2c_bus_get_ud(L, 1);
    lua_Integer              addr   = luaL_checkinteger(L, 2);

    if (addr < 0 || addr > LUA_DRIVER_I2C_ADDR_MAX) {
        return luaL_error(L, "i2c device addr must be 0-%d", LUA_DRIVER_I2C_ADDR_MAX);
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
    /* Only invalidate the Lua handle.  The controller reference is dropped at
     * __gc, not here, so a device still derived from this bus keeps the
     * controller configured until it too is collected. */
    ud->closed = 1;
    return 0;
}

static int lua_driver_i2c_bus_gc(lua_State *L)
{
    lua_driver_i2c_bus_ud_t *ud = (lua_driver_i2c_bus_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_I2C_BUS_MT);
    if (ud) {
        if (ud->counted) {
            lua_driver_i2c_ctrl_release(ud->idx);
            ud->counted = 0;
        }
        ud->closed = 1;
    }
    return 0;
}

/* ---- dev:read_byte([mem_addr]) → integer ---- */
static int lua_driver_i2c_dev_read_byte(lua_State *L)
{
    lua_driver_i2c_dev_ud_t *ud = lua_driver_i2c_dev_get_ud(L, 1);
    u8 result = 0;

    int i = lua_driver_i2c_dev_idx(ud->dev);

    int has_mem = !lua_isnoneornil(L, 2);
    u8  mem     = 0;
    if (has_mem) {
        lua_Integer m = luaL_checkinteger(L, 2);
        if (m < 0 || m > LUA_DRIVER_I2C_MEM_MAX) {
            return luaL_error(L, "i2c mem_addr must be 0-%d", LUA_DRIVER_I2C_MEM_MAX);
        }
        mem = (u8)m;
    }

    lua_driver_i2c_lock(L, i);
    I2C_SetSlaveAddress(ud->dev, ud->addr);
    if (has_mem) {
        I2C_MasterRepeatRead(ud->dev, &mem, 1, &result, 1);
    } else {
        I2C_MasterRead(ud->dev, &result, 1);
    }
    rtos_mutex_give(s_i2c_ctrl[i].lock);

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
        if (m < 0 || m > LUA_DRIVER_I2C_MEM_MAX) {
            return luaL_error(L, "i2c mem_addr must be 0-%d", LUA_DRIVER_I2C_MEM_MAX);
        }
        mem = (u8)m;
    }

    u8 *buf = (u8 *)lua_newuserdata(L, (size_t)len);
    int i = lua_driver_i2c_dev_idx(ud->dev);

    lua_driver_i2c_lock(L, i);
    I2C_SetSlaveAddress(ud->dev, ud->addr);
    if (has_mem) {
        I2C_MasterRepeatRead(ud->dev, &mem, 1, buf, (u32)len);
    } else {
        I2C_MasterRead(ud->dev, buf, (u32)len);
    }
    rtos_mutex_give(s_i2c_ctrl[i].lock);

    lua_pushlstring(L, (const char *)buf, (size_t)len);
    return 1;
}

/* ---- dev:write_byte(value [, mem_addr]) ---- */
static int lua_driver_i2c_dev_write_byte(lua_State *L)
{
    lua_driver_i2c_dev_ud_t *ud  = lua_driver_i2c_dev_get_ud(L, 1);
    lua_Integer              val = luaL_checkinteger(L, 2);

    if (val < 0 || val > LUA_DRIVER_I2C_MEM_MAX) {
        return luaL_error(L, "i2c write_byte value must be 0-%d", LUA_DRIVER_I2C_MEM_MAX);
    }

    int has_mem = !lua_isnoneornil(L, 3);
    u8  m       = 0;
    if (has_mem) {
        lua_Integer mv = luaL_checkinteger(L, 3);
        if (mv < 0 || mv > LUA_DRIVER_I2C_MEM_MAX) {
            return luaL_error(L, "i2c mem_addr must be 0-%d", LUA_DRIVER_I2C_MEM_MAX);
        }
        m = (u8)mv;
    }

    int i = lua_driver_i2c_dev_idx(ud->dev);
    u8  buf[2];
    u32 n;
    if (has_mem) {
        buf[0] = m;
        buf[1] = (u8)val;
        n = 2;
    } else {
        buf[0] = (u8)val;
        n = 1;
    }

    lua_driver_i2c_lock(L, i);
    I2C_SetSlaveAddress(ud->dev, ud->addr);
    I2C_MasterWrite(ud->dev, buf, n);
    rtos_mutex_give(s_i2c_ctrl[i].lock);

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
        if (m < 0 || m > LUA_DRIVER_I2C_MEM_MAX) {
            return luaL_error(L, "i2c mem_addr must be 0-%d", LUA_DRIVER_I2C_MEM_MAX);
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
            if (byte < 0 || byte > LUA_DRIVER_I2C_MEM_MAX) {
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

    /* Build the final on-wire buffer BEFORE locking: a mem_addr write needs a
     * fresh allocation, which must never happen inside the critical section
     * (longjmp-on-OOM would skip the give and deadlock the controller). */
    const u8 *wire     = raw_data;
    u32       wire_len = (u32)raw_len;
    if (has_mem_addr) {
        u8 *combined = (u8 *)lua_newuserdata(L, raw_len + 1);
        combined[0] = mem_addr_byte;
        if (raw_len > 0) {
            memcpy(combined + 1, raw_data, raw_len);
        }
        wire     = combined;
        wire_len = (u32)(raw_len + 1);
    }

    if (wire_len == 0) {
        return 0;  /* nothing to send (empty payload, no mem_addr) */
    }

    int i = lua_driver_i2c_dev_idx(ud->dev);

    lua_driver_i2c_lock(L, i);
    I2C_SetSlaveAddress(ud->dev, ud->addr);
    I2C_MasterWrite(ud->dev, (u8 *)wire, wire_len);
    rtos_mutex_give(s_i2c_ctrl[i].lock);

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
    if (addr < 0 || addr > LUA_DRIVER_I2C_ADDR_MAX) {
        return luaL_error(L, "i2c slave addr must be 0-%d", LUA_DRIVER_I2C_ADDR_MAX);
    }
    if (freq_hz <= 0 || freq_hz > LUA_DRIVER_I2C_MAX_FREQ) {
        return luaL_error(L, "i2c freq_hz must be 1-%d", LUA_DRIVER_I2C_MAX_FREQ);
    }
    if (idx == 0 && freq_hz > LUA_DRIVER_I2C_FS_MAX_FREQ) {
        return luaL_error(L, "i2c0 only supports up to %d Hz",
                          LUA_DRIVER_I2C_FS_MAX_FREQ);
    }

    int i = (int)idx;

    /* Allocate the handle before locking the controller (CONC-02). */
    lua_driver_i2c_slave_ud_t *ud = (lua_driver_i2c_slave_ud_t *)lua_newuserdata(
        L, sizeof(*ud));
    ud->dev     = I2C_DEV_TABLE[i].I2Cx;
    ud->idx     = i;
    ud->addr    = (u16)addr;
    ud->closed  = 0;
    ud->counted = 0;
    luaL_getmetatable(L, LUA_DRIVER_I2C_SLAVE_MT);
    lua_setmetatable(L, -2);

    lua_driver_i2c_open_ctrl(L, i, 1 /*slave*/, (u32)freq_hz, (u16)addr,
                             sda_pin, scl_pin);
    ud->counted = 1;

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

    lua_driver_i2c_lock(L, ud->idx);
    u32 got;
    if (has_timeout) {
        got = I2C_SlaveReadTimeOut(ud->dev, buf, (u32)len, timeout_ms);
    } else {
        /* Use a bounded poll window instead of I2C_SlaveRead (infinite block).
         * Holding the mutex across an unbounded wait causes deadlock if another
         * task's GC calls ctrl_release(RTOS_MAX_DELAY) on the same controller.
         * Callers wanting "wait forever" should loop until #result > 0. */
        got = I2C_SlaveReadTimeOut(ud->dev, buf, (u32)len,
                                   LUA_DRIVER_I2C_SLAVE_READ_POLL_MS);
    }
    rtos_mutex_give(s_i2c_ctrl[ud->idx].lock);

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
            if (byte < 0 || byte > LUA_DRIVER_I2C_MEM_MAX) {
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

    lua_driver_i2c_lock(L, ud->idx);
    u32 sent = 0;
    if (raw_len > 0) {
        sent = I2C_SlaveWrite(ud->dev, (u8 *)raw_data, (u32)raw_len);
    }
    rtos_mutex_give(s_i2c_ctrl[ud->idx].lock);

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

/* ---- slave:close() ---- */
static int lua_driver_i2c_slave_close(lua_State *L)
{
    lua_driver_i2c_slave_ud_t *ud = (lua_driver_i2c_slave_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_I2C_SLAVE_MT);
    /* Invalidate the handle only; the controller reference is released at __gc
     * (the clock is intentionally never powered down — see CONC-03). */
    ud->closed = 1;
    return 0;
}

static int lua_driver_i2c_slave_gc(lua_State *L)
{
    lua_driver_i2c_slave_ud_t *ud = (lua_driver_i2c_slave_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_I2C_SLAVE_MT);
    if (ud) {
        if (ud->counted) {
            lua_driver_i2c_ctrl_release(ud->idx);
            ud->counted = 0;
        }
        ud->closed = 1;
    }
    return 0;
}

/* ================================================================== */
/* Shared C-level master-bus API (see lua_driver_i2c.h)               */
/*                                                                    */
/* Same locks / refcount / config slot as the Lua i2c device API, so  */
/* a C module (e.g. an IMU backend) and Lua i2c code can never        */
/* interleave transactions on one physical controller.               */
/* ================================================================== */

int lua_i2c_bus_acquire(int idx, uint8_t sda, uint8_t scl, uint32_t freq_hz)
{
    if (idx < 0 || idx >= LUA_DRIVER_I2C_NUM_CTRL) {
        return LUA_I2C_ERR_ARG;
    }
    if (freq_hz == 0 || freq_hz > LUA_DRIVER_I2C_MAX_FREQ) {
        return LUA_I2C_ERR_ARG;
    }
    if (idx == 0 && freq_hz > LUA_DRIVER_I2C_FS_MAX_FREQ) {
        return LUA_I2C_ERR_ARG;  /* i2c0 caps at fast mode */
    }
    return i2c_open_ctrl_c(idx, 0 /*master*/, freq_hz, 0, sda, scl);
}

void lua_i2c_bus_release(int idx)
{
    if (idx < 0 || idx >= LUA_DRIVER_I2C_NUM_CTRL) {
        return;
    }
    lua_driver_i2c_ctrl_release(idx);
}

int lua_i2c_bus_write_regs(int idx, uint16_t addr, const uint8_t *buf, uint32_t len)
{
    if (idx < 0 || idx >= LUA_DRIVER_I2C_NUM_CTRL || buf == NULL ||
        len == 0 || len > LUA_DRIVER_I2C_MAX_DATA) {
        return LUA_I2C_ERR_ARG;
    }

    int rc = i2c_lock_c(idx);
    if (rc != LUA_I2C_OK) {
        return rc;
    }
    I2C_TypeDef *dev = I2C_DEV_TABLE[idx].I2Cx;
    I2C_SetSlaveAddress(dev, addr);
    I2C_MasterWrite(dev, (u8 *)buf, len);
    rtos_mutex_give(s_i2c_ctrl[idx].lock);
    return LUA_I2C_OK;
}

int lua_i2c_bus_read_regs(int idx, uint16_t addr, uint8_t reg, uint8_t *buf, uint32_t len)
{
    if (idx < 0 || idx >= LUA_DRIVER_I2C_NUM_CTRL || buf == NULL ||
        len == 0 || len > LUA_DRIVER_I2C_MAX_DATA) {
        return LUA_I2C_ERR_ARG;
    }

    int rc = i2c_lock_c(idx);
    if (rc != LUA_I2C_OK) {
        return rc;
    }
    I2C_TypeDef *dev = I2C_DEV_TABLE[idx].I2Cx;
    u8 mem = reg;
    I2C_SetSlaveAddress(dev, addr);
    I2C_MasterRepeatRead(dev, &mem, 1, buf, len);
    rtos_mutex_give(s_i2c_ctrl[idx].lock);
    return LUA_I2C_OK;
}

/* ---- Driver-level init (called once from lua_module_registry_provision_all,
 *      single-threaded boot phase, before any concurrent Lua execution). ---- */
void lua_driver_i2c_init(void)
{
    for (int i = 0; i < LUA_DRIVER_I2C_NUM_CTRL; i++) {
        if (s_i2c_ctrl[i].lock == NULL) {
            rtos_mutex_create(&s_i2c_ctrl[i].lock);
        }
        s_i2c_ctrl[i].refcnt     = 0;
        s_i2c_ctrl[i].inited     = 0;
        s_i2c_ctrl[i].is_slave   = 0;
        s_i2c_ctrl[i].sda_pin    = 0;
        s_i2c_ctrl[i].scl_pin    = 0;
        s_i2c_ctrl[i].slave_addr = 0;
        s_i2c_ctrl[i].freq_hz    = 0;
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

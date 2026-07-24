/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_driver_adc.h"

#include "ameba_soc.h"
#include "lauxlib.h"
#include "luhw.h"
#include "os_wrapper.h"

#define LUA_DRIVER_ADC_METATABLE    "adc.channel"
#define LUA_DRIVER_ADC_READ_TIMEOUT 10000
#define LUA_DRIVER_ADC_AUTO_MAX     256
#define LUA_DRIVER_ADC_PINS_MAX     ADC_CH_NUM  /* 12: 8 external + 4 internal */

/*
 * One handle may own 1..PINS_MAX channels, all initialised together
 * (mirroring the ameba_adc.c multi-channel init pattern).
 */
typedef struct {
    u8  channels[LUA_DRIVER_ADC_PINS_MAX]; /* hardware channel IDs */
    u8  pins[LUA_DRIVER_ADC_PINS_MAX];     /* PinName values        */
    int n_ch;
    int closed;
    u8  op_mode; /* ADC_SW_TRI_MODE or ADC_AUTO_MODE */
} lua_driver_adc_ud_t;

/* Fixed mapping: GPIO pin -> ADC hardware channel (RTL8721F) */
static const struct { u8 pin; u8 ch; } s_pin_ch_map[] = {
    { _PA_20, 0 }, { _PA_19, 1 }, { _PA_18, 2 }, { _PA_17, 3 },
    { _PA_15, 4 }, { _PA_14, 5 }, { _PA_13, 6 }, { _PA_12, 7 },
};
#define PIN_CH_MAP_SIZE  (sizeof(s_pin_ch_map) / sizeof(s_pin_ch_map[0]))

static rtos_mutex_t s_adc_lock;
/*
 * The ADC is a single hardware instance: one global op_mode and one channel
 * list, configured once per open. Only one handle may own it at a time, so
 * this is an exclusive 0/1 guard (1 = a handle is open), not a real refcount.
 */
static int          s_adc_refcount;

static int pin_to_channel(u8 pin)
{
    for (size_t i = 0; i < PIN_CH_MAP_SIZE; i++) {
        if (s_pin_ch_map[i].pin == pin) {
            return (int)s_pin_ch_map[i].ch;
        }
    }
    return -1;
}

static lua_driver_adc_ud_t *lua_driver_adc_get_ud(lua_State *L, int idx)
{
    lua_driver_adc_ud_t *ud = (lua_driver_adc_ud_t *)luaL_checkudata(
        L, idx, LUA_DRIVER_ADC_METATABLE);
    if (!ud || ud->closed) {
        luaL_error(L, "adc: invalid or closed channel");
    }
    return ud;
}

/*
 * Common hardware init.
 *
 * Enables RCC, calls ADC_Init with the requested op_mode and channel list,
 * then enables the ADC
 * Caller must hold s_adc_lock.
 */
static void adc_hw_init(u8 op_mode, u8 *channels, int n)
{
    ADC_InitTypeDef ADC_InitStruct;
    RCC_PeriphClockCmd(APBPeriph_ADC, APBPeriph_ADC_CLOCK, ENABLE);
    ADC_StructInit(&ADC_InitStruct);
    ADC_InitStruct.ADC_OpMode    = op_mode;
    ADC_InitStruct.ADC_CvlistLen = (u8)(n - 1);
    for (int i = 0; i < n; i++) {
        ADC_InitStruct.ADC_Cvlist[i] = channels[i];
    }
    ADC_Init(&ADC_InitStruct);
    ADC_Cmd(ENABLE);
}

/*
 * adc.new(pin1 [, pin2, ...] [, "auto"]) -> channel handle
 *
 * Accepts 1..PINS_MAX pin arguments followed by an optional mode string:
 *   "sw"   (default) -> ADC_SW_TRI_MODE, use ch:read() / ch:trigger()
 *   "auto"           -> ADC_AUTO_MODE,   use ch:read_auto()
 */
static int lua_driver_adc_new(lua_State *L)
{
    int n = lua_gettop(L);

    u8 op_mode = ADC_SW_TRI_MODE;
    if (n >= 1 && lua_type(L, n) == LUA_TSTRING) {
        const char *last = lua_tostring(L, n);
        if (strcmp(last, "auto") == 0) {
            op_mode = ADC_AUTO_MODE;
            n--;  /* mode string consumed; remaining args are pins */
        } else if (strcmp(last, "sw") == 0) {
            n--;  /* explicit SW -> consume and keep default */
        }
        /* anything else (e.g. a pin name) -> leave n unchanged */
    }

    if (n < 1 || n > LUA_DRIVER_ADC_PINS_MAX) {
        return luaL_error(L, "adc.new: expected 1..%d pin arguments",
                          LUA_DRIVER_ADC_PINS_MAX);
    }

    u8 pins[LUA_DRIVER_ADC_PINS_MAX];
    u8 channels[LUA_DRIVER_ADC_PINS_MAX];

    for (int i = 0; i < n; i++) {
        PinName pin = luhw_check_pin(L, i + 1);
        int ch = pin_to_channel((u8)pin);
        if (ch < 0) {
            /* %d not %x: luaL_error's lua_pushfstring rejects %x/%02x (it would
             * raise "invalid option '%x'..." instead of this message). */
            return luaL_error(L, "adc: pin %d is not ADC-capable", (int)(u8)pin);
        }
        pins[i]     = (u8)pin;
        channels[i] = (u8)ch;
    }

    rtos_mutex_take(s_adc_lock, MUTEX_WAIT_TIMEOUT);

    /*
     * Single hardware instance, single global config: a second concurrent open
     * would silently serve the first handle's mode/channel list. Reject it
     * loudly instead. Do this before touching any pin state so a rejected
     * open leaves the pads untouched. (give the lock first — luaL_error
     * longjmps out and would otherwise leak the mutex.)
     */
    if (s_adc_refcount > 0) {
        rtos_mutex_give(s_adc_lock);
        return luaL_error(L, "adc: another channel is open; close it first");
    }

    for (int i = 0; i < n; i++) {
        Pinmux_Config(pins[i], PINMUX_FUNCTION_ADC);
        PAD_PullCtrl(pins[i], GPIO_PuPd_NOPULL);
        PAD_SleepPullCtrl(pins[i], GPIO_PuPd_NOPULL);
        PAD_InputCtrl(pins[i], DISABLE);
    }

    adc_hw_init(op_mode, channels, n);
    s_adc_refcount++;

    rtos_mutex_give(s_adc_lock);

    lua_driver_adc_ud_t *ud = (lua_driver_adc_ud_t *)lua_newuserdata(
        L, sizeof(*ud));
    for (int i = 0; i < n; i++) {
        ud->channels[i] = channels[i];
        ud->pins[i]     = pins[i];
    }
    ud->n_ch    = n;
    ud->closed  = 0;
    ud->op_mode = op_mode;
    luaL_getmetatable(L, LUA_DRIVER_ADC_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

/*
 * read() -> {[ch_id]=mv, ...}
 *
 * One SW trigger converts all channels in the list (mirrors adc_swtrig_demo).
 * Returns a table keyed by hardware channel ID.
 */
static int lua_driver_adc_read(lua_State *L)
{
    lua_driver_adc_ud_t *ud = lua_driver_adc_get_ud(L, 1);

    rtos_mutex_take(s_adc_lock, MUTEX_WAIT_TIMEOUT);

    ADC_SWTrigCmd(ENABLE);
    int retries = LUA_DRIVER_ADC_READ_TIMEOUT;
    while (ADC_Readable() == 0 && retries-- > 0) {
        ;
    }
    ADC_SWTrigCmd(DISABLE);

    u32 buf[LUA_DRIVER_ADC_PINS_MAX];
    int got = 0;
    while (ADC_Readable() && got < ud->n_ch) {
        buf[got++] = ADC_Read();
    }

    rtos_mutex_give(s_adc_lock);

    if (got == 0) {
        return luaL_error(L, "adc read timeout");
    }

    lua_createtable(L, 0, got);
    for (int i = 0; i < got; i++) {
        u32 ch_id = ADC_GET_CH_NUM_GLOBAL(buf[i]);
        u32 data  = ADC_GET_DATA_GLOBAL(buf[i]);
        s32 mv    = ADC_GetVoltage(data);
        lua_pushinteger(L, (lua_Integer)mv);
        lua_rawseti(L, -2, (lua_Integer)ch_id);
    }
    return 1;
}

/*
 * trigger([enable]) - start/stop the non-blocking SW conversion.
 *
 * trigger() or trigger(true) starts conversion; the hardware keeps filling the
 * FIFO while it is on. trigger(false) stops it, keeping any samples already in
 * the FIFO for read_raw() to drain. Flow: trigger() -> poll readable() ->
 * trigger(false) -> drain with read_raw().
 */
static int lua_driver_adc_trigger(lua_State *L)
{
    lua_driver_adc_get_ud(L, 1);
    /* argument omitted -> default ENABLE */
    u32 state = lua_isnoneornil(L, 2) ? ENABLE
                                      : (lua_toboolean(L, 2) ? ENABLE : DISABLE);

    rtos_mutex_take(s_adc_lock, MUTEX_WAIT_TIMEOUT);
    ADC_SWTrigCmd(state);
    rtos_mutex_give(s_adc_lock);

    return 0;
}

/*
 * readable() -> bool - true when at least one sample is available in FIFO.
 */
static int lua_driver_adc_readable(lua_State *L)
{
    lua_driver_adc_get_ud(L, 1);
    lua_pushboolean(L, ADC_Readable() ? 1 : 0);
    return 1;
}

/*
 * read_raw() -> ch_id, raw | nil - pop one sample from FIFO.
 *
 * Returns the hardware channel ID and the raw 16-bit code (no mV conversion).
 * Returns nil when the FIFO is empty.
 */
static int lua_driver_adc_read_raw(lua_State *L)
{
    lua_driver_adc_get_ud(L, 1);

    rtos_mutex_take(s_adc_lock, MUTEX_WAIT_TIMEOUT);
    int ready = ADC_Readable();
    u32 val = ready ? ADC_Read() : 0;
    rtos_mutex_give(s_adc_lock);

    if (!ready) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushinteger(L, (lua_Integer)ADC_GET_CH_NUM_GLOBAL(val));
    lua_pushinteger(L, (lua_Integer)ADC_GET_DATA_GLOBAL(val));
    return 2;
}

static void lua_driver_adc_do_close(lua_driver_adc_ud_t *ud)
{
    if (ud->closed) {
        return;
    }
    ud->closed = 1;

    for (int i = 0; i < ud->n_ch; i++) {
        Pinmux_Config(ud->pins[i], PINMUX_FUNCTION_GPIO);
        PAD_PullCtrl(ud->pins[i], GPIO_PuPd_NOPULL);
        PAD_InputCtrl(ud->pins[i], ENABLE);
    }

    rtos_mutex_take(s_adc_lock, MUTEX_WAIT_TIMEOUT);
    if (s_adc_refcount > 0) {
        s_adc_refcount--;
    }
    if (s_adc_refcount == 0) {
        ADC_Cmd(DISABLE);
        RCC_PeriphClockCmd(APBPeriph_ADC, APBPeriph_ADC_CLOCK, DISABLE);
    }
    rtos_mutex_give(s_adc_lock);
}

static int lua_driver_adc_close(lua_State *L)
{
    lua_driver_adc_ud_t *ud = (lua_driver_adc_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_ADC_METATABLE);
    lua_driver_adc_do_close(ud);
    return 0;
}

static int lua_driver_adc_gc(lua_State *L)
{
    lua_driver_adc_ud_t *ud = (lua_driver_adc_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_ADC_METATABLE);
    if (ud) {
        lua_driver_adc_do_close(ud);
    }
    return 0;
}

/*
 * ch:read_auto(count) -> array of {ch=hw_channel_id, mv=millivolts}
 *
 * Requires the handle to be opened with adc.new(..., "auto").
 * Hardware is already in AUTO mode from adc_hw_init() in adc.new() — no re-init.
 */
static int lua_driver_adc_read_auto(lua_State *L)
{
    lua_driver_adc_ud_t *ud = lua_driver_adc_get_ud(L, 1);
    lua_Integer count = luaL_checkinteger(L, 2);

    if (count < 1 || count > LUA_DRIVER_ADC_AUTO_MAX) {
        return luaL_error(L, "adc: read_auto count must be 1..%d",
                          LUA_DRIVER_ADC_AUTO_MAX);
    }

    if (ud->op_mode != ADC_AUTO_MODE) {
        return luaL_error(L, "adc: read_auto requires a handle opened with 'auto' mode");
    }

    rtos_mutex_take(s_adc_lock, MUTEX_WAIT_TIMEOUT);

    /* Hardware already in AUTO mode from adc_hw_init() in adc.new() — no re-init */

    /* Drain stale FIFO entries left over from a previous acquisition */
    while (ADC_Readable()) {
        ADC_Read();
    }

    u32 buf[LUA_DRIVER_ADC_AUTO_MAX];
    int collected = 0;

    ADC_AutoCSwCmd(ENABLE);
    for (int i = 0; i < (int)count; i++) {
        int retries = LUA_DRIVER_ADC_READ_TIMEOUT;
        while (!ADC_Readable() && retries-- > 0) {
            ;
        }
        if (ADC_Readable()) {
            buf[collected++] = ADC_Read();
        } else {
            break;
        }
    }
    ADC_AutoCSwCmd(DISABLE);

    rtos_mutex_give(s_adc_lock);

    lua_createtable(L, collected, 0);
    for (int i = 0; i < collected; i++) {
        u32 ch_id = ADC_GET_CH_NUM_GLOBAL(buf[i]);
        u32 data  = ADC_GET_DATA_GLOBAL(buf[i]);
        s32 mv    = ADC_GetVoltage(data);
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, (lua_Integer)ch_id);
        lua_setfield(L, -2, "ch");
        lua_pushinteger(L, (lua_Integer)mv);
        lua_setfield(L, -2, "mv");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int luaopen_adc(lua_State *L)
{
    if (!s_adc_lock) {
        rtos_mutex_create(&s_adc_lock);
    }

    if (luaL_newmetatable(L, LUA_DRIVER_ADC_METATABLE)) {
        lua_pushcfunction(L, lua_driver_adc_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_driver_adc_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_driver_adc_trigger);
        lua_setfield(L, -2, "trigger");
        lua_pushcfunction(L, lua_driver_adc_readable);
        lua_setfield(L, -2, "readable");
        lua_pushcfunction(L, lua_driver_adc_read_raw);
        lua_setfield(L, -2, "read_raw");
        lua_pushcfunction(L, lua_driver_adc_close);
        lua_setfield(L, -2, "close");
        lua_pushcfunction(L, lua_driver_adc_read_auto);
        lua_setfield(L, -2, "read_auto");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_driver_adc_new);
    lua_setfield(L, -2, "new");
    return 1;
}

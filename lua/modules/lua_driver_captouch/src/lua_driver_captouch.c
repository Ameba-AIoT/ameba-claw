/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_driver_captouch.h"

#include <string.h>

#include "ameba_soc.h"
#include "lauxlib.h"
#include "luhw.h"
#include "os_wrapper.h"

#define LUA_DRIVER_CAPTOUCH_METATABLE       "captouch.device"
#define LUA_DRIVER_CAPTOUCH_DEFAULT_NAME    "touch_keys"
#define LUA_DRIVER_CAPTOUCH_MAX_NAME_LEN    64
#define LUA_DRIVER_CAPTOUCH_MAX_KEYS        2

/* Recommended values for RTL8721F (from SDK example) */
#define LUA_DRIVER_CAPTOUCH_DEFAULT_THRESHOLD   1600u
#define LUA_DRIVER_CAPTOUCH_DEFAULT_MBIAS       0x800u
#define LUA_DRIVER_CAPTOUCH_DEFAULT_NOISE_THR   800u
/* Default scan interval: 60 counts * (1/1.024 kHz) ≈ 58.6 ms */
#define LUA_DRIVER_CAPTOUCH_DEFAULT_INTERVAL_MS 60u

typedef struct {
    u8  channel[LUA_DRIVER_CAPTOUCH_MAX_KEYS];
    u8  pin[LUA_DRIVER_CAPTOUCH_MAX_KEYS];
    u32 threshold[LUA_DRIVER_CAPTOUCH_MAX_KEYS];
    int key_count;
    char name[LUA_DRIVER_CAPTOUCH_MAX_NAME_LEN];
    int closed;
} lua_driver_captouch_ud_t;

/* Fixed pin-to-channel mapping (RTL8721F) */
static const struct {
    u8 pin;
    u8 ch;
} s_pin_ch_map[] = {
    { _PA_20, 0 },
    { _PA_19, 1 },
    { _PA_18, 2 },
    { _PA_17, 3 },
    { _PA_16, 4 },
    { _PA_24, 5 },
    { _PA_23, 6 },
    { _PA_22, 7 },
    { _PA_21, 8 },
};

#define PIN_CH_MAP_SIZE  (sizeof(s_pin_ch_map) / sizeof(s_pin_ch_map[0]))

static rtos_mutex_t s_ctc_lock;
static int          s_ctc_refcount;
static int          s_ctc_initialized;

static int pin_to_ctc_channel(u8 pin)
{
    for (size_t i = 0; i < PIN_CH_MAP_SIZE; i++) {
        if (s_pin_ch_map[i].pin == pin) {
            return (int)s_pin_ch_map[i].ch;
        }
    }
    return -1;
}

/* CT_SCAN_PERIOD unit = 1.024 kHz cycle (~0.977 ms). reg = ms * 1024 / 1000, clamped 1..4095. */
static u32 interval_ms_to_reg(u32 ms)
{
    u32 reg = (ms * 1024u + 500u) / 1000u;
    if (reg < 1u)    reg = 1u;
    if (reg > 4095u) reg = 4095u;
    return reg;
}

static lua_driver_captouch_ud_t *lua_driver_captouch_get_ud(lua_State *L, int idx)
{
    lua_driver_captouch_ud_t *ud = (lua_driver_captouch_ud_t *)luaL_checkudata(
        L, idx, LUA_DRIVER_CAPTOUCH_METATABLE);
    if (!ud || ud->closed) {
        luaL_error(L, "captouch: invalid or closed handle");
    }
    return ud;
}

static void lua_driver_captouch_enable_channel(u8 ch, u32 threshold,
                                            u16 mbias, u16 n_noise_thr, u16 p_noise_thr)
{
    CapTouch_SetChDiffThres(CAPTOUCH_DEV, ch, threshold);
    CapTouch_SetChMbias(CAPTOUCH_DEV, ch, mbias);
    CapTouch_SetNNoiseThres(CAPTOUCH_DEV, ch, n_noise_thr);
    CapTouch_SetPNoiseThres(CAPTOUCH_DEV, ch, p_noise_thr);
    CapTouch_ChCmd(CAPTOUCH_DEV, ch, ENABLE);
}

static int lua_driver_captouch_new(lua_State *L)
{
    /* Arg parsing:
     *   captouch.new(pin)
     *   captouch.new(pin1, pin2)
     *   captouch.new(pin, {threshold=N, name="..."})
     *   captouch.new(pin1, pin2, {threshold=N, name="..."}) */

    PinName pins[LUA_DRIVER_CAPTOUCH_MAX_KEYS];
    int key_count = 0;
    u32 threshold   = LUA_DRIVER_CAPTOUCH_DEFAULT_THRESHOLD;
    u16 mbias       = (u16)LUA_DRIVER_CAPTOUCH_DEFAULT_MBIAS;
    u16 n_noise_thr = (u16)LUA_DRIVER_CAPTOUCH_DEFAULT_NOISE_THR;
    u16 p_noise_thr = (u16)LUA_DRIVER_CAPTOUCH_DEFAULT_NOISE_THR;
    u32 interval_ms = LUA_DRIVER_CAPTOUCH_DEFAULT_INTERVAL_MS;
    /* Per-channel overrides (0 means use common value) */
    u16 ch_mbias[LUA_DRIVER_CAPTOUCH_MAX_KEYS]     = {0, 0};
    u32 ch_threshold[LUA_DRIVER_CAPTOUCH_MAX_KEYS] = {0, 0};
    const char *dev_name = LUA_DRIVER_CAPTOUCH_DEFAULT_NAME;

    pins[key_count++] = luhw_check_pin(L, 1);

    int next_arg = 2;
    int arg2_type = lua_type(L, 2);
    if (arg2_type == LUA_TSTRING || arg2_type == LUA_TNUMBER) {
        if (key_count < LUA_DRIVER_CAPTOUCH_MAX_KEYS) {
            pins[key_count++] = luhw_check_pin(L, 2);
        }
        next_arg = 3;
    }

    if (lua_istable(L, next_arg)) {
        lua_getfield(L, next_arg, "threshold");
        if (lua_isnumber(L, -1)) {
            lua_Integer t = luaL_checkinteger(L, -1);
            if (t <= 0 || t > 0xFFF) {
                return luaL_error(L, "captouch: threshold must be 1..4095");
            }
            threshold = (u32)t;
        }
        lua_pop(L, 1);

        lua_getfield(L, next_arg, "mbias");
        if (lua_isnumber(L, -1)) {
            lua_Integer v = luaL_checkinteger(L, -1);
            luaL_argcheck(L, v >= 0 && v <= 0xFFFF, next_arg, "mbias out of range 0..65535");
            mbias = (u16)v;
        }
        lua_pop(L, 1);

        lua_getfield(L, next_arg, "n_noise_thr");
        if (lua_isnumber(L, -1)) {
            lua_Integer v = luaL_checkinteger(L, -1);
            luaL_argcheck(L, v >= 0 && v <= 0xFFF, next_arg, "n_noise_thr out of range 0..4095");
            n_noise_thr = (u16)v;
        }
        lua_pop(L, 1);

        lua_getfield(L, next_arg, "p_noise_thr");
        if (lua_isnumber(L, -1)) {
            lua_Integer v = luaL_checkinteger(L, -1);
            luaL_argcheck(L, v >= 0 && v <= 0xFFF, next_arg, "p_noise_thr out of range 0..4095");
            p_noise_thr = (u16)v;
        }
        lua_pop(L, 1);

        lua_getfield(L, next_arg, "interval_ms");
        if (lua_isnumber(L, -1)) {
            lua_Integer v = luaL_checkinteger(L, -1);
            luaL_argcheck(L, v >= 1 && v <= 4000, next_arg, "interval_ms out of range 1..4000");
            interval_ms = (u32)v;
        }
        lua_pop(L, 1);

        /* ch_threshold = {val1, val2, ...}: per-channel diff threshold by key order */
        lua_getfield(L, next_arg, "ch_threshold");
        if (lua_istable(L, -1)) {
            for (int i = 0; i < LUA_DRIVER_CAPTOUCH_MAX_KEYS; i++) {
                lua_rawgeti(L, -1, i + 1);
                if (lua_isnumber(L, -1)) {
                    lua_Integer v = lua_tointeger(L, -1);
                    luaL_argcheck(L, v > 0 && v <= 0xFFF, next_arg, "ch_threshold value must be 1..4095");
                    ch_threshold[i] = (u32)v;
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);

        /* ch_mbias = {val1, val2, ...}: per-channel mbias override by key order */
        lua_getfield(L, next_arg, "ch_mbias");
        if (lua_istable(L, -1)) {
            for (int i = 0; i < LUA_DRIVER_CAPTOUCH_MAX_KEYS; i++) {
                lua_rawgeti(L, -1, i + 1);
                if (lua_isnumber(L, -1)) {
                    lua_Integer v = lua_tointeger(L, -1);
                    luaL_argcheck(L, v >= 0 && v <= 0xFFFF, next_arg, "ch_mbias value out of range");
                    ch_mbias[i] = (u16)v;
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);

        lua_getfield(L, next_arg, "name");
        if (lua_isstring(L, -1)) {
            dev_name = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
    }

    /* Resolve and validate channels */
    int channels[LUA_DRIVER_CAPTOUCH_MAX_KEYS];
    for (int i = 0; i < key_count; i++) {
        channels[i] = pin_to_ctc_channel((u8)pins[i]);
        if (channels[i] < 0) {
            /* %d not %x: luaL_error's lua_pushfstring rejects %x/%02x (it would
             * raise "invalid option '%x'..." instead of this message). */
            return luaL_error(L, "captouch: pin %d is not a CapTouch-capable pin", (int)(u8)pins[i]);
        }
    }

    /* Configure pads */
    for (int i = 0; i < key_count; i++) {
        Pinmux_Config((u8)pins[i], PINMUX_FUNCTION_CAP_TOUCH);
        PAD_PullCtrl((u8)pins[i], GPIO_PuPd_NOPULL);
        PAD_SleepPullCtrl((u8)pins[i], GPIO_PuPd_NOPULL);
        PAD_InputCtrl((u8)pins[i], DISABLE);
    }

    rtos_mutex_take(s_ctc_lock, MUTEX_WAIT_TIMEOUT);

    if (!s_ctc_initialized) {
        /* CapTouch uses the ADC clock */
        RCC_PeriphClockCmd(APBPeriph_ADC, APBPeriph_ADC_CLOCK, ENABLE);

        CapTouch_InitTypeDef init;
        CapTouch_StructInit(&init);
        init.CT_ScanInterval = interval_ms_to_reg(interval_ms);
        /* Configure requested channels in the init struct.
         * All channel params are committed here before Cmd(ENABLE), avoiding
         * post-init set_* calls that each trigger Cmd(DISABLE)+Cmd(ENABLE) and
         * would corrupt the baseline initialization. */
        for (int i = 0; i < key_count; i++) {
            u8 ch = (u8)channels[i];
            init.CT_Channel[ch].CT_CHEnable     = ENABLE;
            init.CT_Channel[ch].CT_DiffThrehold  = (u16)(ch_threshold[i] ? ch_threshold[i] : threshold);
            init.CT_Channel[ch].CT_MbiasCurrent  = ch_mbias[i] ? ch_mbias[i] : mbias;
            init.CT_Channel[ch].CT_ETCNNoiseThr  = n_noise_thr;
            init.CT_Channel[ch].CT_ETCPNoiseThr  = p_noise_thr;
        }
        CapTouch_Init(CAPTOUCH_DEV, &init);
        /* Disable all interrupts — driver uses polling only */
        CapTouch_INTConfig(CAPTOUCH_DEV, CT_ALL_INT_EN, DISABLE);
        CapTouch_Cmd(CAPTOUCH_DEV, ENABLE);
        s_ctc_initialized = 1;
    } else {
        /* CapTouch already running — enable additional channels dynamically */
        for (int i = 0; i < key_count; i++) {
            u32 ch_thr = ch_threshold[i] ? ch_threshold[i] : threshold;
            u16 ch_mb  = ch_mbias[i] ? ch_mbias[i] : mbias;
            lua_driver_captouch_enable_channel((u8)channels[i], ch_thr, ch_mb, n_noise_thr, p_noise_thr);
        }
    }
    s_ctc_refcount++;

    rtos_mutex_give(s_ctc_lock);

    /* Create Lua userdata */
    lua_driver_captouch_ud_t *ud = (lua_driver_captouch_ud_t *)lua_newuserdata(
        L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    ud->key_count = key_count;
    ud->closed    = 0;
    for (int i = 0; i < key_count; i++) {
        ud->channel[i]   = (u8)channels[i];
        ud->pin[i]       = (u8)pins[i];
        ud->threshold[i] = threshold;
    }
    if (strlen(dev_name) < LUA_DRIVER_CAPTOUCH_MAX_NAME_LEN) {
        strcpy(ud->name, dev_name);
    } else {
        strncpy(ud->name, dev_name, LUA_DRIVER_CAPTOUCH_MAX_NAME_LEN - 1);
        ud->name[LUA_DRIVER_CAPTOUCH_MAX_NAME_LEN - 1] = '\0';
    }

    luaL_getmetatable(L, LUA_DRIVER_CAPTOUCH_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

static void lua_driver_captouch_push_key_table(lua_State *L, int index, int channel,
                                            int pin, int pressed, u32 smooth,
                                            u32 benchmark, s32 delta, u32 threshold)
{
    lua_newtable(L);
    lua_pushinteger(L, index + 1);
    lua_setfield(L, -2, "index");
    lua_pushinteger(L, channel);
    lua_setfield(L, -2, "channel");
    lua_pushinteger(L, pin);
    lua_setfield(L, -2, "pin");
    lua_pushboolean(L, pressed);
    lua_setfield(L, -2, "pressed");
    lua_pushinteger(L, (lua_Integer)smooth);
    lua_setfield(L, -2, "smooth");
    lua_pushinteger(L, (lua_Integer)benchmark);
    lua_setfield(L, -2, "benchmark");
    lua_pushinteger(L, (lua_Integer)delta);
    lua_setfield(L, -2, "delta");
    lua_pushinteger(L, (lua_Integer)threshold);
    lua_setfield(L, -2, "threshold");
}

static int lua_driver_captouch_read(lua_State *L)
{
    lua_driver_captouch_ud_t *ud = lua_driver_captouch_get_ud(L, 1);
    int pressed_count = 0;

    lua_newtable(L);  /* result table */
    lua_newtable(L);  /* keys array */

    for (int i = 0; i < ud->key_count; i++) {
        u8  ch        = ud->channel[i];
        u32 smooth    = CapTouch_GetChAveData(CAPTOUCH_DEV, ch);
        u32 baseline  = CapTouch_GetChBaseline(CAPTOUCH_DEV, ch);
        u32 threshold = CapTouch_GetChDiffThres(CAPTOUCH_DEV, ch);
        /* Capacitance increases on touch → raw count decreases → baseline > smooth */
        s32 delta   = (s32)baseline - (s32)smooth;
        int pressed = (delta >= (s32)threshold);

        if (pressed) {
            pressed_count++;
        }

        lua_driver_captouch_push_key_table(L, i, (int)ch, (int)ud->pin[i],
                                        pressed, smooth, baseline, delta, threshold);
        lua_rawseti(L, -2, i + 1);
    }

    lua_setfield(L, -2, "keys");
    lua_pushinteger(L, ud->key_count);
    lua_setfield(L, -2, "count");
    lua_pushboolean(L, pressed_count > 0);
    lua_setfield(L, -2, "any_pressed");
    lua_pushinteger(L, pressed_count);
    lua_setfield(L, -2, "pressed_count");
    return 1;
}

static int lua_driver_captouch_is_pressed(lua_State *L)
{
    lua_driver_captouch_ud_t *ud = lua_driver_captouch_get_ud(L, 1);
    int index = (int)luaL_checkinteger(L, 2);

    luaL_argcheck(L, index >= 1 && index <= ud->key_count, 2,
                  "captouch key index out of range");

    u8  ch        = ud->channel[index - 1];
    u32 smooth    = CapTouch_GetChAveData(CAPTOUCH_DEV, ch);
    u32 baseline  = CapTouch_GetChBaseline(CAPTOUCH_DEV, ch);
    u32 threshold = CapTouch_GetChDiffThres(CAPTOUCH_DEV, ch);
    s32 delta     = (s32)baseline - (s32)smooth;
    int pressed   = (delta >= (s32)threshold);

    lua_pushboolean(L, pressed);
    return 1;
}

static int lua_driver_captouch_name(lua_State *L)
{
    lua_driver_captouch_ud_t *ud = lua_driver_captouch_get_ud(L, 1);
    lua_pushstring(L, ud->name);
    return 1;
}

/* set_threshold(index, val) — adjust diff threshold at runtime */
static int lua_driver_captouch_set_threshold(lua_State *L)
{
    lua_driver_captouch_ud_t *ud = lua_driver_captouch_get_ud(L, 1);
    int index = (int)luaL_checkinteger(L, 2);
    lua_Integer val = luaL_checkinteger(L, 3);

    luaL_argcheck(L, index >= 1 && index <= ud->key_count, 2,
                  "captouch key index out of range");
    luaL_argcheck(L, val > 0 && val <= 0xFFF, 3, "threshold must be 1..4095");

    u8 ch = ud->channel[index - 1];
    ud->threshold[index - 1] = (u32)val;

    rtos_mutex_take(s_ctc_lock, MUTEX_WAIT_TIMEOUT);
    CapTouch_SetChDiffThres(CAPTOUCH_DEV, ch, (u32)val);
    rtos_mutex_give(s_ctc_lock);

    return 0;
}

/* set_mbias(index, val) — adjust channel mbias current at runtime */
static int lua_driver_captouch_set_mbias(lua_State *L)
{
    lua_driver_captouch_ud_t *ud = lua_driver_captouch_get_ud(L, 1);
    int index = (int)luaL_checkinteger(L, 2);
    lua_Integer val = luaL_checkinteger(L, 3);

    luaL_argcheck(L, index >= 1 && index <= ud->key_count, 2,
                  "captouch key index out of range");
    luaL_argcheck(L, val >= 0 && val <= 0xFFFF, 3, "mbias must be 0..65535");

    u8 ch = ud->channel[index - 1];

    rtos_mutex_take(s_ctc_lock, MUTEX_WAIT_TIMEOUT);
    CapTouch_SetChMbias(CAPTOUCH_DEV, ch, (u16)val);
    rtos_mutex_give(s_ctc_lock);

    return 0;
}

/* set_scan_interval(val) — raw register value 0..4095 (default=60) */
static int lua_driver_captouch_set_scan_interval(lua_State *L)
{
    lua_driver_captouch_get_ud(L, 1);
    lua_Integer val = luaL_checkinteger(L, 2);

    luaL_argcheck(L, val >= 0 && val <= 4095, 2, "scan_interval must be 0..4095");

    rtos_mutex_take(s_ctc_lock, MUTEX_WAIT_TIMEOUT);
    CapTouch_SetScanInterval(CAPTOUCH_DEV, (u32)val);
    rtos_mutex_give(s_ctc_lock);

    return 0;
}

/* get_ch_status(index) → hardware touch-event status bits for that channel */
static int lua_driver_captouch_get_ch_status(lua_State *L)
{
    lua_driver_captouch_ud_t *ud = lua_driver_captouch_get_ud(L, 1);
    int index = (int)luaL_checkinteger(L, 2);

    luaL_argcheck(L, index >= 1 && index <= ud->key_count, 2,
                  "captouch key index out of range");

    u8  ch     = ud->channel[index - 1];
    u32 status = CapTouch_GetChStatus(CAPTOUCH_DEV, (u32)ch);
    lua_pushinteger(L, (lua_Integer)status);
    return 1;
}

static void lua_driver_captouch_do_close(lua_driver_captouch_ud_t *ud)
{
    if (ud->closed) {
        return;
    }
    ud->closed = 1;

    rtos_mutex_take(s_ctc_lock, MUTEX_WAIT_TIMEOUT);

    for (int i = 0; i < ud->key_count; i++) {
        CapTouch_ChCmd(CAPTOUCH_DEV, ud->channel[i], DISABLE);
    }
    if (s_ctc_refcount > 0) {
        s_ctc_refcount--;
    }
    if (s_ctc_refcount == 0 && s_ctc_initialized) {
        CapTouch_Cmd(CAPTOUCH_DEV, DISABLE);
        RCC_PeriphClockCmd(APBPeriph_ADC, APBPeriph_ADC_CLOCK, DISABLE);
        s_ctc_initialized = 0;
    }

    rtos_mutex_give(s_ctc_lock);
}

static int lua_driver_captouch_close(lua_State *L)
{
    lua_driver_captouch_ud_t *ud = (lua_driver_captouch_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_CAPTOUCH_METATABLE);
    lua_driver_captouch_do_close(ud);
    return 0;
}

static int lua_driver_captouch_gc(lua_State *L)
{
    lua_driver_captouch_ud_t *ud = (lua_driver_captouch_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_CAPTOUCH_METATABLE);
    if (ud) {
        lua_driver_captouch_do_close(ud);
    }
    return 0;
}

int luaopen_captouch(lua_State *L)
{
    if (!s_ctc_lock) {
        rtos_mutex_create(&s_ctc_lock);
    }

    if (luaL_newmetatable(L, LUA_DRIVER_CAPTOUCH_METATABLE)) {
        lua_pushcfunction(L, lua_driver_captouch_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_driver_captouch_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_driver_captouch_is_pressed);
        lua_setfield(L, -2, "is_pressed");
        lua_pushcfunction(L, lua_driver_captouch_set_threshold);
        lua_setfield(L, -2, "set_threshold");
        lua_pushcfunction(L, lua_driver_captouch_set_mbias);
        lua_setfield(L, -2, "set_mbias");
        lua_pushcfunction(L, lua_driver_captouch_set_scan_interval);
        lua_setfield(L, -2, "set_scan_interval");
        lua_pushcfunction(L, lua_driver_captouch_get_ch_status);
        lua_setfield(L, -2, "get_ch_status");
        lua_pushcfunction(L, lua_driver_captouch_name);
        lua_setfield(L, -2, "name");
        lua_pushcfunction(L, lua_driver_captouch_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_driver_captouch_new);
    lua_setfield(L, -2, "new");
    return 1;
}

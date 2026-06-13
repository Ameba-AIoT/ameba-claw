/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_driver_pwm.h"

#include <string.h>

#include "ameba_soc.h"
#include "lauxlib.h"
#include "luhw.h"

#define LUA_DRIVER_PWM_METATABLE  "pwm.handle"
#define LUA_DRIVER_PWM_CLK_HZ     40000000U   /* TIM4-TIM7 fixed 40 MHz clock */
#define LUA_DRIVER_PWM_ARR_MAX    65535U
#define LUA_DRIVER_PWM_ARR_MIN    4U

/*
 * Pinmux base functions for TIM4-TIM7 are contiguous in groups of 4.
 * TIM(4+n) PWM channel c → PINMUX_FUNCTION_TIM4_PWM0 + n*4 + c
 */
#define LUA_DRIVER_PWM_PINMUX_BASE  PINMUX_FUNCTION_TIM4_PWM0

typedef struct {
    RTIM_TypeDef *tim;
    u8            tim_idx;    /* 4-7 */
    u16           channel;    /* 0-3 */
    u32           period;     /* ARR + 1 (total ticks per cycle) */
    double        duty;       /* last set duty percent, preserved across set_frequency */
    int           closed;
} lua_driver_pwm_ud_t;

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static lua_driver_pwm_ud_t *lua_driver_pwm_get_ud(lua_State *L, int idx)
{
    lua_driver_pwm_ud_t *ud = (lua_driver_pwm_ud_t *)luaL_checkudata(
        L, idx, LUA_DRIVER_PWM_METATABLE);
    if (!ud || ud->closed) {
        luaL_error(L, "pwm: invalid or closed handle");
    }
    return ud;
}

/*
 * Calculate PSC and ARR such that:
 *   freq_hz = LUA_DRIVER_PWM_CLK_HZ / ((PSC+1) * period)
 * where period = ARR+1.
 * Maximises ARR for best duty-cycle resolution.
 * Returns 1 on success, 0 if freq_hz is out of range.
 */
static int lua_driver_pwm_calc_timing(u32 freq_hz, u32 *out_psc, u32 *out_arr)
{
    if (freq_hz == 0 || freq_hz > LUA_DRIVER_PWM_CLK_HZ) {
        return 0;
    }
    u32 total = LUA_DRIVER_PWM_CLK_HZ / freq_hz;
    if (total < LUA_DRIVER_PWM_ARR_MIN) {
        return 0;
    }
    /* PSC+1 = ceil(total / ARR_MAX) */
    u32 psc_plus1 = (total + LUA_DRIVER_PWM_ARR_MAX - 1) / LUA_DRIVER_PWM_ARR_MAX;
    u32 arr = (total / psc_plus1) - 1;
    if (arr == 0) {
        return 0;
    }
    *out_psc = psc_plus1 - 1;
    *out_arr = arr;
    return 1;
}

/* Apply duty cycle to a running (or stopped) PWM channel. */
static void lua_driver_pwm_apply_duty(lua_driver_pwm_ud_t *ud, double duty_percent)
{
    u32 ccr;
    if (duty_percent <= 0.0) {
        ccr = 0;
    } else if (duty_percent >= 100.0) {
        ccr = ud->period;   /* counter never reaches period → always high */
    } else {
        ccr = (u32)((duty_percent / 100.0 * (double)ud->period) + 0.5);
        if (ccr == 0) {
            ccr = 1;
        }
        if (ccr >= ud->period) {
            ccr = ud->period - 1;
        }
    }
    ud->duty = duty_percent;
    RTIM_CCRxSet(ud->tim, ccr, ud->channel);
}

/* Release hardware resources for a PWM handle. */
static void lua_driver_pwm_destroy(lua_driver_pwm_ud_t *ud)
{
    if (ud->closed) {
        return;
    }
    RTIM_CCxCmd(ud->tim, ud->channel, TIM_CCx_Disable);
    RTIM_DeInit(ud->tim);
    RCC_PeriphClockCmd(APBPeriph_TIMx[ud->tim_idx],
                       APBPeriph_TIMx_CLOCK[ud->tim_idx], DISABLE);
    ud->closed = 1;
}

/* ------------------------------------------------------------------ */
/* Lua constructor                                                       */
/* ------------------------------------------------------------------ */

static int lua_driver_pwm_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    /* Required: pin */
    lua_getfield(L, 1, "pin");
    if (lua_isnil(L, -1)) {
        return luaL_error(L, "pwm.new: 'pin' is required");
    }
    PinName pin = luhw_check_pin(L, -1);
    lua_pop(L, 1);

    /* Required: timer_idx (4-7) */
    lua_getfield(L, 1, "timer_idx");
    if (lua_isnil(L, -1)) {
        return luaL_error(L, "pwm.new: 'timer_idx' is required (4-7)");
    }
    int timer_idx = (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    if (timer_idx < 4 || timer_idx > 7) {
        return luaL_error(L, "pwm.new: timer_idx must be 4-7");
    }

    /* Required: channel (0-3) */
    lua_getfield(L, 1, "channel");
    if (lua_isnil(L, -1)) {
        return luaL_error(L, "pwm.new: 'channel' is required (0-3)");
    }
    int channel = (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    if (channel < 0 || channel > 3) {
        return luaL_error(L, "pwm.new: channel must be 0-3");
    }

    /* Optional: frequency_hz (default 1000) */
    lua_getfield(L, 1, "frequency_hz");
    u32 freq_hz = (u32)luaL_optinteger(L, -1, 1000);
    lua_pop(L, 1);

    /* Optional: duty_percent (default 50) */
    lua_getfield(L, 1, "duty_percent");
    double duty = luaL_optnumber(L, -1, 50.0);
    lua_pop(L, 1);

    if (duty < 0.0 || duty > 100.0) {
        return luaL_error(L, "pwm.new: duty_percent must be 0-100");
    }

    u32 psc, arr;
    if (!lua_driver_pwm_calc_timing(freq_hz, &psc, &arr)) {
        return luaL_error(L, "pwm.new: frequency_hz %u is out of range", (unsigned)freq_hz);
    }

    /* Enable clock */
    RCC_PeriphClockCmd(APBPeriph_TIMx[timer_idx],
                       APBPeriph_TIMx_CLOCK[timer_idx], ENABLE);

    /* Timer base init */
    RTIM_TimeBaseInitTypeDef tb;
    RTIM_TimeBaseStructInit(&tb);
    tb.TIM_Idx        = (u8)timer_idx;
    tb.TIM_Prescaler  = psc;
    tb.TIM_Period     = arr;
    tb.TIM_UpdateSource = TIM_UpdateSource_Overflow;
    tb.TIM_ARRProtection = ENABLE;
    RTIM_TimeBaseInit(TIMx[timer_idx], &tb, TIMx_irq[timer_idx], NULL, NULL);

    /* Channel (compare/capture) init */
    TIM_CCInitTypeDef cc;
    RTIM_CCStructInit(&cc);
    cc.TIM_OCPulse    = 0;
    cc.TIM_CCMode     = TIM_CCMode_PWM;
    cc.TIM_CCPolarity = TIM_CCPolarity_High;
    cc.TIM_OCProtection = TIM_OCPreload_Enable;
    RTIM_CCxInit(TIMx[timer_idx], &cc, (u16)channel);
    RTIM_CCxCmd(TIMx[timer_idx], (u16)channel, TIM_CCx_Enable);

    /* Pinmux: TIM(4+n) channel c → PINMUX_FUNCTION_TIM4_PWM0 + n*4 + c */
    u32 pinmux_func = (u32)(LUA_DRIVER_PWM_PINMUX_BASE
                             + (timer_idx - 4) * 4
                             + channel);
    Pinmux_Config((u8)pin, pinmux_func);

    /* Start timer */
    RTIM_Cmd(TIMx[timer_idx], ENABLE);

    /* Create userdata */
    lua_driver_pwm_ud_t *ud = (lua_driver_pwm_ud_t *)lua_newuserdata(
        L, sizeof(*ud));
    ud->tim      = TIMx[timer_idx];
    ud->tim_idx  = (u8)timer_idx;
    ud->channel  = (u16)channel;
    ud->period   = arr + 1;
    ud->closed   = 0;

    /* Apply initial duty */
    lua_driver_pwm_apply_duty(ud, duty);

    luaL_getmetatable(L, LUA_DRIVER_PWM_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Lua methods                                                           */
/* ------------------------------------------------------------------ */

static int lua_driver_pwm_start(lua_State *L)
{
    lua_driver_pwm_ud_t *ud = lua_driver_pwm_get_ud(L, 1);
    RTIM_Cmd(ud->tim, ENABLE);
    return 0;
}

static int lua_driver_pwm_stop(lua_State *L)
{
    lua_driver_pwm_ud_t *ud = lua_driver_pwm_get_ud(L, 1);
    RTIM_Cmd(ud->tim, DISABLE);
    return 0;
}

static int lua_driver_pwm_set_enabled(lua_State *L)
{
    lua_driver_pwm_ud_t *ud = lua_driver_pwm_get_ud(L, 1);
    int enable = lua_toboolean(L, 2);
    RTIM_Cmd(ud->tim, enable ? ENABLE : DISABLE);
    return 0;
}

static int lua_driver_pwm_set_duty(lua_State *L)
{
    lua_driver_pwm_ud_t *ud = lua_driver_pwm_get_ud(L, 1);
    double duty;

    /* set_duty(percent) or set_duty(channel, percent) — channel arg ignored,
     * accepted for compatibility with single-channel PWM drivers that pass it. */
    if (lua_gettop(L) >= 3) {
        duty = luaL_checknumber(L, 3);
    } else {
        duty = luaL_checknumber(L, 2);
    }

    if (duty < 0.0 || duty > 100.0) {
        return luaL_error(L, "pwm: duty_percent must be 0-100");
    }
    lua_driver_pwm_apply_duty(ud, duty);
    return 0;
}

static int lua_driver_pwm_set_frequency(lua_State *L)
{
    lua_driver_pwm_ud_t *ud = lua_driver_pwm_get_ud(L, 1);
    u32 freq_hz = (u32)luaL_checkinteger(L, 2);

    u32 psc, arr;
    if (!lua_driver_pwm_calc_timing(freq_hz, &psc, &arr)) {
        return luaL_error(L, "pwm: frequency_hz %u is out of range", (unsigned)freq_hz);
    }

    RTIM_Cmd(ud->tim, DISABLE);
    RTIM_PrescalerConfig(ud->tim, psc, TIM_PSCReloadMode_Immediate);
    RTIM_ChangePeriodImmediate(ud->tim, arr);
    ud->period = arr + 1;
    lua_driver_pwm_apply_duty(ud, ud->duty);  /* re-apply duty ratio with new period */
    RTIM_Cmd(ud->tim, ENABLE);
    return 0;
}

static int lua_driver_pwm_get_channel_count(lua_State *L)
{
    lua_driver_pwm_get_ud(L, 1);
    lua_pushinteger(L, 1);
    return 1;
}

static int lua_driver_pwm_close(lua_State *L)
{
    lua_driver_pwm_ud_t *ud = (lua_driver_pwm_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_PWM_METATABLE);
    if (ud) {
        lua_driver_pwm_destroy(ud);
    }
    return 0;
}

static int lua_driver_pwm_gc(lua_State *L)
{
    lua_driver_pwm_ud_t *ud = (lua_driver_pwm_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_PWM_METATABLE);
    if (ud) {
        lua_driver_pwm_destroy(ud);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Module open                                                           */
/* ------------------------------------------------------------------ */

LUAMOD_API int luaopen_pwm(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_DRIVER_PWM_METATABLE)) {
        lua_pushcfunction(L, lua_driver_pwm_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_driver_pwm_start);
        lua_setfield(L, -2, "start");
        lua_pushcfunction(L, lua_driver_pwm_stop);
        lua_setfield(L, -2, "stop");
        lua_pushcfunction(L, lua_driver_pwm_set_enabled);
        lua_setfield(L, -2, "set_enabled");
        lua_pushcfunction(L, lua_driver_pwm_set_duty);
        lua_setfield(L, -2, "set_duty");
        lua_pushcfunction(L, lua_driver_pwm_set_frequency);
        lua_setfield(L, -2, "set_frequency");
        lua_pushcfunction(L, lua_driver_pwm_get_channel_count);
        lua_setfield(L, -2, "get_channel_count");
        lua_pushcfunction(L, lua_driver_pwm_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_driver_pwm_new);
    lua_setfield(L, -2, "new");
    return 1;
}

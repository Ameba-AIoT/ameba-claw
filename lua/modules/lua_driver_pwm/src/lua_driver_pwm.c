/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Concurrency design (concurrency.md STEP 1 classification):
 *
 * Peripheral class : NON-BUS (PWM timer TIM4-TIM7).
 *   TIM4-TIM7 each have up to 4 PWM output channels.  Multiple Lua handles may
 *   share the same physical timer (different channels), so timer-wide operations
 *   (init, deinit, set_frequency, start, stop) must be serialised per-timer.
 *
 * Called from ISR? : NO — all exported functions run in Lua task context
 *   (cap_lua jobs / timer callbacks), never from an ISR.
 *   => rtos_mutex is the correct primitive (concurrency.md decision tree).
 *
 * Strategy (concurrency.md template C, adapted for non-bus with init/deinit):
 *   1. One rtos_mutex per physical timer (TIM4-TIM7 -> index 0-3), held only
 *      during timer-wide operations (init / deinit / set_frequency / start /
 *      stop).  Created once in lua_driver_pwm_init() at boot, before any
 *      concurrent Lua execution can start.
 *   2. Reference counting: the first handle on a timer runs RTIM_TimeBaseInit()
 *      and enables the peripheral clock; the last handle runs RTIM_DeInit() and
 *      disables the clock.  Closing one handle therefore never kills a sibling
 *      handle on the same timer (CONC-03).
 *   3. Conflicting configuration: opening the same timer twice with a different
 *      frequency_hz is rejected with an error (CONC-04).
 *
 * Critical-section purity (CONC-02 / concurrency.md red-line 1):
 *   - lua_newuserdata() is called BEFORE rtos_mutex_take, never inside.
 *   - ud->closed is pre-set to 1 before the lock so that a GC-triggered __gc
 *     returns cleanly even if lock acquisition fails and luaL_error fires.
 *   - No Lua API that can longjmp (luaL_error, luaL_check*, lua_newuserdata) is
 *     ever invoked while the mutex is held; errors detected inside the lock are
 *     stored in a plain integer flag, the lock is released, then luaL_error is
 *     called outside.
 *
 * Per-channel set_duty uses RTIM_CCRxSet which writes a single CCRx register
 * dedicated to each channel.  Different handles own different channels on the
 * same timer, so set_duty is channel-safe without the per-timer mutex.
 * Sharing one handle between two Lua tasks is not supported (document-only).
 */

#include "lua_driver_pwm.h"

#include <string.h>

#include "ameba_soc.h"
#include "lauxlib.h"
#include "luhw.h"
#include "os_wrapper.h"

#define LUA_DRIVER_PWM_METATABLE    "pwm.handle"
#define LUA_DRIVER_PWM_CLK_HZ       40000000U   /* TIM4-TIM7 fixed 40 MHz clock */
#define LUA_DRIVER_PWM_ARR_MAX      65535U
#define LUA_DRIVER_PWM_ARR_MIN      4U
#define LUA_DRIVER_PWM_NUM_TIMERS     4            /* TIM4-TIM7 -> index 0-3 */
#define LUA_DRIVER_PWM_TIM_BASE       4            /* TIMx index base */
#define LUA_DRIVER_PWM_CHANS_PER_TIM  4            /* channels per timer (CH0-CH3) */
#define LUA_DRIVER_PWM_LOCK_MS        100          /* mutex acquire timeout (ms) */
#define LUA_DRIVER_PWM_DESTROY_LOCK_MS 1000        /* destroy() timeout: longer to survive busy timers */

/*
 * Pinmux base: TIM(4+n) PWM channel c -> PINMUX_FUNCTION_TIM4_PWM0 + n*4 + c
 */
#define LUA_DRIVER_PWM_PINMUX_BASE  PINMUX_FUNCTION_TIM4_PWM0

/* Per-timer shared state — one entry for TIM4 (idx 0) through TIM7 (idx 3). */
typedef struct {
    rtos_mutex_t lock;     /* guards timer-wide ops: init/deinit/freq/start/stop */
    int          refcnt;   /* number of live handles on this timer */
    u32          period;   /* ARR+1, shared period for all channels on this timer */
    u32          freq_hz;  /* current configured frequency */
    u8           inited;   /* 1 after RTIM_TimeBaseInit; cleared when refcnt=0 */
} lua_driver_pwm_timer_t;

static lua_driver_pwm_timer_t s_pwm_timer[LUA_DRIVER_PWM_NUM_TIMERS];

/* Per-handle state stored as Lua userdata. */
typedef struct {
    RTIM_TypeDef *tim;
    u8            tim_idx;    /* 4-7 */
    u16           channel;    /* 0-3 */
    u32           period;     /* cached ARR+1 for duty calculations */
    double        duty;       /* last set duty percent */
    int           closed;     /* 1 = hardware released, handle invalid */
} lua_driver_pwm_ud_t;

/* ------------------------------------------------------------------ */
/* Boot-time init (call once before concurrent execution starts)       */
/* ------------------------------------------------------------------ */

void lua_driver_pwm_init(void)
{
    int i;
    for (i = 0; i < LUA_DRIVER_PWM_NUM_TIMERS; i++) {
        rtos_mutex_create(&s_pwm_timer[i].lock);
    }
}

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
    u32 psc_plus1 = (total + LUA_DRIVER_PWM_ARR_MAX - 1) / LUA_DRIVER_PWM_ARR_MAX;
    u32 arr = (total / psc_plus1) - 1;
    if (arr == 0) {
        return 0;
    }
    *out_psc = psc_plus1 - 1;
    *out_arr = arr;
    return 1;
}

/* Apply duty cycle to a running (or stopped) PWM channel.
 * Writes one CCRx register — per-channel, no timer-wide lock required. */
static void lua_driver_pwm_apply_duty(lua_driver_pwm_ud_t *ud, double duty_percent)
{
    u32 ccr;
    if (duty_percent <= 0.0) {
        ccr = 0;
    } else if (duty_percent >= 100.0) {
        ccr = ud->period;   /* CCR >= period -> output stays high */
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

/* Release hardware resources for a PWM handle.
 * Decrements refcnt under lock; only DeInits the timer when the last handle
 * is closed, so sibling handles on the same timer are never disturbed. */
static void lua_driver_pwm_destroy(lua_driver_pwm_ud_t *ud)
{
    if (ud->closed) {
        return;
    }

    int idx = (int)ud->tim_idx - LUA_DRIVER_PWM_TIM_BASE;

    /* Disable this channel's output — per-channel register, no timer lock. */
    RTIM_CCxCmd(ud->tim, ud->channel, TIM_CCx_Disable);

    /* Serialise refcnt decrement and conditional DeInit.
     * Use a finite timeout (CONC-04): if the lock cannot be acquired the handle
     * is still marked closed to prevent double-deinit; the timer hardware is left
     * running rather than racing a concurrent user. */
    if (rtos_mutex_take(s_pwm_timer[idx].lock,
                        LUA_DRIVER_PWM_DESTROY_LOCK_MS) != RTK_SUCCESS) {
        printf("[pwm] destroy: timer %d lock timeout, skipping deinit\n",
               (int)ud->tim_idx);
        ud->closed = 1;
        return;
    }
    s_pwm_timer[idx].refcnt--;
    if (s_pwm_timer[idx].refcnt == 0) {
        RTIM_DeInit(ud->tim);
        RCC_PeriphClockCmd(APBPeriph_TIMx[ud->tim_idx],
                           APBPeriph_TIMx_CLOCK[ud->tim_idx], DISABLE);
        s_pwm_timer[idx].inited   = 0;
        s_pwm_timer[idx].period   = 0;
        s_pwm_timer[idx].freq_hz  = 0;
    }
    rtos_mutex_give(s_pwm_timer[idx].lock);

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
        /* %d not %u: luaL_error routes through lua_pushfstring, which supports
         * only %d/%f/%s/%p/%c/%U/%I/%% — a %u raises "invalid option '%u' to
         * 'lua_pushfstring'" instead of this message. freq_hz fits in int. */
        return luaL_error(L, "pwm.new: frequency_hz %d is out of range", (int)freq_hz);
    }

    /* Allocate userdata BEFORE acquiring the lock (CONC-02 red-line 1).
     * Set closed=1 immediately so that if luaL_error fires before hardware
     * init completes, the GC-triggered __gc returns cleanly. */
    lua_driver_pwm_ud_t *ud = (lua_driver_pwm_ud_t *)lua_newuserdata(
        L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    ud->closed = 1;

    luaL_getmetatable(L, LUA_DRIVER_PWM_METATABLE);
    lua_setmetatable(L, -2);

    int idx = timer_idx - LUA_DRIVER_PWM_TIM_BASE;

    /* Acquire per-timer lock — must NOT call any Lua API while lock is held. */
    if (rtos_mutex_take(s_pwm_timer[idx].lock,
                        LUA_DRIVER_PWM_LOCK_MS) != RTK_SUCCESS) {
        return luaL_error(L, "pwm.new: timer %d busy", timer_idx);
    }

    int err_conflict = 0;

    if (!s_pwm_timer[idx].inited) {
        /* First handle on this timer: initialise hardware. */
        RCC_PeriphClockCmd(APBPeriph_TIMx[timer_idx],
                           APBPeriph_TIMx_CLOCK[timer_idx], ENABLE);

        RTIM_TimeBaseInitTypeDef tb;
        RTIM_TimeBaseStructInit(&tb);
        tb.TIM_Idx           = (u8)timer_idx;
        tb.TIM_Prescaler     = psc;
        tb.TIM_Period        = arr;
        tb.TIM_UpdateSource  = TIM_UpdateSource_Overflow;
        tb.TIM_ARRProtection = ENABLE;
        RTIM_TimeBaseInit(TIMx[timer_idx], &tb, TIMx_irq[timer_idx], NULL, NULL);

        s_pwm_timer[idx].inited  = 1;
        s_pwm_timer[idx].period  = arr + 1;
        s_pwm_timer[idx].freq_hz = freq_hz;

        RTIM_Cmd(TIMx[timer_idx], ENABLE);
    } else if (s_pwm_timer[idx].freq_hz != freq_hz) {
        /* Timer already in use with a different frequency — reject. */
        err_conflict = 1;
    }
    /* else: compatible re-open (same timer, same freq) — reuse existing config. */

    if (!err_conflict) {
        s_pwm_timer[idx].refcnt++;
    }

    rtos_mutex_give(s_pwm_timer[idx].lock);

    /* After releasing the lock it is safe to call luaL_error again. */
    if (err_conflict) {
        return luaL_error(L,
            "pwm.new: timer %d already open at %u Hz; requested %u Hz",
            timer_idx, (unsigned)s_pwm_timer[idx].freq_hz, (unsigned)freq_hz);
    }

    /* Channel-specific setup — no timer-wide lock needed for CCRx/pinmux. */
    TIM_CCInitTypeDef cc;
    RTIM_CCStructInit(&cc);
    cc.TIM_OCPulse      = 0;
    cc.TIM_CCMode       = TIM_CCMode_PWM;
    cc.TIM_CCPolarity   = TIM_CCPolarity_High;
    cc.TIM_OCProtection = TIM_OCPreload_Enable;
    RTIM_CCxInit(TIMx[timer_idx], &cc, (u16)channel);
    RTIM_CCxCmd(TIMx[timer_idx], (u16)channel, TIM_CCx_Enable);

    u32 pinmux_func = (u32)(LUA_DRIVER_PWM_PINMUX_BASE
                             + (timer_idx - LUA_DRIVER_PWM_TIM_BASE) * LUA_DRIVER_PWM_CHANS_PER_TIM
                             + channel);
    Pinmux_Config((u8)pin, pinmux_func);

    /* Populate handle fields and open it. */
    ud->tim     = TIMx[timer_idx];
    ud->tim_idx = (u8)timer_idx;
    ud->channel = (u16)channel;
    ud->period  = s_pwm_timer[idx].period;
    ud->closed  = 0;

    /* Apply initial duty — per-channel CCRxSet, no lock needed. */
    lua_driver_pwm_apply_duty(ud, duty);

    return 1;
}

/* ------------------------------------------------------------------ */
/* Lua methods                                                           */
/* ------------------------------------------------------------------ */

static int lua_driver_pwm_set_enabled(lua_State *L)
{
    lua_driver_pwm_ud_t *ud = lua_driver_pwm_get_ud(L, 1);
    int enable = lua_toboolean(L, 2);
    int idx = (int)ud->tim_idx - LUA_DRIVER_PWM_TIM_BASE;

    if (rtos_mutex_take(s_pwm_timer[idx].lock,
                        LUA_DRIVER_PWM_LOCK_MS) != RTK_SUCCESS) {
        return luaL_error(L, "pwm: timer %d busy", (int)ud->tim_idx);
    }
    RTIM_Cmd(ud->tim, enable ? ENABLE : DISABLE);
    rtos_mutex_give(s_pwm_timer[idx].lock);
    return 0;
}

static int lua_driver_pwm_set_duty(lua_State *L)
{
    lua_driver_pwm_ud_t *ud = lua_driver_pwm_get_ud(L, 1);
    double duty;

    /* set_duty(percent) or set_duty(channel, percent) — channel arg ignored,
     * accepted for API compatibility. */
    if (lua_gettop(L) >= 3) {
        duty = luaL_checknumber(L, 3);
    } else {
        duty = luaL_checknumber(L, 2);
    }

    if (duty < 0.0 || duty > 100.0) {
        return luaL_error(L, "pwm: duty_percent must be 0-100");
    }
    /* Per-channel CCRxSet — no timer-wide lock needed. */
    lua_driver_pwm_apply_duty(ud, duty);
    return 0;
}

static int lua_driver_pwm_set_frequency(lua_State *L)
{
    lua_driver_pwm_ud_t *ud = lua_driver_pwm_get_ud(L, 1);
    u32 freq_hz = (u32)luaL_checkinteger(L, 2);

    u32 psc, arr;
    if (!lua_driver_pwm_calc_timing(freq_hz, &psc, &arr)) {
        /* %d not %u — see the note in lua_driver_pwm_new above. */
        return luaL_error(L, "pwm: frequency_hz %d is out of range", (int)freq_hz);
    }

    int idx = (int)ud->tim_idx - LUA_DRIVER_PWM_TIM_BASE;

    if (rtos_mutex_take(s_pwm_timer[idx].lock,
                        LUA_DRIVER_PWM_LOCK_MS) != RTK_SUCCESS) {
        return luaL_error(L, "pwm: timer %d busy", (int)ud->tim_idx);
    }

    /* CONC-04 extension: set_frequency reconfigures PSC/ARR for the whole
     * timer.  Sibling handles cache their own ud->period and there is no way
     * to reach their userdata from s_pwm_timer, so syncing them is
     * impossible.  Reject the call when another handle shares the timer. */
    int err_shared = (s_pwm_timer[idx].refcnt > 1);
    int captured_refcnt = s_pwm_timer[idx].refcnt;

    if (!err_shared) {
        /* Reconfigure timer-wide timing registers. */
        RTIM_Cmd(ud->tim, DISABLE);
        RTIM_PrescalerConfig(ud->tim, psc, TIM_PSCReloadMode_Immediate);
        RTIM_ChangePeriodImmediate(ud->tim, arr);

        ud->period               = arr + 1;
        s_pwm_timer[idx].period  = arr + 1;
        s_pwm_timer[idx].freq_hz = freq_hz;

        /* Re-apply duty ratio with updated period — CCRxSet is safe inside lock. */
        lua_driver_pwm_apply_duty(ud, ud->duty);

        RTIM_Cmd(ud->tim, ENABLE);
    }

    rtos_mutex_give(s_pwm_timer[idx].lock);

    if (err_shared) {
        return luaL_error(L,
            "pwm: timer %d has %d active handles; close others before changing frequency",
            (int)ud->tim_idx, captured_refcnt);
    }
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

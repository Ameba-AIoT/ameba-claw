/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * lua_driver_basictimer.c — Lua binding for RTL8721F basic hardware timers
 * (TIM0–TIM3).
 *
 * Clock sources:
 *   "sdm32k" (default): 32768 Hz, ~30.5 us/tick.  ARR = period_us/1e6*32768-1
 *   "xtal"             : 40 MHz / xtal_div (div 2..64).  ARR = period_us*40/div-1
 *                        At div=40 (1 MHz), period_us maps directly to us.
 *
 * Thread safety:
 *   s_lock      -- protects Lua-level HW operations.  Args validated before
 *                  acquiring so longjmp cannot leak the lock.
 *   s_irq_count -- volatile u32; Cortex-M 32-bit aligned store/load is atomic,
 *                  ISR increments without the lock.
 */

#include "lua_driver_basictimer.h"

#include "ameba_soc.h"
#include "lauxlib.h"
#include "os_wrapper.h"

#define METATABLE        "basictimer.handle"
#define BASIC_TIM_NUM    4

/* ---- Global state ---- */

static rtos_mutex_t    s_lock;
static volatile u32    s_irq_count[BASIC_TIM_NUM];
static u8              s_in_use[BASIC_TIM_NUM];

#define LOCK()   do { if (s_lock) rtos_mutex_take(s_lock, 0xFFFFFFFFUL); } while (0)
#define UNLOCK() do { if (s_lock) rtos_mutex_give(s_lock); } while (0)

/* ---- Userdata ---- */

typedef struct {
    u8  tim_idx;
    u8  clk_xtal;   /* 0=SDM32K, 1=XTAL */
    u32 xtal_div;   /* 2..64; valid only when clk_xtal=1 */
    int closed;
} basictimer_ud_t;

static basictimer_ud_t *get_ud(lua_State *L, int stack_idx)
{
    basictimer_ud_t *ud = (basictimer_ud_t *)luaL_checkudata(L, stack_idx, METATABLE);
    if (!ud || ud->closed) {
        luaL_error(L, "basictimer: invalid or closed handle");
    }
    return ud;
}

/* ---- Clock helpers (switch required: macros use token pasting) ---- */

static void set_clk_src(u8 idx, u8 use_xtal)
{
    switch (idx) {
    case 0: if (use_xtal) { RCC_PeriphClockSourceSet(LTIM0, XTAL); } else { RCC_PeriphClockSourceSet(LTIM0, SDM32K); } break;
    case 1: if (use_xtal) { RCC_PeriphClockSourceSet(LTIM1, XTAL); } else { RCC_PeriphClockSourceSet(LTIM1, SDM32K); } break;
    case 2: if (use_xtal) { RCC_PeriphClockSourceSet(LTIM2, XTAL); } else { RCC_PeriphClockSourceSet(LTIM2, SDM32K); } break;
    case 3: if (use_xtal) { RCC_PeriphClockSourceSet(LTIM3, XTAL); } else { RCC_PeriphClockSourceSet(LTIM3, SDM32K); } break;
    default: break;
    }
}

static void set_xtal_div(u8 idx, u32 div)
{
    switch (idx) {
    case 0: RCC_PeriphClockDividerSet(XTAL_LTIM0, div); break;
    case 1: RCC_PeriphClockDividerSet(XTAL_LTIM1, div); break;
    case 2: RCC_PeriphClockDividerSet(XTAL_LTIM2, div); break;
    case 3: RCC_PeriphClockDividerSet(XTAL_LTIM3, div); break;
    default: break;
    }
}

/*
 * Convert period_us to raw ARR based on clock config.
 *
 * SDM32K (32768 Hz): ARR = period_us / 1e6 * 32768 - 1
 * XTAL (40MHz/div) : ARR = period_us * 40 / div - 1
 *   div=40 (1MHz): ARR = period_us - 1  (exact us resolution)
 *
 * Uses RTIM_ChangePeriodImmediate (raw ARR) rather than
 * RTIM_ChangePeriodImmediate_us to avoid that function's implicit assumption
 * that XTAL is always at 1 MHz.
 */
static u32 period_to_arr(u32 period_us, u8 clk_xtal, u32 xtal_div)
{
    if (!clk_xtal) {
        return (u32)((float)period_us / 1000000.0f * 32768.0f) - 1;
    } else {
        return (u32)((float)period_us * 40.0f / (float)xtal_div) - 1;
    }
}

/* ---- ISR ---- */

static u32 basictimer_irq_handler(void *data)
{
    u8 idx = (u8)(u32)data;
    RTIM_INTClear(TIMx[idx]);
    s_irq_count[idx]++;
    return 0;
}

/* ---- Lua API ---- */

/*
 * basictimer.new(tim_idx, period_us [, clk_src [, xtal_div]]) -> handle
 *
 *   tim_idx  : 0..3
 *   period_us: microseconds; min 1000 for sdm32k, min 5 for xtal
 *   clk_src  : "sdm32k" (default) | "xtal"
 *   xtal_div : 2..64, only used with "xtal" (default 40 -> 1 MHz, 1 us/tick)
 */
static int l_new(lua_State *L)
{
    int tim_idx   = (int)luaL_checkinteger(L, 1);
    int period_us = (int)luaL_checkinteger(L, 2);
    const char *clk_str = luaL_optstring(L, 3, "sdm32k");
    int xtal_div  = (int)luaL_optinteger(L, 4, 40);

    if (tim_idx < 0 || tim_idx >= BASIC_TIM_NUM) {
        return luaL_error(L, "basictimer.new: tim_idx must be 0-%d", BASIC_TIM_NUM - 1);
    }

    u8 clk_xtal = 0;
    if (strcmp(clk_str, "xtal") == 0) {
        clk_xtal = 1;
    } else if (strcmp(clk_str, "sdm32k") != 0) {
        return luaL_error(L, "basictimer.new: clk_src must be 'sdm32k' or 'xtal'");
    }

    if (clk_xtal && (xtal_div < 2 || xtal_div > 64)) {
        return luaL_error(L, "basictimer.new: xtal_div must be 2-64");
    }

    int min_us = clk_xtal ? 5 : 1000;
    if (period_us < min_us) {
        return luaL_error(L, "basictimer.new: period_us must be >= %d for %s clock", min_us, clk_str);
    }

    u8 idx = (u8)tim_idx;

    LOCK();
    if (s_in_use[idx]) {
        UNLOCK();
        return luaL_error(L, "basictimer.new: TIM%d already in use", (int)idx);
    }
    s_in_use[idx] = 1;
    UNLOCK();

    /* Set divider before switching clock source; then enable peripheral clock. */
    if (clk_xtal) {
        set_xtal_div(idx, (u32)xtal_div);
    }
    set_clk_src(idx, clk_xtal);

    RCC_PeriphClockCmd(APBPeriph_TIMx[idx], APBPeriph_TIMx_CLOCK[idx], ENABLE);

    RTIM_TimeBaseInitTypeDef tb;
    RTIM_TimeBaseStructInit(&tb);
    tb.TIM_Idx    = idx;
    /* Set period in the init struct to avoid a post-init ChangePeriod call
     * (which generates a UG event and could produce a spurious interrupt). */
    tb.TIM_Period = period_to_arr((u32)period_us, clk_xtal, (u32)xtal_div);

    RTIM_TimeBaseInit(TIMx[idx], &tb, TIMx_irq[idx],
                      (IRQ_FUN)basictimer_irq_handler, (u32)idx);
    RTIM_INTConfig(TIMx[idx], TIM_IT_Update, ENABLE);

    s_irq_count[idx] = 0;

    basictimer_ud_t *ud = (basictimer_ud_t *)lua_newuserdata(L, sizeof(*ud));
    ud->tim_idx  = idx;
    ud->clk_xtal = clk_xtal;
    ud->xtal_div = (u32)xtal_div;
    ud->closed   = 0;
    luaL_getmetatable(L, METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

/* handle:start() */
static int l_start(lua_State *L)
{
    basictimer_ud_t *ud = get_ud(L, 1);
    LOCK();
    RTIM_Cmd(TIMx[ud->tim_idx], ENABLE);
    UNLOCK();
    return 0;
}

/* handle:stop() */
static int l_stop(lua_State *L)
{
    basictimer_ud_t *ud = get_ud(L, 1);
    LOCK();
    RTIM_Cmd(TIMx[ud->tim_idx], DISABLE);
    UNLOCK();
    return 0;
}

/* handle:close() */
static int l_close(lua_State *L)
{
    basictimer_ud_t *ud = (basictimer_ud_t *)luaL_checkudata(L, 1, METATABLE);
    if (!ud || ud->closed) {
        return 0;
    }
    u8 idx = ud->tim_idx;
    ud->closed = 1;

    LOCK();
    RTIM_DeInit(TIMx[idx]);
    RCC_PeriphClockCmd(APBPeriph_TIMx[idx], APBPeriph_TIMx_CLOCK[idx], DISABLE);
    /* Restore to SDM32K so the next new() starts from a known state. */
    set_clk_src(idx, 0);
    s_irq_count[idx] = 0;
    s_in_use[idx]    = 0;
    UNLOCK();
    return 0;
}

/* handle:get_count() -> integer (current counter register value) */
static int l_get_count(lua_State *L)
{
    basictimer_ud_t *ud = get_ud(L, 1);
    lua_pushinteger(L, (lua_Integer)RTIM_GetCount(TIMx[ud->tim_idx]));
    return 1;
}

/* handle:get_irq_count() -> integer */
static int l_get_irq_count(lua_State *L)
{
    basictimer_ud_t *ud = get_ud(L, 1);
    lua_pushinteger(L, (lua_Integer)s_irq_count[ud->tim_idx]);
    return 1;
}

/* handle:clear_irq_count() */
static int l_clear_irq_count(lua_State *L)
{
    basictimer_ud_t *ud = get_ud(L, 1);
    s_irq_count[ud->tim_idx] = 0;
    return 0;
}

/* handle:set_period(period_us)
 * Uses RTIM_ChangePeriodImmediate (raw ARR) to correctly handle any
 * xtal_div, bypassing RTIM_ChangePeriodImmediate_us which assumes 1 MHz. */
static int l_set_period(lua_State *L)
{
    basictimer_ud_t *ud = get_ud(L, 1);
    int period_us = (int)luaL_checkinteger(L, 2);

    int min_us = ud->clk_xtal ? 5 : 1000;
    if (period_us < min_us) {
        return luaL_error(L, "basictimer.set_period: period_us must be >= %d", min_us);
    }

    u32 arr = period_to_arr((u32)period_us, ud->clk_xtal, ud->xtal_div);
    LOCK();
    RTIM_ChangePeriodImmediate(TIMx[ud->tim_idx], arr);
    UNLOCK();
    return 0;
}

/* __gc metamethod */
static int l_gc(lua_State *L)
{
    basictimer_ud_t *ud = (basictimer_ud_t *)luaL_testudata(L, 1, METATABLE);
    if (ud && !ud->closed) {
        u8 idx = ud->tim_idx;
        ud->closed = 1;

        LOCK();
        RTIM_DeInit(TIMx[idx]);
        RCC_PeriphClockCmd(APBPeriph_TIMx[idx], APBPeriph_TIMx_CLOCK[idx], DISABLE);
        set_clk_src(idx, 0);
        s_irq_count[idx] = 0;
        s_in_use[idx]    = 0;
        UNLOCK();
    }
    return 0;
}

/* ---- Module init and opener ---- */

void lua_driver_basictimer_init(void)
{
    rtos_mutex_create(&s_lock);
}

int luaopen_basictimer(lua_State *L)
{
    if (luaL_newmetatable(L, METATABLE)) {
        lua_pushcfunction(L, l_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, l_start);          lua_setfield(L, -2, "start");
        lua_pushcfunction(L, l_stop);           lua_setfield(L, -2, "stop");
        lua_pushcfunction(L, l_close);          lua_setfield(L, -2, "close");
        lua_pushcfunction(L, l_get_count);      lua_setfield(L, -2, "get_count");
        lua_pushcfunction(L, l_get_irq_count);  lua_setfield(L, -2, "get_irq_count");
        lua_pushcfunction(L, l_clear_irq_count);lua_setfield(L, -2, "clear_irq_count");
        lua_pushcfunction(L, l_set_period);     lua_setfield(L, -2, "set_period");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, l_new);
    lua_setfield(L, -2, "new");
    return 1;
}

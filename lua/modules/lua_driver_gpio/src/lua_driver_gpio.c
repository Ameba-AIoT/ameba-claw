/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_driver_gpio.c — Lua GPIO driver for Ameba RTOS (RTL8721F).
**
** Provides require("gpio"):
**   gpio.set_direction(pin, "input"|"output")
**   gpio.set_level(pin, 0|1)
**   gpio.get_level(pin)           -> integer
**   gpio.set_pull(pin, "none"|"up"|"down")
**
** Interrupt:
**   gpio.set_irq(pin, trigger, [debounce_en])
**       trigger: "rising"|"falling"|"both"|"level_high"|"level_low"
**       debounce_en: 1 (default) or 0 — pass 0 to disable ~64 µs debounce filter
**   gpio.irq_enable(pin)
**   gpio.irq_disable(pin)
**   gpio.get_irq_count(pin)       -> integer (fires counted by the ISR)
**   gpio.clear_irq_count(pin)     -> reset that pin's counter to 0
**
** ── Concurrency & resources ────────────────────────────────────────────────
** require("gpio") returns a flat function table (no per-pin handle object).  It
** is loaded (luaL_requiref) into several Lua states — the REPL, the timer
** sandbox and the skill sandbox — so multiple lua_run jobs plus timer callbacks
** can call into it concurrently.  Every public API mutates shared per-pin state
** arrays (s_gpio_inited / s_irq_*) and pokes GPIO registers, so two jobs racing
** on the same pin (or on the shared GPIO clock / port-IRQ registration) could
** interleave register writes.  Mirroring lua_driver_i2c / lua_driver_rtc:
**
**   1. One process-wide rtos_mutex; every Lua API holds it for the whole
**      hardware sequence.  Created once in lua_driver_gpio_init() during the
**      single-threaded boot phase, before any concurrent Lua execution.  Lua
**      arg/range validation (luhw_check_pin / luaL_check*) runs BEFORE the lock
**      is taken — those longjmp on error and would otherwise leak the mutex.
**   2. There is no per-call hardware deinit: the GPIO clock stays on for the
**      lifetime of the boot.  "Release" of an interrupt resource means
**      gpio.irq_disable(pin) (stops firing); the counter is reset with
**      gpio.clear_irq_count(pin).  init()/set_irq() are idempotent per pin.
**   3. The interrupt callback (gpio_irq_cb) runs in ISR context and therefore
**      must NOT take the mutex; it only bumps the per-pin counter (a single
**      word write) and, for LEVEL triggers, re-arms its own pin.
*/

#define LUA_LIB
#include "lua.h"
#include "lauxlib.h"

#include "lua_driver_gpio.h"
#include "ameba_soc.h"
#include "luhw.h"
#include <string.h>
#include <stdio.h>

#define GPIO_PIN_MAX	PIN_TOTAL_NUM   /* covers PA0-PA31, PB0-PB31, PC0-PC8 */

/* ── Concurrency guard (see header comment) ──────────────────────────────────
 * Created in lua_driver_gpio_init() at boot; NULL-safe so a missing init never
 * faults (it just degrades to no locking). */
static rtos_mutex_t s_gpio_lock;

#define GPIO_LOCK()    do { if (s_gpio_lock) { rtos_mutex_take(s_gpio_lock, 0xFFFFFFFFUL); } } while (0)
#define GPIO_UNLOCK()  do { if (s_gpio_lock) { rtos_mutex_give(s_gpio_lock); } } while (0)

static uint8_t s_gpio_inited[GPIO_PIN_MAX];
static uint8_t s_irq_inited[GPIO_PIN_MAX];
static uint8_t s_irq_trigger[GPIO_PIN_MAX];  /* GPIO_INT_Trigger_* */
static uint8_t s_irq_polarity[GPIO_PIN_MAX]; /* current polarity (flipped on each level fire) */
static uint8_t s_irq_debounce[GPIO_PIN_MAX]; /* GPIO_INT_DEBOUNCE_* */
static volatile uint32_t s_irq_count[GPIO_PIN_MAX]; /* fires counted by the ISR */
static uint8_t s_port_irq_reg[3];           /* system IRQ registered per port */

/* IRQ table: port index → (GPIO base, IRQ number) */
static const struct {
	GPIO_TypeDef *base;
	IRQn_Type     irq;
} s_port_irq[3] = {
	{GPIOA_BASE, GPIOA_IRQ},
	{GPIOB_BASE, GPIOB_IRQ},
	{GPIOC_BASE, GPIOC_IRQ},
};

static void gpio_irq_cb(void *data, u32 event)
{
	(void)data;
	u32 pin = (event >> 16) & 0xFF;
	if (pin >= (u32)GPIO_PIN_MAX) {
		return;
	}
	/* Count every fire (edge or level). Single-word write — ISR-safe, no lock. */
	s_irq_count[pin]++;
	if (s_irq_trigger[pin] == GPIO_INT_Trigger_LEVEL) {
		/* 1. Disable — stops continuous re-firing while pin level holds */
		GPIO_INTConfig(pin, DISABLE);
		u8 next_pol = (s_irq_polarity[pin] == GPIO_INT_POLARITY_ACTIVE_HIGH)
		              ? GPIO_INT_POLARITY_ACTIVE_LOW
		              : GPIO_INT_POLARITY_ACTIVE_HIGH;
		s_irq_polarity[pin] = next_pol;
		/* 2. Reconfigure polarity (INT_MASK still set from step 1) */
		GPIO_INTMode(pin, ENABLE, GPIO_INT_Trigger_LEVEL, next_pol, s_irq_debounce[pin]);
		/* 3. Delay 64 µs so signal settles before re-arm (avoids false trigger) */
		DelayUs(64);
		/* 4. Unmask: clears INT_MASK and any pending flag */
		GPIO_INTConfig(pin, ENABLE);
	}
}

static void gpio_ensure_clk(void)
{
	RCC_PeriphClockCmd(APBPeriph_GPIO, APBPeriph_GPIO_CLOCK, ENABLE);
}

static void gpio_ensure_init(u32 gpio_pin)
{
	int idx = (int)gpio_pin;
	if (idx < 0 || idx >= GPIO_PIN_MAX) {
		return;
	}
	if (s_gpio_inited[idx]) {
		return;
	}
	GPIO_InitTypeDef init;
	gpio_ensure_clk();
	init.GPIO_Pin  = gpio_pin;
	init.GPIO_Mode = GPIO_Mode_IN;
	init.GPIO_PuPd = GPIO_PuPd_NOPULL;
	GPIO_Init(&init);
	s_gpio_inited[idx] = 1;
}

static void gpio_ensure_port_irq(u8 port_num)
{
	if (port_num >= 3) {
		return;
	}
	if (s_port_irq_reg[port_num]) {
		return;
	}
	InterruptRegister(GPIO_INTHandler, s_port_irq[port_num].irq,
	                  (u32)s_port_irq[port_num].base, 6);
	InterruptEn(s_port_irq[port_num].irq, 6);
	s_port_irq_reg[port_num] = 1;
}

static int lgpio_set_direction(lua_State *L)
{
	PinName     pin = luhw_check_pin(L, 1);
	const char *s   = luaL_checkstring(L, 2);
	u32 mode;
	if (strcmp(s, "input") == 0)       { mode = GPIO_Mode_IN; }
	else if (strcmp(s, "output") == 0) { mode = GPIO_Mode_OUT; }
	else { return luaL_error(L, "invalid direction '%s' (input|output)", s); }
	GPIO_LOCK();
	gpio_ensure_init((u32)pin);
	GPIO_Direction((u32)pin, mode);
	GPIO_UNLOCK();
	return 0;
}

static int lgpio_set_level(lua_State *L)
{
	PinName pin = luhw_check_pin(L, 1);
	int     val = (int)luaL_checkinteger(L, 2);
	GPIO_LOCK();
	gpio_ensure_init((u32)pin);
	GPIO_WriteBit((u32)pin, val ? 1 : 0);
	GPIO_UNLOCK();
	return 0;
}

static int lgpio_get_level(lua_State *L)
{
	PinName pin = luhw_check_pin(L, 1);
	GPIO_LOCK();
	gpio_ensure_init((u32)pin);
	lua_Integer v = (lua_Integer)GPIO_ReadDataBit((u32)pin);
	GPIO_UNLOCK();
	lua_pushinteger(L, v);
	return 1;
}

static int lgpio_set_pull(lua_State *L)
{
	PinName     pin = luhw_check_pin(L, 1);
	const char *s   = luaL_checkstring(L, 2);
	u32 pull;
	if (strcmp(s, "none") == 0)      { pull = GPIO_PuPd_NOPULL; }
	else if (strcmp(s, "up") == 0)   { pull = GPIO_PuPd_UP; }
	else if (strcmp(s, "down") == 0) { pull = GPIO_PuPd_DOWN; }
	else { return luaL_error(L, "invalid pull mode '%s' (none|up|down)", s); }
	GPIO_LOCK();
	gpio_ensure_init((u32)pin);
	PAD_PullCtrl((u8)pin, (u8)pull);
	GPIO_UNLOCK();
	return 0;
}

/*
** gpio.set_irq(pin, trigger_str [, debounce_en])
**   trigger_str: "rising" | "falling" | "both" | "level_high" | "level_low"
**   debounce_en: 1 (default) or 0 — pass 0 to disable ~64 µs debounce filter
**
** Configures the pin in interrupt mode and registers the polarity-flip callback.
** Does NOT enable the interrupt — call gpio.irq_enable() after.
*/
static int lgpio_set_irq(lua_State *L)
{
	PinName     pin      = luhw_check_pin(L, 1);
	const char *trig_str = luaL_checkstring(L, 2);
	int         db_en    = (int)luaL_optinteger(L, 3, 1);
	int         idx      = (int)(u32)pin;

	if (idx < 0 || idx >= GPIO_PIN_MAX) {
		return luaL_error(L, "pin index out of range");
	}

	u32 trigger, polarity;
	if (strcmp(trig_str, "rising") == 0) {
		trigger  = GPIO_INT_Trigger_EDGE;
		polarity = GPIO_INT_POLARITY_ACTIVE_HIGH;
	} else if (strcmp(trig_str, "falling") == 0) {
		trigger  = GPIO_INT_Trigger_EDGE;
		polarity = GPIO_INT_POLARITY_ACTIVE_LOW;
	} else if (strcmp(trig_str, "both") == 0) {
		trigger  = GPIO_INT_Trigger_BOTHEDGE;
		polarity = GPIO_INT_POLARITY_ACTIVE_HIGH; /* ignored for BOTHEDGE */
	} else if (strcmp(trig_str, "level_high") == 0) {
		trigger  = GPIO_INT_Trigger_LEVEL;
		polarity = GPIO_INT_POLARITY_ACTIVE_HIGH;
	} else if (strcmp(trig_str, "level_low") == 0) {
		trigger  = GPIO_INT_Trigger_LEVEL;
		polarity = GPIO_INT_POLARITY_ACTIVE_LOW;
	} else {
		return luaL_error(L,
		    "invalid trigger '%s' (rising|falling|both|level_high|level_low)",
		    trig_str);
	}

	GPIO_LOCK();
	gpio_ensure_clk();
	GPIO_INTConfig((u32)pin, DISABLE);

	GPIO_InitTypeDef init;
	init.GPIO_Pin        = (u32)pin;
	init.GPIO_Mode       = GPIO_Mode_INT;
	init.GPIO_PuPd       = (polarity == GPIO_INT_POLARITY_ACTIVE_LOW) ? GPIO_PuPd_UP : GPIO_PuPd_DOWN;
	init.GPIO_ITTrigger  = trigger;
	init.GPIO_ITPolarity = polarity;
	init.GPIO_ITDebounce = db_en ? GPIO_INT_DEBOUNCE_ENABLE : GPIO_INT_DEBOUNCE_DISABLE;
	GPIO_Init(&init);

	if (db_en) {
		GPIO_DebounceClock(PORT_NUM((u32)pin), 0); /* (0+1)*64 µs = 64 µs */
	}

	s_irq_trigger[idx] = (uint8_t)trigger;
	s_irq_polarity[idx] = (uint8_t)polarity;
	s_irq_debounce[idx] = db_en ? GPIO_INT_DEBOUNCE_ENABLE : GPIO_INT_DEBOUNCE_DISABLE;
	s_irq_count[idx]    = 0;
	s_gpio_inited[idx]  = 1;

	gpio_ensure_port_irq(PORT_NUM((u32)pin));
	GPIO_UserRegIrq((u32)pin, gpio_irq_cb, NULL);
	s_irq_inited[idx] = 1;
	GPIO_UNLOCK();

	return 0;
}

static int lgpio_irq_enable(lua_State *L)
{
	PinName pin = luhw_check_pin(L, 1);
	int     idx = (int)(u32)pin;
	if (idx < 0 || idx >= GPIO_PIN_MAX || !s_irq_inited[idx]) {
		return luaL_error(L, "irq not configured for pin, call set_irq first");
	}
	GPIO_LOCK();
	GPIO_INTConfig((u32)pin, ENABLE);
	GPIO_UNLOCK();
	return 0;
}

static int lgpio_irq_disable(lua_State *L)
{
	PinName pin = luhw_check_pin(L, 1);
	GPIO_LOCK();
	GPIO_INTConfig((u32)pin, DISABLE);
	GPIO_UNLOCK();
	return 0;
}

static int lgpio_get_irq_count(lua_State *L)
{
	PinName pin = luhw_check_pin(L, 1);
	int     idx = (int)(u32)pin;
	if (idx < 0 || idx >= GPIO_PIN_MAX) {
		return luaL_error(L, "pin index out of range");
	}
	lua_pushinteger(L, (lua_Integer)s_irq_count[idx]);
	return 1;
}

static int lgpio_clear_irq_count(lua_State *L)
{
	PinName pin = luhw_check_pin(L, 1);
	int     idx = (int)(u32)pin;
	if (idx < 0 || idx >= GPIO_PIN_MAX) {
		return luaL_error(L, "pin index out of range");
	}
	s_irq_count[idx] = 0;
	return 0;
}

static const luaL_Reg lgpio_funcs[] = {
	{"set_direction",   lgpio_set_direction},
	{"set_level",       lgpio_set_level},
	{"get_level",       lgpio_get_level},
	{"set_pull",        lgpio_set_pull},
	{"set_irq",         lgpio_set_irq},
	{"irq_enable",      lgpio_irq_enable},
	{"irq_disable",     lgpio_irq_disable},
	{"get_irq_count",   lgpio_get_irq_count},
	{"clear_irq_count", lgpio_clear_irq_count},
	{NULL, NULL}
};

/* ── Driver-level init (called once from lua_module_registry_provision_all,
 *      single-threaded boot phase, before any concurrent Lua execution). ──── */
void lua_driver_gpio_init(void)
{
	if (s_gpio_lock == NULL) {
		rtos_mutex_create(&s_gpio_lock);
	}
}

LUAMOD_API int luaopen_gpio(lua_State *L)
{
	luaL_newlib(L, lgpio_funcs);
	return 1;
}

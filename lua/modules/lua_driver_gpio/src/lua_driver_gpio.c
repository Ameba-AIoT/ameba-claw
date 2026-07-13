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
#include "ameba_claw_defs.h"
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

/* ── Event callback subsystem ─────────────────────────────────────────────────
 * ISR writes s_irq_pending[pin]=1 (single-byte, ISR-safe) and gives s_ev_sema
 * (ISR-safe, same pattern as lua_driver_ir).  gpio.dispatch() scans the pending
 * array and calls the registered Lua function in the SAME lua_State.
 * s_cb_state tracks which lua_State currently owns the callbacks; a new script
 * calling gpio.on() takes ownership and resets stale refs from the previous run.
 */
#define GPIO_EV_SEMA_MAX   64  /* counting semaphore ceiling */

static volatile uint8_t  s_irq_pending[GPIO_PIN_MAX]; /* set by ISR, cleared by dispatch */
static volatile uint8_t  s_irq_edge[GPIO_PIN_MAX];    /* 1=rise 2=fall, set by ISR */
static int               s_cb_ref[GPIO_PIN_MAX];       /* luaL_ref or LUA_NOREF per pin */
static lua_State        *s_cb_state = NULL;            /* owner State for s_cb_ref[] */
static rtos_sema_t       s_ev_sema  = NULL;            /* event-ready signal, created at init */

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
	/* Button subsystem routing (see lua_driver_gpio.h): if this pin is owned
	 * by the button bottom-half, the hook records the edge timestamp + wakes
	 * the consumer and we return early — button pins never use the legacy
	 * gpio.on path (the two are mutually exclusive per pin). */
	if (lua_btn_isr_hook(pin)) {
		return;
	}
	/* Count every fire — single-word write, ISR-safe, no lock. */
	s_irq_count[pin]++;
	/* Record pending event for gpio.dispatch() / event.wait(). Single-byte
	 * writes are ISR-safe on Cortex-M33.  edge bits 1:0 from event word. */
	s_irq_pending[pin] = 1;
	s_irq_edge[pin]    = (uint8_t)(event & 0x3u); /* 1=rise, 2=fall */
	/* Signal event.wait() if any callback is registered for this pin.
	 * rtos_sema_give is ISR-safe (same pattern used by lua_driver_ir). */
	if (s_cb_ref[pin] != LUA_NOREF && s_ev_sema) {
		rtos_sema_give(s_ev_sema);
	}
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

/* Internal helper — configure IRQ hardware for a pin.  Caller must hold GPIO_LOCK.
 * Used by both lgpio_set_irq (Lua API) and lgpio_on (auto-configure). */
static void gpio_setup_irq_hw(u32 pin, u32 trigger, u32 polarity, int db_en)
{
	int idx = (int)pin;
	gpio_ensure_clk();
	GPIO_INTConfig(pin, DISABLE);

	GPIO_InitTypeDef init;
	init.GPIO_Pin        = pin;
	init.GPIO_Mode       = GPIO_Mode_INT;
	init.GPIO_PuPd       = (polarity == GPIO_INT_POLARITY_ACTIVE_LOW)
	                       ? GPIO_PuPd_UP : GPIO_PuPd_DOWN;
	init.GPIO_ITTrigger  = trigger;
	init.GPIO_ITPolarity = polarity;
	init.GPIO_ITDebounce = db_en ? GPIO_INT_DEBOUNCE_ENABLE : GPIO_INT_DEBOUNCE_DISABLE;
	GPIO_Init(&init);
	if (db_en) {
		GPIO_DebounceClock(PORT_NUM(pin), 0); /* (0+1)*64 µs = 64 µs */
	}

	s_irq_trigger[idx]  = (uint8_t)trigger;
	s_irq_polarity[idx] = (uint8_t)polarity;
	s_irq_debounce[idx] = db_en ? GPIO_INT_DEBOUNCE_ENABLE : GPIO_INT_DEBOUNCE_DISABLE;
	s_irq_count[idx]    = 0;
	s_irq_pending[idx]  = 0;
	s_gpio_inited[idx]  = 1;

	gpio_ensure_port_irq(PORT_NUM(pin));
	GPIO_UserRegIrq(pin, gpio_irq_cb, NULL);
	s_irq_inited[idx] = 1;
}

/* ── Button subsystem HW hooks (see lua_driver_gpio.h) ───────────────────────
 * Thin wrappers so lua_driver_gpio_button.c never touches GPIO_LOCK or the
 * static gpio_setup_irq_hw directly.  Called only while button.c holds
 * s_btn_lock; these take GPIO_LOCK (inner) — fixed order, no deadlock. */
void lua_gpio_btn_setup_hw(unsigned int pin, int active_low)
{
	u32 polarity = active_low ? GPIO_INT_POLARITY_ACTIVE_LOW
	                          : GPIO_INT_POLARITY_ACTIVE_HIGH;
	GPIO_LOCK();
	/* BOTHEDGE: button needs both press and release; polarity only selects the
	 * internal pull (active_low -> pull-up).  Override the default 64 µs debounce
	 * window to the full mechanical-bounce window (~8.2 ms, PORT-wide). */
	gpio_setup_irq_hw((u32)pin, GPIO_INT_Trigger_BOTHEDGE, polarity, 1);
	GPIO_DebounceClock(PORT_NUM(pin), CLAW_BTN_HW_DEBOUNCE_DIV_COUNT);
	GPIO_INTConfig((u32)pin, ENABLE);
	GPIO_UNLOCK();
}

void lua_gpio_btn_teardown_hw(unsigned int pin)
{
	GPIO_LOCK();
	GPIO_INTConfig((u32)pin, DISABLE);
	GPIO_UNLOCK();
}

/*
** gpio.set_irq(pin, trigger_str [, debounce_en])
**   trigger_str: "rising" | "falling" | "both" | "level_high" | "level_low"
**   debounce_en: 1 (default) or 0 — pass 0 to disable ~64 µs debounce filter
**
** Configures the pin in interrupt mode and registers the ISR callback.
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
	gpio_setup_irq_hw((u32)pin, trigger, polarity, db_en);
	GPIO_UNLOCK();
	return 0;
}

/* ── gpio.on / gpio.off / gpio.dispatch ──────────────────────────────────────
 *
 * gpio.on(pin, edge_str, fn)
 *   Register a Lua callback for GPIO edge events on the given pin.
 *   edge_str: "rising" | "falling" | "both"
 *   Configures the IRQ if not already done.  Call gpio.irq_enable(pin) after.
 *   At most one script can own GPIO callbacks at a time; calling gpio.on() from
 *   a new lua_State takes ownership and clears any stale refs from the previous
 *   run (which are safe to abandon — they GC when that State closes).
 *
 * gpio.off(pin)
 *   Unregister the callback, disable the interrupt.
 *
 * gpio.dispatch()  →  integer (events dispatched)
 *   Non-blocking.  Scans all pins for pending events; for each found, calls the
 *   registered Lua callback in the current lua_State and returns count.  Use
 *   event.wait(timeout_ms) for a blocking alternative.
 */

static int lgpio_on(lua_State *L)
{
	PinName     pin      = luhw_check_pin(L, 1);
	const char *edge_str = luaL_checkstring(L, 2);
	luaL_checktype(L, 3, LUA_TFUNCTION);
	int idx = (int)(u32)pin;

	if (idx < 0 || idx >= GPIO_PIN_MAX) {
		return luaL_error(L, "pin index out of range");
	}

	u32 trigger, polarity;
	if (strcmp(edge_str, "rising") == 0) {
		trigger  = GPIO_INT_Trigger_EDGE;
		polarity = GPIO_INT_POLARITY_ACTIVE_HIGH;
	} else if (strcmp(edge_str, "falling") == 0) {
		trigger  = GPIO_INT_Trigger_EDGE;
		polarity = GPIO_INT_POLARITY_ACTIVE_LOW;
	} else if (strcmp(edge_str, "both") == 0) {
		trigger  = GPIO_INT_Trigger_BOTHEDGE;
		/* For BOTHEDGE polarity is ignored by the HW, but it controls the
		 * GPIO_PuPd selection in gpio_setup_irq_hw.  Use ACTIVE_LOW so the
		 * pin is pulled UP — correct for the dominant use-case of active-low
		 * push-buttons.  Users needing pull-down can call gpio.set_pull(). */
		polarity = GPIO_INT_POLARITY_ACTIVE_LOW;
	} else {
		return luaL_error(L, "invalid edge '%s' (rising|falling|both)", edge_str);
	}

	/* Ownership transfer: if a different State had callbacks, reset all refs
	 * without unref-ing (old State may already be closed, unref would crash).
	 * The abandoned luaL_refs become unreachable and GC once the old State
	 * is lua_close()'d — no leak. */
	if (s_cb_state != L) {
		for (int i = 0; i < GPIO_PIN_MAX; i++) {
			s_cb_ref[i] = LUA_NOREF;
		}
		s_cb_state = L;
	}

	/* Replace existing callback for this pin (unref safely, same State). */
	if (s_cb_ref[idx] != LUA_NOREF) {
		luaL_unref(L, LUA_REGISTRYINDEX, s_cb_ref[idx]);
	}
	lua_pushvalue(L, 3);
	s_cb_ref[idx] = luaL_ref(L, LUA_REGISTRYINDEX);

	/* Auto-configure IRQ: initialise on first use, or re-configure when the
	 * trigger type changes (e.g. a previous script used "falling" and the new
	 * one requests "both" — without reconfiguring, the hardware would silently
	 * keep firing on one edge only and the second edge would never arrive). */
	GPIO_LOCK();
	if (!s_irq_inited[idx] || s_irq_trigger[idx] != (uint8_t)trigger) {
		gpio_setup_irq_hw((u32)pin, trigger, polarity, 1 /* debounce */);
	}
	GPIO_UNLOCK();

	lua_pushboolean(L, 1);
	return 1;
}

static int lgpio_off(lua_State *L)
{
	PinName pin = luhw_check_pin(L, 1);
	int     idx = (int)(u32)pin;
	if (idx < 0 || idx >= GPIO_PIN_MAX) {
		return luaL_error(L, "pin index out of range");
	}

	if (s_cb_state == L && s_cb_ref[idx] != LUA_NOREF) {
		luaL_unref(L, LUA_REGISTRYINDEX, s_cb_ref[idx]);
		s_cb_ref[idx] = LUA_NOREF;
	}
	s_irq_pending[idx] = 0;

	GPIO_LOCK();
	GPIO_INTConfig((u32)pin, DISABLE);
	GPIO_UNLOCK();

	lua_pushboolean(L, 1);
	return 1;
}

/* Internal dispatch — returns count of callbacks called.  Does NOT push to stack.
 * Called by both lgpio_dispatch (Lua wrapper) and lua_gpio_dispatch (export). */
static int gpio_dispatch_internal(lua_State *L)
{
	if (s_cb_state != L) {
		return 0;
	}
	int count = 0;
	for (int pin = 0; pin < GPIO_PIN_MAX; pin++) {
		if (!s_irq_pending[pin]) {
			continue;
		}
		uint8_t edge = s_irq_edge[pin];
		s_irq_pending[pin] = 0; /* clear before callback to avoid missing next event */
		if (s_cb_ref[pin] == LUA_NOREF) {
			continue;
		}
		lua_rawgeti(L, LUA_REGISTRYINDEX, s_cb_ref[pin]);
		if (!lua_isfunction(L, -1)) {
			lua_pop(L, 1);
			continue;
		}
		char pin_buf[8];
		lua_newtable(L);
		lua_pushstring(L, luhw_pin_to_str(pin, pin_buf, sizeof(pin_buf)));
		lua_setfield(L, -2, "pin");
		lua_pushstring(L, edge == 1u ? "rise" : "fall"); lua_setfield(L, -2, "edge");
		lua_pushstring(L, "gpio");                      lua_setfield(L, -2, "type");
		if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
			RTK_LOGW("gpio", "dispatch cb error pin %d: %s\n",
			         pin, lua_tostring(L, -1));
			lua_pop(L, 1);
		}
		count++;
	}
	return count;
}

static int lgpio_dispatch(lua_State *L)
{
	lua_pushinteger(L, gpio_dispatch_internal(L));
	return 1;
}

/* Exported for lua_module_event (event.wait): returns s_ev_sema handle. */
void *lua_gpio_get_ev_sema(void)
{
	return (void *)s_ev_sema;
}

/* Exported for lua_module_event (event.wait): dispatch pending events in L. */
int lua_gpio_dispatch(lua_State *L)
{
	return gpio_dispatch_internal(L);
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
	{"on",              lgpio_on},
	{"off",             lgpio_off},
	{"dispatch",        lgpio_dispatch},
	{NULL, NULL}
};

/* ── Driver-level init (called once from lua_module_registry_provision_all,
 *      single-threaded boot phase, before any concurrent Lua execution). ──── */
void lua_driver_gpio_init(void)
{
	if (s_gpio_lock == NULL) {
		rtos_mutex_create(&s_gpio_lock);
	}
	/* Create the event counting semaphore once at boot. */
	if (s_ev_sema == NULL) {
		rtos_sema_create(&s_ev_sema, 0, GPIO_EV_SEMA_MAX);
	}
	/* Initialize all callback refs to LUA_NOREF. */
	for (int i = 0; i < GPIO_PIN_MAX; i++) {
		s_cb_ref[i] = LUA_NOREF;
	}
}

LUAMOD_API int luaopen_gpio(lua_State *L)
{
	luaL_newlib(L, lgpio_funcs);
	return 1;
}

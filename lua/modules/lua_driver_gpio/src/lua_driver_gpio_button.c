/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_driver_gpio_button.c — interrupt-driven push-button engine for ameba_claw.
**
** Provides require("button"):
**   button.on(pin, type [, fn])   subscribe a semantic/raw event on a pin; with
**                                 fn also register a callback (use button.dispatch),
**                                 without fn just subscribe (use button.get_event).
**                                 First on() of a pin implicitly takes it over and
**                                 configures the hardware.
**   button.off([pin])             unregister one / all pins.
**   button.get_event([timeout_ms])  pull ONE finished event (table {pin,type,hold_ms})
**                                 or nil.  0/absent = non-blocking; >0 = block until
**                                 an event arrives or the timeout elapses.
**   button.get_level(pin)         instantaneous logical-pressed state (1 = pressed),
**                                 polarity-aware (CLAW_BTN_ACTIVE_LEVEL); not queued.
**   button.flush()                drop all queued events + reset every FSM to idle
**                                 (call on page / context switch).
**   button.dispatch()             drain the queue and invoke registered callbacks for
**                                 pins owned by the calling lua_State; returns count.
**   button.events()               iterator: for ev in button.events() do ... end.
**
** Event types (ev.type string):
**   raw      : "down" / "up"                       (opt-in)
**   semantic : "click" / "double" / "long_press" / "hold"   (default tier)
**
** ── Architecture (see lua/modules/lua_driver_gpio/docs/button.md) ────────────
** Debounce is done in HARDWARE: the GPIO debounce filter (CLAW_BTN_HW_DEBOUNCE_
** DIV_COUNT, ~8.2 ms) only fires the ISR after the line has been stable that
** long, so there is no software settle window.
** Top half (ISR, lua_btn_isr_hook): for an owned pin, record the edge timestamp
** (DTimestamp_Get, 1 µs) + set a dirty flag + give a counting semaphore.  No
** computation, no lock.
** Bottom half (btn_drain, runs in the consumer lua_State): the ISR stores only a
** timestamp (not the direction), so on a dirty pin we resample the real level
** once to learn press vs release, then run the click/double/long_press/hold
** state machine.  Time events (long/double/hold) are settled lazily — get_event
** computes the nearest deadline and blocks on the semaphore for exactly that
** long, so there is no software timer and no background task.
**
** ── Concurrency ──────────────────────────────────────────────────────────────
** The gpio module (and therefore this one) is loaded into several lua_States
** (REPL / timer / skill sandbox) that may run concurrently, so the button
** registry is guarded by a private mutex s_btn_lock: on / off / flush / drain
** all hold it for their whole sequence.  The ISR hook takes NO lock (single-word
** writes only).  When on() configures hardware it calls lua_gpio_btn_setup_hw,
** which takes gpio.c's GPIO_LOCK — lock order is fixed s_btn_lock -> GPIO_LOCK,
** never the reverse, so the two cannot deadlock.
*/

#define LUA_LIB
#include "lua.h"
#include "lauxlib.h"

#include "lua_driver_gpio.h"
#include "ameba_soc.h"
#include "luhw.h"
#include "os_wrapper.h"
#include "ameba_claw_defs.h"
#include <string.h>

#if CLAW_BTN_DEBUG
#define BTN_DBG(fmt, ...)  RTK_LOGI("btn", fmt, ##__VA_ARGS__)
#else
#define BTN_DBG(fmt, ...)  do {} while (0)
#endif

#define GPIO_PIN_MAX        PIN_TOTAL_NUM
#define BTN_LONG_US         (CLAW_BTN_LONG_MS * 1000u)
#define BTN_DOUBLE_GAP_US   (CLAW_BTN_DOUBLE_GAP_MS * 1000u)
#define BTN_HOLD_REPEAT_US  (CLAW_BTN_HOLD_REPEAT_MS * 1000u)

/* ── Event types — index into sub_mask bits and lua_ref[] ─────────────────── */
enum {
	BTN_EV_DOWN = 0,   /* raw: press confirmed   */
	BTN_EV_UP,         /* raw: release confirmed */
	BTN_EV_CLICK,      /* semantic: emitted EAGERLY on release (zero latency) */
	BTN_EV_DOUBLE,     /* semantic: appended after the 2nd click in the window */
	BTN_EV_LONG,       /* semantic: long press (once) */
	BTN_EV_HOLD,       /* semantic: repeat while held */
	BTN_EV_NTYPES
};

static const char *const s_type_names[BTN_EV_NTYPES] = {
	"down", "up", "click", "double", "long_press", "hold"
};

/* ── FSM states ───────────────────────────────────────────────────────────── */
enum { BTN_IDLE = 0, BTN_PRESSED, BTN_WAIT_DOUBLE, BTN_HELD };

/* ── Finished event (queue element) ───────────────────────────────────────── */
typedef struct {
	uint16_t pin;
	uint8_t  type;
	uint32_t hold_ms;   /* press duration so far (up/long_press/hold), else 0 */
} btn_event_t;

/* ── Per-pin registry entry (guarded by s_btn_lock) ───────────────────────── */
typedef struct {
	uint8_t     used;
	uint16_t    pin;                 /* PinName index */
	lua_State  *owner_state;         /* State that owns lua_ref[] / callbacks  */
	uint8_t     sub_mask;            /* OR of (1 << BTN_EV_*) — emitted types   */
	int         lua_ref[BTN_EV_NTYPES];

	uint8_t     state;               /* BTN_IDLE / PRESSED / WAIT_DOUBLE / HELD */
	uint8_t     confirmed_pressed;   /* last debounced logical level            */
	uint8_t     click_cnt;
	uint32_t    down_at_us;
	uint32_t    last_up_us;
	uint32_t    last_hold_us;
} btn_reg_t;

/* ── ISR-written state, indexed by raw pin (no lock; single-word writes) ──── */
static volatile uint8_t  s_btn_owned[GPIO_PIN_MAX];
static volatile uint8_t  s_btn_edge_dirty[GPIO_PIN_MAX];
static volatile uint32_t s_btn_last_edge_us[GPIO_PIN_MAX];

/* ── Registry + sync primitives ───────────────────────────────────────────── */
static btn_reg_t      s_reg[CLAW_BTN_MAX_PINS];
static rtos_mutex_t   s_btn_lock;   /* registry guard (NULL-safe before init)  */
static rtos_sema_t    s_btn_sema;   /* wakeup-only signal, NOT an event count   */
static rtos_queue_t   s_evq;        /* finished events                          */

#define BTN_LOCK()    do { if (s_btn_lock) rtos_mutex_take(s_btn_lock, RTOS_MAX_DELAY); } while (0)
#define BTN_UNLOCK()  do { if (s_btn_lock) rtos_mutex_give(s_btn_lock); } while (0)

/* ── Time helpers (1 µs free-run timer; u32 subtraction is wrap-safe for the
 *    sub-2 s windows used here) ──────────────────────────────────────────── */
static inline uint32_t btn_now_us(void)            { return DTimestamp_Get(); }
static inline uint32_t btn_since_us(uint32_t t0)   { return (uint32_t)(DTimestamp_Get() - t0); }

static inline int btn_raw_pressed(unsigned int pin)
{
	/* Unlocked single-register read — atomic on M33, independent of config
	 * (see GPIO_LOCK rationale in lua_driver_gpio.c). */
	return (GPIO_ReadDataBit((u32)pin) == CLAW_BTN_ACTIVE_LEVEL) ? 1 : 0;
}

/* ── Top half: ISR hook (called from gpio.c's gpio_irq_cb) ─────────────────── */
int lua_btn_isr_hook(unsigned int pin)
{
	if (pin >= (unsigned int)GPIO_PIN_MAX || !s_btn_owned[pin]) {
		return 0;
	}
	s_btn_last_edge_us[pin] = btn_now_us();   /* drain uses this as settle base */
	s_btn_edge_dirty[pin]   = 1;              /* direction irrelevant: resample */
	if (s_btn_sema) {
		rtos_sema_give(s_btn_sema);            /* wake any blocked get_event     */
	}
	return 1;
}

/* ── Registry lookup (caller holds s_btn_lock) ────────────────────────────── */
static btn_reg_t *btn_find(unsigned int pin)
{
	for (int i = 0; i < CLAW_BTN_MAX_PINS; i++) {
		if (s_reg[i].used && s_reg[i].pin == (uint16_t)pin) {
			return &s_reg[i];
		}
	}
	return NULL;
}

static btn_reg_t *btn_alloc(unsigned int pin)
{
	for (int i = 0; i < CLAW_BTN_MAX_PINS; i++) {
		if (!s_reg[i].used) {
			btn_reg_t *r = &s_reg[i];
			memset(r, 0, sizeof(*r));
			r->used = 1;
			r->pin  = (uint16_t)pin;
			r->state = BTN_IDLE;
			for (int t = 0; t < BTN_EV_NTYPES; t++) {
				r->lua_ref[t] = LUA_NOREF;
			}
			return r;
		}
	}
	return NULL;
}

/* ── Emit a finished event (caller holds s_btn_lock) ──────────────────────── */
static void btn_emit(btn_reg_t *r, int type)
{
	if (!(r->sub_mask & (1u << type))) {
		return;   /* not subscribed — never enters the queue */
	}
	btn_event_t ev;
	ev.pin  = r->pin;
	ev.type = (uint8_t)type;
	ev.hold_ms = (type == BTN_EV_UP || type == BTN_EV_LONG || type == BTN_EV_HOLD)
	             ? btn_since_us(r->down_at_us) / 1000u
	             : 0u;
	if (ev.hold_ms) {
		BTN_DBG("emit %s pin %u hold %ums\n", s_type_names[type], (unsigned)r->pin, (unsigned)ev.hold_ms);
	} else {
		BTN_DBG("emit %s pin %u\n", s_type_names[type], (unsigned)r->pin);
	}
	if (s_evq && rtos_queue_send(s_evq, &ev, 0) != RTK_SUCCESS) {
		static uint8_t warned;
		if (!warned) {
			warned = 1;
			RTK_LOGW("btn", "event queue full, dropping %s (pin %u)\n",
			         s_type_names[type], (unsigned)r->pin);
		}
	}
}

/* ── FSM: confirmed level change (caller holds s_btn_lock) ────────────────── */
static void btn_feed_fsm(btn_reg_t *r, int pressed, uint32_t now)
{
	switch (r->state) {
	case BTN_IDLE:
		if (pressed) {
			r->state = BTN_PRESSED;
			r->down_at_us = now;
			btn_emit(r, BTN_EV_DOWN);
		}
		break;
	case BTN_PRESSED:
		if (!pressed) {
			r->click_cnt++;
			btn_emit(r, BTN_EV_UP);
			/* Eager (mouse-style) click: the click is emitted the instant the
			 * button is released, with no added latency.  The double-gap window
			 * is then used ONLY to decide whether to *append* a double event —
			 * it never holds the click back.  (A double-click therefore arrives
			 * as click + double, exactly like Win32 WM_LBUTTONUP + DBLCLK.) */
			int wants_double = (r->sub_mask & (1u << BTN_EV_DOUBLE)) != 0;
			if (r->click_cnt >= 2) {
				/* Second release inside the window completes the double; the
				 * first release already emitted the eager click, so only the
				 * double is added here. */
				btn_emit(r, BTN_EV_DOUBLE);
				r->click_cnt = 0;
				r->state = BTN_IDLE;
			} else {
				btn_emit(r, BTN_EV_CLICK);          /* fire immediately */
				if (wants_double) {
					r->state = BTN_WAIT_DOUBLE;     /* arm second-click window */
					r->last_up_us = now;
				} else {
					r->click_cnt = 0;               /* nobody wants double — done */
					r->state = BTN_IDLE;
				}
			}
		}
		break;
	case BTN_WAIT_DOUBLE:
		if (pressed) {
			r->state = BTN_PRESSED;
			r->down_at_us = now;
			/* click_cnt stays 1; the upcoming release bumps it to 2 → double */
			btn_emit(r, BTN_EV_DOWN);
		}
		break;
	case BTN_HELD:
		if (!pressed) {
			r->state = BTN_IDLE;
			r->click_cnt = 0;          /* long-press release: no trailing click */
			btn_emit(r, BTN_EV_UP);
		}
		break;
	default:
		break;
	}
}

/* ── FSM: time-deadline settlement (caller holds s_btn_lock) ──────────────── */
static void btn_check_deadlines(btn_reg_t *r)
{
	switch (r->state) {
	case BTN_PRESSED:
		if (btn_since_us(r->down_at_us) >= BTN_LONG_US) {
			btn_emit(r, BTN_EV_LONG);
			r->state = BTN_HELD;
			r->last_hold_us = btn_now_us();
			r->click_cnt = 0;          /* drop any pending click count           */
		}
		break;
	case BTN_HELD:
		if (btn_since_us(r->last_hold_us) >= BTN_HOLD_REPEAT_US) {
			btn_emit(r, BTN_EV_HOLD);
			r->last_hold_us = btn_now_us();
		}
		break;
	case BTN_WAIT_DOUBLE:
		if (btn_since_us(r->last_up_us) >= BTN_DOUBLE_GAP_US) {
			/* Window closed with no second click.  The eager click was already
			 * emitted on release, so there is nothing to append — just disarm. */
			r->click_cnt = 0;
			r->state = BTN_IDLE;
		}
		break;
	default:
		break;
	}
}

/* ── Bottom half: advance every pin's debounce + FSM (takes s_btn_lock) ───── */
static void btn_drain(void)
{
	BTN_LOCK();
	for (int i = 0; i < CLAW_BTN_MAX_PINS; i++) {
		btn_reg_t *r = &s_reg[i];
		if (!r->used) {
			continue;
		}
		unsigned int pin = r->pin;

		/* (A) debounce — HW circuit fires ISR only after ~8.2 ms stable; sample now */
		if (s_btn_edge_dirty[pin]) {
			s_btn_edge_dirty[pin] = 0;
			BTN_DBG("edge pin %u\n", pin);
			int pressed = btn_raw_pressed(pin);
			BTN_DBG("settled pin %u lvl %d\n", pin, pressed);
			if (pressed != r->confirmed_pressed) {
				r->confirmed_pressed = (uint8_t)pressed;
				btn_feed_fsm(r, pressed, btn_now_us());
			}
		}

		/* (B) time-event settlement */
		btn_check_deadlines(r);
	}
	BTN_UNLOCK();
}

/* ── Nearest pending deadline across all pins, in ms (rounded up); UINT32_MAX
 *    if no pin has a pending deadline.  Takes s_btn_lock. ─────────────────── */
static uint32_t btn_next_wake_ms(void)
{
	uint32_t best_us = 0xFFFFFFFFu;
	BTN_LOCK();
	for (int i = 0; i < CLAW_BTN_MAX_PINS; i++) {
		btn_reg_t *r = &s_reg[i];
		if (!r->used) {
			continue;
		}
		uint32_t rem;
		switch (r->state) {
		case BTN_PRESSED: {
			uint32_t e = btn_since_us(r->down_at_us);
			rem = (e >= BTN_LONG_US) ? 0u : (BTN_LONG_US - e);
			if (rem < best_us) best_us = rem;
			break;
		}
		case BTN_HELD: {
			uint32_t e = btn_since_us(r->last_hold_us);
			rem = (e >= BTN_HOLD_REPEAT_US) ? 0u : (BTN_HOLD_REPEAT_US - e);
			if (rem < best_us) best_us = rem;
			break;
		}
		case BTN_WAIT_DOUBLE: {
			uint32_t e = btn_since_us(r->last_up_us);
			rem = (e >= BTN_DOUBLE_GAP_US) ? 0u : (BTN_DOUBLE_GAP_US - e);
			if (rem < best_us) best_us = rem;
			break;
		}
		default:
			break;
		}
	}
	BTN_UNLOCK();
	if (best_us == 0xFFFFFFFFu) {
		return 0xFFFFFFFFu;
	}
	return best_us / 1000u + 1u;   /* round up to >=1 ms */
}

/* Push a btn_event_t as a Lua table {pin, type, hold_ms}. */
static void btn_push_event(lua_State *L, const btn_event_t *ev)
{
	char pin_buf[8];
	lua_newtable(L);
	lua_pushstring(L, luhw_pin_to_str(ev->pin, pin_buf, sizeof(pin_buf)));
	lua_setfield(L, -2, "pin");
	lua_pushstring(L, ev->type < BTN_EV_NTYPES ? s_type_names[ev->type] : "?");
	lua_setfield(L, -2, "type");
	lua_pushinteger(L, (lua_Integer)ev->hold_ms);          lua_setfield(L, -2, "hold_ms");
}

/* Cooperative-cancel check (same hook as event.wait / sys.sleep_ms). */
static void btn_check_cancel(lua_State *L)
{
	lua_getfield(L, LUA_REGISTRYINDEX, "__cancel_ptr");
	volatile int *cp = (volatile int *)lua_touserdata(L, -1);
	lua_pop(L, 1);
	if (cp && *cp) {
		luaL_error(L, "button.get_event cancelled");
	}
}

/* Shared get_event body: drain, then return one event or block to timeout. */
static int btn_get_event_impl(lua_State *L, uint32_t timeout_ms)
{
	uint32_t start_us = btn_now_us();
	for (;;) {
		btn_drain();

		btn_event_t ev;
		if (s_evq && rtos_queue_receive(s_evq, &ev, 0) == RTK_SUCCESS) {
			btn_push_event(L, &ev);
			return 1;
		}
		if (timeout_ms == 0) {
			lua_pushnil(L);
			return 1;
		}

		uint32_t elapsed_ms = btn_since_us(start_us) / 1000u;
		if (elapsed_ms >= timeout_ms) {
			lua_pushnil(L);
			return 1;
		}
		uint32_t remaining = timeout_ms - elapsed_ms;
		uint32_t wake = remaining;
		uint32_t nd = btn_next_wake_ms();
		if (nd < wake) wake = nd;
		if (wake > CLAW_BTN_WAKE_MAX_MS) wake = CLAW_BTN_WAKE_MAX_MS;
		if (wake < CLAW_BTN_WAKE_MIN_MS) wake = CLAW_BTN_WAKE_MIN_MS;

		if (s_btn_sema) {
			rtos_sema_take(s_btn_sema, wake);   /* wakes on edge or timeout */
		}
		btn_check_cancel(L);
	}
}

/* ── Lua: button.on(pin, type [, fn]) ─────────────────────────────────────── */
static int btn_parse_type(const char *s)
{
	for (int t = 0; t < BTN_EV_NTYPES; t++) {
		if (strcmp(s, s_type_names[t]) == 0) {
			return t;
		}
	}
	return -1;
}

static int lbtn_on(lua_State *L)
{
	PinName     pin  = luhw_check_pin(L, 1);
	const char *tstr = luaL_checkstring(L, 2);
	int has_fn = !lua_isnoneornil(L, 3);
	if (has_fn) {
		luaL_checktype(L, 3, LUA_TFUNCTION);
	}
	int type = btn_parse_type(tstr);
	if (type < 0) {
		return luaL_error(L,
		    "invalid button event '%s' (down|up|click|double|long_press|hold)", tstr);
	}

	BTN_LOCK();
	btn_reg_t *r = btn_find((unsigned int)pin);
	if (!r) {
		r = btn_alloc((unsigned int)pin);
		if (!r) {
			BTN_UNLOCK();
			lua_pushnil(L);
			lua_pushfstring(L, "too many button pins (max %d)", CLAW_BTN_MAX_PINS);
			return 2;
		}
		/* First takeover of this pin: configure HW, seed confirmed level, then
		 * publish ownership LAST so the ISR can't observe a half-built entry. */
		DTimer_Cmd(ENABLE);                          /* idempotent timer guard  */
		lua_gpio_btn_setup_hw((unsigned int)pin, CLAW_BTN_ACTIVE_LEVEL == 0 ? 1 : 0);
		r->confirmed_pressed = (uint8_t)btn_raw_pressed((unsigned int)pin);
		s_btn_last_edge_us[(unsigned int)pin] = btn_now_us();
		s_btn_edge_dirty[(unsigned int)pin]   = 0;
		s_btn_owned[(unsigned int)pin]        = 1;   /* <-- last */
	}

	/* Ownership: refs are per-State.  A new owner abandons stale refs (the old
	 * State may already be closed — unref would be unsafe; they GC with it). */
	if (r->owner_state != L) {
		for (int t = 0; t < BTN_EV_NTYPES; t++) {
			r->lua_ref[t] = LUA_NOREF;
		}
		r->owner_state = L;
	}

	r->sub_mask |= (uint8_t)(1u << type);
	if (has_fn) {
		if (r->lua_ref[type] != LUA_NOREF) {
			luaL_unref(L, LUA_REGISTRYINDEX, r->lua_ref[type]);
		}
		lua_pushvalue(L, 3);
		r->lua_ref[type] = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	BTN_UNLOCK();

	lua_pushboolean(L, 1);
	return 1;
}

/* Tear down one registry entry (caller holds s_btn_lock). */
static void btn_release(lua_State *L, btn_reg_t *r)
{
	unsigned int pin = r->pin;
	s_btn_owned[pin] = 0;                 /* stop ISR routing first */
	lua_gpio_btn_teardown_hw(pin);
	if (r->owner_state == L) {
		for (int t = 0; t < BTN_EV_NTYPES; t++) {
			if (r->lua_ref[t] != LUA_NOREF) {
				luaL_unref(L, LUA_REGISTRYINDEX, r->lua_ref[t]);
			}
		}
	}
	memset(r, 0, sizeof(*r));
	for (int t = 0; t < BTN_EV_NTYPES; t++) {
		r->lua_ref[t] = LUA_NOREF;
	}
}

/* ── Lua: button.off([pin]) ───────────────────────────────────────────────── */
static int lbtn_off(lua_State *L)
{
	BTN_LOCK();
	if (!lua_isnoneornil(L, 1)) {
		PinName pin = luhw_check_pin(L, 1);
		btn_reg_t *r = btn_find((unsigned int)pin);
		if (r) {
			btn_release(L, r);
		}
	} else {
		for (int i = 0; i < CLAW_BTN_MAX_PINS; i++) {
			if (s_reg[i].used) {
				btn_release(L, &s_reg[i]);
			}
		}
	}
	BTN_UNLOCK();
	lua_pushboolean(L, 1);
	return 1;
}

/* ── Lua: button.get_event([timeout_ms]) ──────────────────────────────────── */
static int lbtn_get_event(lua_State *L)
{
	uint32_t timeout_ms = (uint32_t)luaL_optinteger(L, 1, 0);
	return btn_get_event_impl(L, timeout_ms);
}

/* ── Lua: button.get_level(pin) ───────────────────────────────────────────── */
static int lbtn_get_level(lua_State *L)
{
	PinName pin = luhw_check_pin(L, 1);
	lua_pushinteger(L, btn_raw_pressed((unsigned int)pin));
	return 1;
}

/* ── Lua: button.flush() — global reset (drop queue + reset every FSM) ────── */
static int lbtn_flush(lua_State *L)
{
	(void)L;
	BTN_LOCK();
	if (s_evq) {
		btn_event_t ev;
		while (rtos_queue_receive(s_evq, &ev, 0) == RTK_SUCCESS) { /* drain */ }
	}
	for (int i = 0; i < CLAW_BTN_MAX_PINS; i++) {
		btn_reg_t *r = &s_reg[i];
		if (!r->used) {
			continue;
		}
		r->state = BTN_IDLE;
		r->click_cnt = 0;
		s_btn_edge_dirty[r->pin] = 0;
		/* Reset to the CURRENT real level so a held button doesn't fake an up. */
		r->confirmed_pressed = (uint8_t)btn_raw_pressed(r->pin);
	}
	BTN_UNLOCK();
	return 0;
}

/* ── Lua: button.dispatch() — drain queue, invoke owner-State callbacks ───── */
static int lbtn_dispatch(lua_State *L)
{
	int count = 0;
	btn_drain();
	if (!s_evq) {
		lua_pushinteger(L, 0);
		return 1;
	}
	btn_event_t ev;
	while (rtos_queue_receive(s_evq, &ev, 0) == RTK_SUCCESS) {
		int ref = LUA_NOREF;
		BTN_LOCK();
		btn_reg_t *r = btn_find(ev.pin);
		if (r && r->owner_state == L && ev.type < BTN_EV_NTYPES) {
			ref = r->lua_ref[ev.type];
		}
		BTN_UNLOCK();
		if (ref == LUA_NOREF) {
			continue;   /* not our pin / no callback — event consumed */
		}
		lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
		if (!lua_isfunction(L, -1)) {
			lua_pop(L, 1);
			continue;
		}
		btn_push_event(L, &ev);
		if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
			RTK_LOGW("btn", "dispatch cb error pin %u: %s\n",
			         (unsigned)ev.pin, lua_tostring(L, -1));
			lua_pop(L, 1);
		}
		count++;
	}
	lua_pushinteger(L, count);
	return 1;
}

/* ── Lua: button.events() — iterator wrapping non-blocking get_event ──────── */
static int lbtn_events_iter(lua_State *L)
{
	return btn_get_event_impl(L, 0);
}

static int lbtn_events(lua_State *L)
{
	lua_pushcclosure(L, lbtn_events_iter, 0);
	return 1;
}

static const luaL_Reg lbtn_funcs[] = {
	{"on",        lbtn_on},
	{"off",       lbtn_off},
	{"get_event", lbtn_get_event},
	{"get_level", lbtn_get_level},
	{"flush",     lbtn_flush},
	{"dispatch",  lbtn_dispatch},
	{"events",    lbtn_events},
	{NULL, NULL}
};

/* ── Boot-time init (single-threaded; called from provision_all) ──────────── */
void lua_driver_gpio_button_init(void)
{
	if (s_btn_lock == NULL) {
		rtos_mutex_create(&s_btn_lock);
	}
	if (s_btn_sema == NULL) {
		/* counting sema: wakeup-only signal, give-on-full failure is harmless */
		rtos_sema_create(&s_btn_sema, 0, CLAW_BTN_EVENT_QUEUE_DEPTH * 2u);
	}
	if (s_evq == NULL) {
		rtos_queue_create(&s_evq, CLAW_BTN_EVENT_QUEUE_DEPTH, sizeof(btn_event_t));
	}
	for (int i = 0; i < CLAW_BTN_MAX_PINS; i++) {
		s_reg[i].used = 0;
		for (int t = 0; t < BTN_EV_NTYPES; t++) {
			s_reg[i].lua_ref[t] = LUA_NOREF;
		}
	}
	/* DTimer is enabled lazily on first button.on(); SDK usually has it on. */
}

LUAMOD_API int luaopen_button(lua_State *L)
{
	luaL_newlib(L, lbtn_funcs);
	return 1;
}

/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int luaopen_gpio(lua_State *L);

/* Create the GPIO concurrency mutex and event semaphore.  Call once during the
 * single-threaded boot phase (from lua_module_registry_provision_all) before any
 * concurrent Lua execution can reach the gpio.* APIs. */
void lua_driver_gpio_init(void);

/* Event subsystem exports — used by lua_module_event for event.wait().
 *
 * lua_gpio_get_ev_sema() returns the counting semaphore that the GPIO ISR gives
 * whenever a pin with a registered callback fires.  The semaphore is created at
 * boot in lua_driver_gpio_init(); callers may treat a NULL return as "no events
 * possible yet" and fall through to timeout.
 *
 * lua_gpio_dispatch(L) scans all pending GPIO events and calls the Lua callbacks
 * registered in State L (via gpio.on).  Returns the number of callbacks called.
 * Safe to call from any task context; does NOT push anything to the Lua stack. */
void *lua_gpio_get_ev_sema(void);
int   lua_gpio_dispatch(lua_State *L);

/* ── Button subsystem cross-file contract (lua_driver_gpio_button.c) ──────────
 * The button bottom-half lives in lua_driver_gpio_button.c but reuses gpio.c's
 * single per-port ISR and its hardware-config sequence.  button.c is always
 * compiled alongside gpio.c (same LUA_MOD_ENABLE_GPIO switch), so these are
 * resolved at link time without weak symbols.
 *
 * Lock order is fixed s_btn_lock (button, outer) -> GPIO_LOCK (gpio, inner):
 * lua_gpio_btn_setup_hw/teardown_hw take GPIO_LOCK internally and are only ever
 * called by button.c while it holds s_btn_lock.  gpio.c never calls back into
 * button.c while holding GPIO_LOCK, and the ISR takes neither lock — so the two
 * locks can never deadlock. */

/* Called from gpio.c's shared gpio_irq_cb at the very top.  Returns 1 if the
 * pin is owned by the button subsystem (edge recorded, ISR should return early)
 * or 0 to fall through to the legacy gpio.on event path.  ISR context: only
 * single-word writes + sema give, takes no lock. */
int  lua_btn_isr_hook(unsigned int pin);

/* Configure `pin` as a BOTHEDGE interrupt source (internal pull per
 * active_low) and enable it.  Takes GPIO_LOCK internally.  active_low != 0 →
 * pull-up (button to GND). */
void lua_gpio_btn_setup_hw(unsigned int pin, int active_low);

/* Disable the pin's interrupt (button release of the pin).  Takes GPIO_LOCK. */
void lua_gpio_btn_teardown_hw(unsigned int pin);

/* Lua `button` module entry + boot-time init (mutex / sema / queue creation,
 * single-threaded boot phase, called right after lua_driver_gpio_init()). */
int  luaopen_button(lua_State *L);
void lua_driver_gpio_button_init(void);

#ifdef __cplusplus
}
#endif

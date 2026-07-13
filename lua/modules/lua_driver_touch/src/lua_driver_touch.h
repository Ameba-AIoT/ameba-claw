/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
** lua_driver_touch.h — GT911 capacitive touch-panel driver (I2C + INT).
**
** Exposes require("touch") for the st7701p 480x480 panel's GT911 touch IC.
** This is the TOUCH PANEL, distinct from the on-chip CapTouch self-cap keys
** driver (require("captouch"), lua_driver_captouch.c).
**
** See design_spec/display/phase4_touch_gt911.md for architecture.
*/
#pragma once

#include <stddef.h>

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int luaopen_touch(lua_State *L);

/* ── C-level engine API (no lua_State involved) ────────────────────────────
 * The engine (I2C bus + INT ISR + reader task) is decoupled from any Lua
 * state already; these three entry points let lua_module_lvgl drive the same
 * GT911 engine directly for its lv_indev, instead of duplicating a second
 * touch bring-up (see design_spec/display/phase5_lvgl_full.md §6). The Lua
 * touch.init/deinit/get_event wrappers are thin shells over the same code. */
/* want_queue: whether the caller will drain touch.get_event()'s queue.
 * lvgl's lv_indev only ever reads touch_engine_snapshot() — pass 0 there, or
 * the reader task's down/up events pile up in a queue nobody empties and log
 * "event queue full, dropped ..." on every touch once it fills. touch.init()
 * (the Lua module, whose whole point IS get_event()) passes 1. */
int  touch_engine_init(const char *board_id, char *err, size_t errlen, int want_queue);  /* 0=ok */
void touch_engine_deinit(void);                                          /* idempotent */
void touch_engine_snapshot(int *x, int *y, int *pressed);                /* lv_indev read_cb poll */

#ifdef __cplusplus
}
#endif

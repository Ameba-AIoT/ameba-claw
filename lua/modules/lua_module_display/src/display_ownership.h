/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * display_ownership.h — the panel-ownership arbitrator, shared by the
 * "display" (command canvas) and "lvgl" (widget tree) Lua modules so the two
 * can never be simultaneously active (design_spec/display/lvgl_display_two_layer.md
 * §4.0, phase5_lvgl_full.md §4).
 *
 * display_lua.c owns the single mutex + mode state and implements these
 * functions.  lua_module_lvgl only ever calls through this header — it never
 * reaches into display_lua.c internals, keeping the dependency direction
 * one-way (lvgl depends on display's arbiter, not vice versa).
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISP_OWNER_NONE    = 0,
    DISP_OWNER_DISPLAY = 1,
    DISP_OWNER_LVGL    = 2,
} disp_owner_mode_t;

/* Try to acquire the panel for `mode`.  Returns a non-zero owner token on
 * success, 0 if the panel is already owned (by either mode). */
uint32_t disp_own_acquire(disp_owner_mode_t mode);

/* Idempotent release: only the token holder tears down.  For DISP_OWNER_LVGL
 * this blocks (bounded by CLAW_LVGL_TEARDOWN_JOIN_TIMEOUT_MS) waiting for
 * lvgl_timer_task to finish its own-thread cleanup before returning — see
 * phase5_lvgl_full.md §5. */
void disp_own_release(uint32_t token);

/* Current owner mode (DISP_OWNER_NONE if free).  Used to compose the
 * "display busy: owned by another mode" error text. */
disp_owner_mode_t disp_own_current_mode(void);

/* Ensure lv_init() has run (idempotent, never torn down).  Both modes must
 * call this before touching any lv_* API. */
void disp_lv_ensure_init(void);

/* Registered once by lua_module_lvgl (NULL until then).  own_teardown_locked()
 * calls this instead of touching LVGL/timer-task internals itself, keeping
 * display_lua.c ignorant of lua_module_lvgl's implementation. */
typedef void (*disp_lvgl_teardown_fn)(void);
void disp_set_lvgl_teardown_fn(disp_lvgl_teardown_fn fn);

#ifdef __cplusplus
}
#endif

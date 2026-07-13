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

/* Open the "lvgl" Lua module.  Called by the Lua require machinery. */
int luaopen_lvgl(lua_State *L);

/* Boot-phase init: registers the LVGL teardown callback with display_lua.c's
 * ownership arbiter (display_ownership.h).  Does NOT touch LVGL itself — that
 * stays lazy, on the first lvgl.start().  Called once from the single-
 * threaded boot phase (lua_module_registry_provision_all), same pattern as
 * lua_module_display_init(). */
void lua_module_lvgl_init(void);

#ifdef __cplusplus
}
#endif

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

/* Open the "display" Lua module.  Called by the Lua require machinery. */
int  luaopen_display(lua_State *L);

/* Boot-phase init: create the ownership mutex.  Does NOT touch LVGL — lv_init
 * is lazy and happens on the first display.init(id).  Called once from the
 * single-threaded boot phase (lua_module_registry_provision_all). */
void lua_module_display_init(void);

#ifdef __cplusplus
}
#endif

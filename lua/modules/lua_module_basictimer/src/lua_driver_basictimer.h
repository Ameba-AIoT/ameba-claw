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

/* Create the global mutex. Call once at boot before any Lua code runs. */
void lua_driver_basictimer_init(void);

/* Lua module opener — registered as "basictimer". */
LUAMOD_API int luaopen_basictimer(lua_State *L);

#ifdef __cplusplus
}
#endif

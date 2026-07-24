/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LUA_MODULE_MAGNETOMETER_H
#define LUA_MODULE_MAGNETOMETER_H

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

LUAMOD_API int luaopen_magnetometer(lua_State *L);

/* Called once at boot (single-threaded) to initialise module-level state. */
void lua_module_magnetometer_init(void);

#ifdef __cplusplus
}
#endif

#endif /* LUA_MODULE_MAGNETOMETER_H */

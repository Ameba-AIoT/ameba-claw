/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LUA_DRIVER_THERMAL_H_
#define LUA_DRIVER_THERMAL_H_

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

LUAMOD_API int luaopen_thermal(lua_State *L);
void lua_driver_thermal_provision(void);

#ifdef __cplusplus
}
#endif

#endif /* LUA_DRIVER_THERMAL_H_ */

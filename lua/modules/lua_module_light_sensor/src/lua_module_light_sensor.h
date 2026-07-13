/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LUA_MODULE_LIGHT_SENSOR_H
#define LUA_MODULE_LIGHT_SENSOR_H

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

LUAMOD_API int luaopen_light_sensor(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif /* LUA_MODULE_LIGHT_SENSOR_H */

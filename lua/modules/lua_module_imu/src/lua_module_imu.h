/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LUA_MODULE_IMU_H
#define LUA_MODULE_IMU_H

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

LUAMOD_API int luaopen_imu(lua_State *L);

/* Called once at boot (single-threaded) to create per-controller locks. */
void lua_module_imu_init(void);

#ifdef __cplusplus
}
#endif

#endif /* LUA_MODULE_IMU_H */

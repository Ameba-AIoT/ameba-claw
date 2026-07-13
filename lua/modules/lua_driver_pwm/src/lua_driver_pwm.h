/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LUA_DRIVER_PWM_H
#define LUA_DRIVER_PWM_H

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

LUAMOD_API int luaopen_pwm(lua_State *L);

/* Call once at boot (before any concurrent Lua execution) to create the
 * per-timer mutexes used for concurrency protection. */
void lua_driver_pwm_init(void);

#ifdef __cplusplus
}
#endif

#endif /* LUA_DRIVER_PWM_H */

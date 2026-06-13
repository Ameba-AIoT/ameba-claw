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

int luaopen_touch(lua_State *L);
void lua_driver_touch_provision(void);

#ifdef __cplusplus
}
#endif

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

int  luaopen_i2c(lua_State *L);
void lua_driver_i2c_init(void); /* call once at boot (single-threaded) */

#ifdef __cplusplus
}
#endif

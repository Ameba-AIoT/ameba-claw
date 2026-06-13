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

int luaopen_rtc(lua_State *L);

/* Create the RTC concurrency mutex. Call once during the single-threaded boot
 * phase (from lua_module_registry_provision_all) before any concurrent Lua
 * execution can reach the rtc.* APIs. */
void lua_driver_rtc_init(void);

#ifdef __cplusplus
}
#endif

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

int luaopen_gpio(lua_State *L);

/* Create the GPIO concurrency mutex. Call once during the single-threaded boot
 * phase (from lua_module_registry_provision_all) before any concurrent Lua
 * execution can reach the gpio.* APIs. */
void lua_driver_gpio_init(void);

#ifdef __cplusplus
}
#endif

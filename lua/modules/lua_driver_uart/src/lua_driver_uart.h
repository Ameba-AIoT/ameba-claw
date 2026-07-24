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

/* Called once from lua_module_registry_provision_all() during single-threaded
 * boot to create per-controller mutexes before any concurrent Lua execution. */
void lua_driver_uart_init(void);

int luaopen_uart(lua_State *L);

#ifdef __cplusplus
}
#endif

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

/* Open the "spi" Lua module.  Called by the Lua require machinery. */
int  luaopen_spi(lua_State *L);

/* Create per-controller mutex and semaphore.  Call once from the
 * single-threaded boot phase, before any concurrent Lua execution. */
void lua_driver_spi_init(void);

/* Register the module and run any self-test fixtures.  Called by
 * lua_module_registry_provision_all() at boot. */
void lua_driver_spi_provision(void);

#ifdef __cplusplus
}
#endif

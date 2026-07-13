/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LUA_REPL_H
#define LUA_REPL_H

#include "lua.h"

void lua_repl_run(lua_State *L);

/* Create a fresh Lua state, run the interactive REPL, then close it.
 * Temporarily disables LOG UART RX interrupt (restored on exit). */
void lua_run_repl_once(void);

#endif /* LUA_REPL_H */

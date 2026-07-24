/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _LUA_DRIVER_CAPTOUCH_H_
#define _LUA_DRIVER_CAPTOUCH_H_

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int luaopen_captouch(lua_State *L);
void lua_driver_captouch_provision(void);

#ifdef __cplusplus
}
#endif

#endif /* _LUA_DRIVER_CAPTOUCH_H_ */

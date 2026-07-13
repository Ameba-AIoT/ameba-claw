/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
** lusys.c — Lua system utilities for Ameba RTOS.
**
** Provides require("sys"):
**   sys.sleep_ms(ms)        blocking delay, yields to the RTOS scheduler
**   sys.shell(cmd_string)   dispatch a console command (e.g. "aplay -r 16000 ...")
**                           non-blocking: spawns a background task, returns true/false
** SPDX-License-Identifier: Apache-2.0
*/

#define LUA_LIB
#include "lua.h"
#include "lauxlib.h"
#include "os_wrapper.h"
#include <string.h>
#include <stdint.h>

/* sys.sleep_ms(ms) — breaks into 10 ms chunks so cancel hook can interrupt */
static int lsys_sleep_ms(lua_State *L)
{
	uint32_t ms = (uint32_t)luaL_checkinteger(L, 1);
	uint32_t elapsed = 0;
	while (elapsed < ms) {
		uint32_t chunk = (ms - elapsed > 10u) ? 10u : (ms - elapsed);
		rtos_time_delay_ms(chunk);
		elapsed += chunk;
		/* Check cancel pointer registered by skill sandbox (NULL in main task) */
		lua_getfield(L, LUA_REGISTRYINDEX, "__cancel_ptr");
		volatile int *p = (volatile int *)lua_touserdata(L, -1);
		lua_pop(L, 1);
		if (p && *p) luaL_error(L, "skill execution cancelled");
	}
	return 0;
}

/* sys.millis() — monotonic millisecond counter (wraps at ~49 days on 32-bit) */
static int lsys_millis(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)rtos_time_get_current_system_time_ms());
	return 1;
}

/* sys.uptime() — seconds since boot as a float */
static int lsys_uptime(lua_State *L)
{
	lua_pushnumber(L, (lua_Number)rtos_time_get_current_system_time_ms() / 1000.0);
	return 1;
}

/* sys.shell(cmd) — placeholder; aplay is amebasmart-only and not available on RTL8721F. */
static int lsys_shell(lua_State *L)
{
	const char *cmd_str = luaL_checkstring(L, 1);
	lua_pushnil(L);
	lua_pushfstring(L, "sys.shell: unsupported command: %s", cmd_str);
	return 2;
}

static const luaL_Reg lusys_funcs[] = {
	{"sleep_ms", lsys_sleep_ms},
	{"millis",   lsys_millis},
	{"uptime",   lsys_uptime},
	{"shell",    lsys_shell},
	{NULL, NULL}
};

LUAMOD_API int luaopen_sys(lua_State *L)
{
	luaL_newlib(L, lusys_funcs);
	return 1;
}

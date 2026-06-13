/*
** lloadlib_stub.c — minimal package library stub for Ameba embedded Lua.
**
** Provides require() that resolves only preloaded modules (_LOADED table).
** Dynamic file loading is not supported on this platform.
*/

#define LUA_LIB
#include "lprefix.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static int ll_require(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	/* Check _LOADED[name] first */
	luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
	lua_getfield(L, -1, name);
	if (lua_toboolean(L, -1)) {
		lua_remove(L, -2);
		return 1;
	}
	/* Check package.preload[name] */
	lua_pop(L, 2);
	lua_getglobal(L, "package");
	if (!lua_isnil(L, -1)) {
		lua_getfield(L, -1, "preload");
		lua_remove(L, -2);
		if (lua_istable(L, -1)) {
			lua_getfield(L, -1, name);
			lua_remove(L, -2);
			if (lua_isfunction(L, -1)) {
				lua_pushstring(L, name);
				lua_call(L, 1, 1);
				/* cache in _LOADED */
				luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
				lua_pushvalue(L, -2);
				lua_setfield(L, -2, name);
				lua_pop(L, 1);
				return 1;
			}
			lua_pop(L, 1);
		} else {
			lua_pop(L, 1);
		}
	} else {
		lua_pop(L, 1);
	}
	return luaL_error(L, "module '%s' not found", name);
}

static const luaL_Reg pk_funcs[] = {
	{NULL, NULL}
};

LUAMOD_API int luaopen_package(lua_State *L)
{
	luaL_newlib(L, pk_funcs);
	/* preload subtable for user extensions */
	lua_newtable(L);
	lua_setfield(L, -2, "preload");
	/* expose require as global */
	lua_pushcfunction(L, ll_require);
	lua_setglobal(L, "require");
	return 1;
}

/*
** $Id: linit.c $
** Initialization of libraries for lua.c and other clients
** See Copyright Notice in lua.h
*/


#define linit_c
#define LUA_LIB

#include "lprefix.h"


#include <stddef.h>

#include "lua.h"

#include "lualib.h"
#include "lauxlib.h"
#include "lua_module_registry.h"

/*
** Custom linit.c for embedded Ameba RTOS.
** Registers safe standard libraries only — no oslib (dangerous on bare-metal),
** no loadlib (no dynamic loading). The Ameba custom modules (gpio, i2c, cap,
** event, ...) are described once in lua_module_registry.c and installed here
** via lua_module_registry_install(); there is no second module list to keep in
** sync with cap_skill_mgr.c or lua_main.c.
*/

static const luaL_Reg loadedlibs[] = {
  {LUA_GNAME, luaopen_base},
  {LUA_LOADLIBNAME, luaopen_package},
  {LUA_COLIBNAME, luaopen_coroutine},
  {LUA_TABLIBNAME, luaopen_table},
  /* io excluded: luaopen_io calls fopen/getc via raw FILE* which bypasses the
   * VFS prefix guard in lua_module_file.c (io.open can reach any path). */
  {LUA_STRLIBNAME, luaopen_string},
  {LUA_UTF8LIBNAME, luaopen_utf8},
  {LUA_MATHLIBNAME, luaopen_math},
  /* debug excluded: debug.upvaluejoin / debug.getmetatable are sandbox-escape
   * primitives reachable from the AT+CLAW=lua console. */
  {NULL, NULL}
};


LUALIB_API void luaL_openlibs (lua_State *L) {
  const luaL_Reg *lib;
  /* Standard libraries (the safe subset). */
  for (lib = loadedlibs; lib->func; lib++) {
    luaL_requiref(L, lib->name, lib->func, 1);
    lua_pop(L, 1);
  }
  /* Ameba custom modules for the full REPL environment. Eager modules are
   * required immediately; lazy ones (e.g. udp) go into package.preload so
   * require('udp') works without paying their heap cost at startup. */
  lua_module_registry_install(L, LUA_MOD_ENV_REPL);
}

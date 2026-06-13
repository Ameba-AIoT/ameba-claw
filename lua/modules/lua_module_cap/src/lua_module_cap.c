/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
** lucap.c — Lua cap module for Ameba RTOS (ameba_claw).
**
** Provides require("cap"):
**   local ok, result = cap.call("cap_name", input_json_str) — call a registered cap
**   local catalog    = cap.list()                           — return cap catalog JSON
**
** SPDX-License-Identifier: Apache-2.0
*/

#define LUA_LIB
#include "lua.h"
#include "lauxlib.h"
#include "claw_cap.h"
#include "claw_compat.h"
#include <stdlib.h>
#include <string.h>

static int lua_cap_call(lua_State *L)
{
    const char *name  = luaL_checkstring(L, 1);
    const char *input = luaL_optstring(L, 2, "{}");
    char *out = NULL;
    claw_cap_call_context_t ctx = {0};
    ctx.caller = CLAW_CAP_CALLER_MANUAL;
    int r = claw_cap_call(name, input, &ctx, &out);
    lua_pushboolean(L, r == RTK_SUCCESS);
    lua_pushstring(L, out ? out : "");
    free(out);
    return 2;
}

static int lua_cap_list(lua_State *L)
{
    char *catalog = claw_cap_build_catalog();
    lua_pushstring(L, catalog ? catalog : "[]");
    free(catalog);
    return 1;
}

static const luaL_Reg cap_lib[] = {
    {"call", lua_cap_call},
    {"list", lua_cap_list},
    {NULL, NULL}
};

LUAMOD_API int luaopen_cap(lua_State *L)
{
    luaL_newlib(L, cap_lib);
    return 1;
}

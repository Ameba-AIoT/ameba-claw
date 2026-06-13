/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
** luevent.c — Lua event module for Ameba RTOS (ameba_claw).
**
** Provides require("event"):
**   event.publish_message(source_cap, channel, chat_id, text [, sender_id]) → true/false
**   event.publish_trigger(source_cap, event_type, event_key [, payload_json]) → true/false
**
** SPDX-License-Identifier: Apache-2.0
*/

#define LUA_LIB
#include "lua.h"
#include "lauxlib.h"
#include "claw_event_publisher.h"
#include "claw_compat.h"

static int lua_event_publish_message(lua_State *L)
{
    const char *source  = luaL_checkstring(L, 1);
    const char *channel = luaL_checkstring(L, 2);
    const char *chat_id = luaL_checkstring(L, 3);
    const char *text    = luaL_checkstring(L, 4);
    const char *sender  = lua_isstring(L, 5) ? lua_tostring(L, 5) : NULL;
    int r = claw_event_dispatcher_publish_message(source, channel, chat_id, text, sender, NULL);
    lua_pushboolean(L, r == RTK_SUCCESS);
    return 1;
}

static int lua_event_publish_trigger(lua_State *L)
{
    const char *source  = luaL_checkstring(L, 1);
    const char *etype   = luaL_checkstring(L, 2);
    const char *ekey    = luaL_checkstring(L, 3);
    const char *payload = luaL_optstring(L, 4, "{}");
    int r = claw_event_dispatcher_publish_trigger(source, etype, ekey, payload);
    lua_pushboolean(L, r == RTK_SUCCESS);
    return 1;
}

static const luaL_Reg event_lib[] = {
    {"publish_message", lua_event_publish_message},
    {"publish_trigger", lua_event_publish_trigger},
    {NULL, NULL}
};

LUAMOD_API int luaopen_event(lua_State *L)
{
    luaL_newlib(L, event_lib);
    return 1;
}

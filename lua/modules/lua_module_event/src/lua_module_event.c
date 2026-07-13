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
**   event.send(channel, chat_id, text) → true/false   (outbound push to a channel)
**   event.notify(text)                 → true/false   (push to this job's launcher)
**   event.origin()                     → channel, chat_id  (this job's launcher)
**
** SPDX-License-Identifier: Apache-2.0
*/

#define LUA_LIB
#include "lua.h"
#include "lauxlib.h"
#include "claw_event_publisher.h"
#include "claw_im_dispatch.h"
#include "claw_compat.h"
#include "os_wrapper.h"

/* GPIO event subsystem — resolved at link time from lua_driver_gpio.c */
extern void *lua_gpio_get_ev_sema(void);
extern int   lua_gpio_dispatch(lua_State *L);

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

/*
** event.wait([timeout_ms])  →  true | nil
**
** Block until a hardware event is ready (currently: GPIO pin with gpio.on
** registered callback), then dispatch it (calling the Lua callback in the
** current lua_State) and return true.  Returns nil on timeout.
**
** Blocking is done in 50 ms chunks so the cooperative cancel hook can fire
** (same pattern as sys.sleep_ms).  Passing 0 or omitting the argument gives
** a non-blocking check (equivalent to gpio.dispatch() > 0).
**
** Usage:
**   gpio.on(pin, "both", function(ev) print(ev.pin, ev.edge) end)
**   gpio.irq_enable(pin)
**   while true do
**     if event.wait(5000) then print("got event") else print("timeout") end
**   end
*/
/*
** event.send(channel, chat_id, text)  →  true | false
**
** Proactively push a text message OUTBOUND to a user on a registered IM/serial
** channel, without going through an agent turn.  This is the "notify the user"
** primitive for detached background jobs (e.g. a camera→vision monitor loop
** that must tell the user when the scene changes).  Contrast with
** publish_message(), which injects an INBOUND message that re-triggers the LLM.
**
** Returns false (does nothing) if no send handler is registered for `channel`.
** `channel`/`chat_id` are the same ids seen on inbound events (ev.channel /
** ev.chat) — capture them at start-up and reuse them to reply to that user.
*/
static int lua_event_send(lua_State *L)
{
    const char *channel = luaL_checkstring(L, 1);
    const char *chat_id = luaL_checkstring(L, 2);
    const char *text    = luaL_checkstring(L, 3);
    if (!claw_im_dispatch_has_channel(channel)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    claw_im_dispatch_send(channel, chat_id, text);
    lua_pushboolean(L, 1);
    return 1;
}

/*
** event.notify(text)  →  true | false
**
** The robust "tell the user" primitive: push `text` OUTBOUND to whoever
** launched this script, with NO channel/chat_id arguments — the harness
** captured the caller's origin (channel + chat_id) at launch time and routes
** for you, so the script (and the LLM that wrote it) can never target the
** wrong user. This is the right call for a detached background monitor loop
** that must report a change.
**
** Returns false (sends nothing) when there is no reachable origin — e.g. the
** job was launched from a trigger/scheduler with no channel, or that channel
** has no outbound send handler. Use event.send() to target a specific channel.
*/
static int lua_event_notify(lua_State *L)
{
    const char *text = luaL_checkstring(L, 1);

    lua_getfield(L, LUA_REGISTRYINDEX, "__origin_channel");
    const char *channel = lua_tostring(L, -1);
    lua_getfield(L, LUA_REGISTRYINDEX, "__origin_chat");
    const char *chat_id = lua_tostring(L, -1);

    int ok = 0;
    if (channel && channel[0] && claw_im_dispatch_has_channel(channel)) {
        claw_im_dispatch_send(channel, chat_id ? chat_id : "", text);
        ok = 1;
    }
    lua_pop(L, 2);
    lua_pushboolean(L, ok);
    return 1;
}

/*
** event.origin()  →  channel, chat_id
**
** Return the captured origin of the current script's launcher (the ids that
** arrived on the inbound event). Both are strings, possibly "" when the launch
** had no channel. Usually you want event.notify() instead; use this only when
** you need the raw ids (e.g. to route to a different channel via event.send()).
*/
static int lua_event_origin(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__origin_channel");
    if (!lua_isstring(L, -1)) { lua_pop(L, 1); lua_pushstring(L, ""); }
    lua_getfield(L, LUA_REGISTRYINDEX, "__origin_chat");
    if (!lua_isstring(L, -1)) { lua_pop(L, 1); lua_pushstring(L, ""); }
    return 2;
}

static int lua_event_wait(lua_State *L)
{
    uint32_t timeout_ms = (uint32_t)luaL_optinteger(L, 1, 0);

    /* Non-blocking fast path: dispatch any already-pending events. */
    if (lua_gpio_dispatch(L) > 0) {
        lua_pushboolean(L, 1);
        return 1;
    }
    if (timeout_ms == 0) {
        lua_pushnil(L);
        return 1;
    }

    rtos_sema_t sema = (rtos_sema_t)lua_gpio_get_ev_sema();
    uint32_t elapsed = 0;

    while (elapsed < timeout_ms) {
        uint32_t remaining = timeout_ms - elapsed;
        uint32_t chunk = remaining < 50u ? remaining : 50u;

        if (sema && rtos_sema_take(sema, chunk) == RTK_SUCCESS) {
            lua_gpio_dispatch(L);
            lua_pushboolean(L, 1);
            return 1;
        }
        elapsed += chunk;

        /* Cooperative cancel check — same as sys.sleep_ms. */
        lua_getfield(L, LUA_REGISTRYINDEX, "__cancel_ptr");
        volatile int *cp = (volatile int *)lua_touserdata(L, -1);
        lua_pop(L, 1);
        if (cp && *cp) {
            luaL_error(L, "event.wait cancelled");
        }
    }

    lua_pushnil(L);
    return 1;
}

static const luaL_Reg event_lib[] = {
    {"publish_message", lua_event_publish_message},
    {"publish_trigger", lua_event_publish_trigger},
    {"send",            lua_event_send},
    {"notify",          lua_event_notify},
    {"origin",          lua_event_origin},
    {"wait",            lua_event_wait},
    {NULL, NULL}
};

LUAMOD_API int luaopen_event(lua_State *L)
{
    luaL_newlib(L, event_lib);
    return 1;
}

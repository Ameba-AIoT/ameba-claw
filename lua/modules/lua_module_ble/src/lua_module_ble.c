/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * lua_module_ble.c — require("ble"): BLE peripheral Lua API.
 *
 *   ble.init([name])            → true | nil, err   (enable stack, idempotent)
 *   ble.deinit()                → true
 *   ble.set_name(name)          → true | nil, err
 *   ble.address()               → "xx:xx:.." | nil, err
 *   ble.adv_start()             → true | nil, err
 *   ble.adv_stop()              → true | nil, err
 *   ble.disconnect(conn_index)  → true | nil, err
 *   ble.on_event(fn)            → true            (fn(ev) callback)
 *   ble.process_events([ms])    → n dispatched
 *   ble.stats()                 → { enabled, advertising, conn_count, max_conn }
 *
 * Event table (ev) passed to the on_event callback:
 *   ev.type   = "connected" | "disconnected" | "adv_started" | "adv_stopped"
 *             | "data" | "subscribe" | "mtu_changed"
 *   ev.conn   = conn_index (when applicable)
 *   ev.reason = disconnect reason / adv stop reason (when applicable)
 *   ev.payload= inbound bytes (data event)
 *   ev.notify = bool (subscribe event)
 *   ev.mtu    = negotiated MTU (mtu_changed event)
 *
 * luaopen_ble MUST be side-effect free (EAGER load runs it in every lua_State):
 * it only builds the function table. The BT stack is touched only in ble.init().
 */
#define LUA_LIB
#include "lua.h"
#include "lauxlib.h"

#include "lua_ble_bridge.h"
#include "ameba_claw_defs.h"

#define BLE_ON_EVENT_KEY  "__ble_on_event"

/* ---- helpers ------------------------------------------------------------ */

static int push_ok_or_err(lua_State *L, int rc)
{
    if (rc == 0) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushfstring(L, "ble error 0x%x", rc);
    return 2;
}

static const char *ev_type_name(uint8_t type)
{
    switch (type) {
    case LUA_BLE_EV_CONNECTED:    return "connected";
    case LUA_BLE_EV_DISCONNECTED: return "disconnected";
    case LUA_BLE_EV_ADV_STARTED:  return "adv_started";
    case LUA_BLE_EV_ADV_STOPPED:  return "adv_stopped";
    case LUA_BLE_EV_DATA:         return "data";
    case LUA_BLE_EV_SUBSCRIBE:    return "subscribe";
    case LUA_BLE_EV_MTU:          return "mtu_changed";
    default:                      return "unknown";
    }
}

/* Build the ev table on the stack, invoke the registered callback (if any),
 * then free the event's heap payload. Never propagates a Lua error out of the
 * callback (uses pcall) so one bad handler cannot stall process_events. */
static void dispatch_one(lua_State *L, lua_ble_evt_t *ev)
{
    lua_getfield(L, LUA_REGISTRYINDEX, BLE_ON_EVENT_KEY);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        lua_ble_bridge_evt_free(ev);
        return;
    }

    lua_newtable(L);
    lua_pushstring(L, ev_type_name(ev->type));
    lua_setfield(L, -2, "type");

    if (ev->conn_index >= 0) {
        lua_pushinteger(L, ev->conn_index);
        lua_setfield(L, -2, "conn");
    }

    switch (ev->type) {
    case LUA_BLE_EV_DISCONNECTED:
    case LUA_BLE_EV_ADV_STOPPED:
        lua_pushinteger(L, ev->err);
        lua_setfield(L, -2, "reason");
        break;
    case LUA_BLE_EV_DATA:
        lua_pushlstring(L, (const char *)(ev->payload ? ev->payload : (const uint8_t *)""),
                        ev->len);
        lua_setfield(L, -2, "payload");
        break;
    case LUA_BLE_EV_SUBSCRIBE:
        lua_pushboolean(L, ev->flag);
        lua_setfield(L, -2, "notify");
        break;
    case LUA_BLE_EV_MTU:
        lua_pushinteger(L, ev->err);
        lua_setfield(L, -2, "mtu");
        break;
    default:
        break;
    }

    /* stack: callback, ev_table */
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        /* Swallow the error message (already on stack) — pop and continue. */
        lua_pop(L, 1);
    }
    lua_ble_bridge_evt_free(ev);
}

/* ---- API ---------------------------------------------------------------- */

static int lble_init(lua_State *L)
{
    const char *name = luaL_optstring(L, 1, NULL);
    return push_ok_or_err(L, lua_ble_bridge_init(name));
}

static int lble_deinit(lua_State *L)
{
    lua_ble_bridge_deinit();
    lua_pushboolean(L, 1);
    return 1;
}

static int lble_set_name(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    return push_ok_or_err(L, lua_ble_bridge_set_name(name));
}

static int lble_address(lua_State *L)
{
    char buf[32] = {0};
    int rc = lua_ble_bridge_get_addr(buf, sizeof(buf));
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "ble error 0x%x", rc);
        return 2;
    }
    lua_pushstring(L, buf);
    return 1;
}

static int lble_adv_start(lua_State *L)
{
    return push_ok_or_err(L, lua_ble_bridge_adv_start());
}

static int lble_adv_stop(lua_State *L)
{
    return push_ok_or_err(L, lua_ble_bridge_adv_stop());
}

static int lble_disconnect(lua_State *L)
{
    int conn = (int)luaL_checkinteger(L, 1);
    return push_ok_or_err(L, lua_ble_bridge_disconnect(conn));
}

static int lble_notify(lua_State *L)
{
    int         conn = (int)luaL_checkinteger(L, 1);
    size_t      len;
    const char *data = luaL_checklstring(L, 2, &len);
    return push_ok_or_err(L, lua_ble_bridge_notify(conn, (const uint8_t *)data,
                                                    (uint16_t)len));
}

static int lble_on_event(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, BLE_ON_EVENT_KEY);
    lua_pushboolean(L, 1);
    return 1;
}

static int lble_process_events(lua_State *L)
{
    uint32_t timeout = (uint32_t)luaL_optinteger(L, 1, 0);
    int      dispatched = 0;
    uint32_t elapsed = 0;
    lua_ble_evt_t ev;

    /* Fast path: dispatch everything already queued. */
    while (dispatched < CLAW_BLE_DISPATCH_MAX_PER_CALL &&
           lua_ble_bridge_poll(&ev, 0) == 1) {
        dispatch_one(L, &ev);
        dispatched++;
    }
    if (dispatched > 0 || timeout == 0) {
        lua_pushinteger(L, dispatched);
        return 1;
    }

    /* Block for the first event, in 50 ms chunks so the cooperative cancel
     * hook can fire (same pattern as event.wait / sys.sleep_ms). */
    while (elapsed < timeout && dispatched == 0) {
        uint32_t remaining = timeout - elapsed;
        uint32_t chunk = remaining < 50u ? remaining : 50u;

        if (lua_ble_bridge_poll(&ev, chunk) == 1) {
            dispatch_one(L, &ev);
            dispatched++;
            while (dispatched < CLAW_BLE_DISPATCH_MAX_PER_CALL &&
                   lua_ble_bridge_poll(&ev, 0) == 1) {
                dispatch_one(L, &ev);
                dispatched++;
            }
            break;
        }
        elapsed += chunk;

        lua_getfield(L, LUA_REGISTRYINDEX, "__cancel_ptr");
        volatile int *cp = (volatile int *)lua_touserdata(L, -1);
        lua_pop(L, 1);
        if (cp && *cp) {
            luaL_error(L, "ble.process_events cancelled");
        }
    }

    lua_pushinteger(L, dispatched);
    return 1;
}

static int lble_stats(lua_State *L)
{
    lua_ble_stats_t st;
    lua_ble_bridge_stats(&st);
    lua_newtable(L);
    lua_pushboolean(L, st.enabled);     lua_setfield(L, -2, "enabled");
    lua_pushboolean(L, st.advertising); lua_setfield(L, -2, "advertising");
    lua_pushinteger(L, st.conn_count);  lua_setfield(L, -2, "conn_count");
    lua_pushinteger(L, st.max_conn);    lua_setfield(L, -2, "max_conn");
    return 1;
}

static const luaL_Reg ble_funcs[] = {
    {"init",           lble_init},
    {"deinit",         lble_deinit},
    {"set_name",       lble_set_name},
    {"address",        lble_address},
    {"adv_start",      lble_adv_start},
    {"adv_stop",       lble_adv_stop},
    {"disconnect",     lble_disconnect},
    {"notify",         lble_notify},
    {"on_event",       lble_on_event},
    {"process_events", lble_process_events},
    {"stats",          lble_stats},
    {NULL, NULL}
};

LUAMOD_API int luaopen_ble(lua_State *L)
{
    luaL_newlib(L, ble_funcs);
    return 1;
}

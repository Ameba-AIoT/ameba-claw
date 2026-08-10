/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * lua_ble_bridge.h — plain-C bridge between the RTK BT peripheral stack and the
 * Lua `ble` module. lua_module_ble.c is the only caller; it must never touch
 * rtk_bt_* directly. All functions run on the Lua job task EXCEPT the RTK event
 * callbacks (which run on bt_evt_task and only enqueue deep-copied events).
 *
 * Design: design_spec/lua_ble/lua_ble_project_plan.md
 */
#ifndef LUA_BLE_BRIDGE_H
#define LUA_BLE_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Event types delivered to Lua via lua_ble_bridge_poll(). Mirrors the plan's
 * ev.type strings; lua_module_ble.c maps these ints to the string names. */
typedef enum {
    LUA_BLE_EV_CONNECTED = 1,   /* conn_index valid */
    LUA_BLE_EV_DISCONNECTED,    /* conn_index valid, err = disconnect reason */
    LUA_BLE_EV_ADV_STARTED,     /* err = 0 on success */
    LUA_BLE_EV_ADV_STOPPED,     /* err = stop reason */
    LUA_BLE_EV_DATA,            /* conn_index + payload/len (peer wrote fff1) */
    LUA_BLE_EV_SUBSCRIBE,       /* conn_index + flag (peer toggled fff1 CCCD notify) */
    LUA_BLE_EV_MTU,             /* conn_index + value (negotiated MTU) */
} lua_ble_ev_type_t;

/* One dequeued event. payload is heap-owned by the consumer: after handling,
 * call lua_ble_bridge_evt_free(). payload is NULL for non-data events. */
typedef struct {
    uint8_t   type;         /* lua_ble_ev_type_t */
    int8_t    conn_index;   /* 0..CLAW_BLE_MAX_CONN-1, or -1 if n/a */
    uint8_t   flag;         /* subscribe: 1=notify on; else 0 */
    uint16_t  err;          /* error / reason / mtu value depending on type */
    uint16_t  len;          /* payload length (data event) */
    uint8_t  *payload;      /* deep-copied inbound bytes, or NULL */
} lua_ble_evt_t;

/* Lifecycle. Return 0 on success, non-zero RTK error code otherwise.
 * init is idempotent: a second call while enabled is a no-op success. */
int  lua_ble_bridge_init(const char *name);   /* name may be NULL -> keep default */
int  lua_ble_bridge_deinit(void);
bool lua_ble_bridge_is_enabled(void);

/* GAP helpers (require enabled). */
int  lua_ble_bridge_get_addr(char *out, size_t out_len);   /* "xx:xx:.." string */
int  lua_ble_bridge_set_name(const char *name);
int  lua_ble_bridge_adv_start(void);
int  lua_ble_bridge_adv_stop(void);
int  lua_ble_bridge_disconnect(int conn_index);

/* Send bytes to a subscribed peer over fff1 (server->client notify). Fails if
 * the peer has not enabled notify (CCCD) or len exceeds the negotiated MTU-3. */
int  lua_ble_bridge_notify(int conn_index, const uint8_t *data, uint16_t len);

/* Event poll (consumer = Lua job). Returns 1 and fills *out if an event was
 * dequeued within timeout_ms; 0 on timeout; <0 if not initialised. */
int  lua_ble_bridge_poll(lua_ble_evt_t *out, uint32_t timeout_ms);
void lua_ble_bridge_evt_free(lua_ble_evt_t *evt);

/* Stats snapshot for ble.stats(). */
typedef struct {
    bool    enabled;
    bool    advertising;
    uint8_t conn_count;
    uint8_t max_conn;
} lua_ble_stats_t;
void lua_ble_bridge_stats(lua_ble_stats_t *out);

#endif /* LUA_BLE_BRIDGE_H */

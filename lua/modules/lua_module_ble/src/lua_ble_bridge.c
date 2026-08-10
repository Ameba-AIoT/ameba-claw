/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * lua_ble_bridge.c — RTK BLE peripheral stack wrapped for the Lua `ble` module.
 *
 * Two-task model (see design_spec/lua_ble):
 *   - RTK event callbacks run on bt_evt_task. They ONLY deep-copy the event into
 *     a C ring queue and return; they never call back into Lua.
 *   - The Lua job task calls lua_ble_bridge_poll() to drain that queue.
 *
 * This file follows ameba_claw conventions (rtos_* wrappers, RTK_LOGS), NOT the
 * component/bluetooth osif_ / BT_LOGx conventions.
 */
#include "lua_ble_bridge.h"
#include "ameba_claw_defs.h"

#include "ameba_soc.h"          /* RTK_LOGS / RTK_LOG_* */
#include "os_wrapper.h"
#include <string.h>
#include <stdlib.h>

/* RTK BT public API (only api/include/ headers, per module boundary). */
#include "rtk_bt_def.h"
#include "rtk_bt_common.h"
#include "rtk_bt_device.h"
#include "rtk_bt_le_gap.h"
#include "rtk_bt_att_defs.h"
#include "rtk_bt_gatts.h"

/* Hard-wired transparent service (fff0) with a single characteristic (fff1)
 * that is read + write + write-without-response + notify. The attribute table
 * is fixed here; a dynamic gatts_define engine is future work (plan §7).
 *
 * Attribute layout / indices (attr_count = 4):
 *   [0] primary service (fff0)
 *   [1] characteristic declaration
 *   [2] characteristic VALUE (fff1)  <- write/read target, notify source (APP flag)
 *   [3] client characteristic config (CCCD)
 */
#define CLAW_BLE_APP_ID          0
#define CLAW_BLE_ATTR_CHAR_VAL   2
#define CLAW_BLE_ATTR_CCCD       3

/* ---- module state ------------------------------------------------------- */

static bool          s_enabled     = false;
static bool          s_advertising = false;
static rtos_queue_t  s_evt_queue   = NULL;   /* lua_ble_evt_t by value */
static rtos_mutex_t  s_lock        = NULL;   /* guards conn table + flags */
static char          s_name[CLAW_BLE_NAME_MAX] = "ameba-claw";

typedef struct {
    bool     used;
    uint16_t handle;
    bool     subscribed;   /* peer enabled fff1 notify (CCCD) */
    uint16_t mtu;          /* negotiated ATT MTU (default 23 until exchanged) */
} ble_conn_slot_t;
static ble_conn_slot_t s_conns[CLAW_BLE_MAX_CONN];

/* ---- fff0/fff1 transparent GATT service ---------------------------------- */

static rtk_bt_gatt_attr_t s_attrs[] = {
    RTK_BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_16(CLAW_BLE_SVC_UUID)),
    RTK_BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_16(CLAW_BLE_CHAR_UUID),
                               RTK_BT_GATT_CHRC_READ | RTK_BT_GATT_CHRC_WRITE |
                               RTK_BT_GATT_CHRC_WRITE_WITHOUT_RESP | RTK_BT_GATT_CHRC_NOTIFY,
                               RTK_BT_GATT_PERM_READ | RTK_BT_GATT_PERM_WRITE),
    RTK_BT_GATT_CCC(RTK_BT_GATT_PERM_READ | RTK_BT_GATT_PERM_WRITE),
};
static struct rtk_bt_gatt_service s_service = RTK_BT_GATT_SERVICE(s_attrs, CLAW_BLE_APP_ID);

/* Small readable value for a client READ of fff1 (the value attr has the APP
 * flag so reads must be answered by us). Holds the device name by default. */
static uint8_t s_read_val[CLAW_BLE_NAME_MAX] = "ameba-claw";
static uint16_t s_read_len = 10;

/* ---- small locking helpers ---------------------------------------------- */

static void lock(void)   { if (s_lock) rtos_mutex_take(s_lock, 0xFFFFFFFFUL); }
static void unlock(void) { if (s_lock) rtos_mutex_give(s_lock); }

/* ---- connection table (called from both tasks; hold lock) --------------- */

static int conn_add_locked(uint16_t handle)
{
    for (int i = 0; i < CLAW_BLE_MAX_CONN; i++) {
        if (!s_conns[i].used) {
            s_conns[i].used       = true;
            s_conns[i].handle     = handle;
            s_conns[i].subscribed = false;
            s_conns[i].mtu        = 23;   /* BLE default until MTU exchange */
            return i;
        }
    }
    return -1;
}

static int conn_find_locked(uint16_t handle)
{
    for (int i = 0; i < CLAW_BLE_MAX_CONN; i++) {
        if (s_conns[i].used && s_conns[i].handle == handle) {
            return i;
        }
    }
    return -1;
}

static uint16_t conn_handle_locked(int index)
{
    if (index < 0 || index >= CLAW_BLE_MAX_CONN || !s_conns[index].used) {
        return 0xFFFF;
    }
    return s_conns[index].handle;
}

/* ---- event enqueue (producer: bt_evt_task) ------------------------------ */

/* Push one event. On a full queue the NEWEST event is dropped (bt_evt_task must
 * never block). payload (if any) is freed on drop. */
static void evt_push(uint8_t type, int conn_index, uint8_t flag,
                     uint16_t err, uint8_t *payload, uint16_t len)
{
    lua_ble_evt_t ev;
    ev.type       = type;
    ev.conn_index = (int8_t)conn_index;
    ev.flag       = flag;
    ev.err        = err;
    ev.len        = len;
    ev.payload    = payload;

    if (!s_evt_queue ||
        rtos_queue_send(s_evt_queue, &ev, 0) != RTK_SUCCESS) {
        if (payload) {
            free(payload);
        }
        RTK_LOGS(NOTAG, RTK_LOG_WARN, "[ble] event queue full, dropped type=%d\n",
                 (int)type);
    }
}

/* ---- GAP event callback (bt_evt_task) ----------------------------------- */

static rtk_bt_evt_cb_ret_t ble_gap_cb(uint8_t evt_code, void *data, uint32_t len)
{
    (void)len;
    switch (evt_code) {

    case RTK_BT_LE_GAP_EVT_ADV_START_IND: {
        rtk_bt_le_adv_start_ind_t *ind = (rtk_bt_le_adv_start_ind_t *)data;
        lock();
        if (ind->err == 0) {
            s_advertising = true;
        }
        unlock();
        evt_push(LUA_BLE_EV_ADV_STARTED, -1, 0, ind->err, NULL, 0);
        break;
    }

    case RTK_BT_LE_GAP_EVT_ADV_STOP_IND: {
        rtk_bt_le_adv_stop_ind_t *ind = (rtk_bt_le_adv_stop_ind_t *)data;
        lock();
        s_advertising = false;
        unlock();
        evt_push(LUA_BLE_EV_ADV_STOPPED, -1, 0, (uint16_t)ind->stop_reason, NULL, 0);
        break;
    }

    case RTK_BT_LE_GAP_EVT_CONNECT_IND: {
        rtk_bt_le_conn_ind_t *ind = (rtk_bt_le_conn_ind_t *)data;
        int idx;
        lock();
        /* A successful connection stops legacy adv in the controller. */
        s_advertising = false;
        idx = (ind->err == 0) ? conn_add_locked(ind->conn_handle) : -1;
        unlock();
        evt_push(LUA_BLE_EV_CONNECTED, idx, 0, ind->err, NULL, 0);
        break;
    }

    case RTK_BT_LE_GAP_EVT_DISCONN_IND: {
        rtk_bt_le_disconn_ind_t *ind = (rtk_bt_le_disconn_ind_t *)data;
        int idx;
        lock();
        idx = conn_find_locked(ind->conn_handle);
        if (idx >= 0) {
            s_conns[idx].used = false;
        }
        unlock();
        evt_push(LUA_BLE_EV_DISCONNECTED, idx, 0, ind->reason, NULL, 0);
        break;
    }

    default:
        break;
    }
    return RTK_BT_EVT_CB_OK;
}

/* ---- GATTS event callback (bt_evt_task) --------------------------------- *
 * ATT requires read/write requests to be answered promptly, so read_resp /
 * write_resp are sent SYNCHRONOUSLY here (never deferred to the Lua poller).
 * Inbound data / subscribe state are additionally deep-copied onto the queue. */

static rtk_bt_evt_cb_ret_t ble_gatts_cb(uint8_t evt_code, void *data, uint32_t len)
{
    (void)len;
    switch (evt_code) {

    case RTK_BT_GATTS_EVT_REGISTER_SERVICE: {
        rtk_bt_gatts_reg_ind_t *ind = (rtk_bt_gatts_reg_ind_t *)data;
        RTK_LOGS(NOTAG, RTK_LOG_INFO, "[ble] service register status=0x%08x\n",
                 (unsigned int)ind->reg_status);
        break;
    }

    case RTK_BT_GATTS_EVT_READ_IND: {
        rtk_bt_gatts_read_ind_t *ind = (rtk_bt_gatts_read_ind_t *)data;
        rtk_bt_gatts_read_resp_param_t resp = {0};
        resp.app_id      = ind->app_id;
        resp.conn_handle = ind->conn_handle;
        resp.cid         = ind->cid;
        resp.index       = ind->index;
        if (ind->index == CLAW_BLE_ATTR_CHAR_VAL && ind->offset <= s_read_len) {
            resp.data = &s_read_val[ind->offset];
            resp.len  = (uint16_t)(s_read_len - ind->offset);
        } else if (ind->index == CLAW_BLE_ATTR_CHAR_VAL) {
            resp.err_code = RTK_BT_ATT_ERR_INVALID_OFFSET;
        } else {
            resp.err_code = RTK_BT_ATT_ERR_ATTR_NOT_FOUND;
        }
        rtk_bt_gatts_read_resp(&resp);
        break;
    }

    case RTK_BT_GATTS_EVT_WRITE_IND: {
        rtk_bt_gatts_write_ind_t *ind = (rtk_bt_gatts_write_ind_t *)data;
        rtk_bt_gatts_write_resp_param_t resp = {0};
        resp.app_id      = ind->app_id;
        resp.conn_handle = ind->conn_handle;
        resp.cid         = ind->cid;
        resp.index       = ind->index;
        resp.type        = ind->type;

        /* Answer the ATT layer first (synchronously), then hand data to Lua. */
        rtk_bt_gatts_write_resp(&resp);

        if (ind->index == CLAW_BLE_ATTR_CHAR_VAL && ind->len && ind->value) {
            int idx;
            uint16_t n = ind->len;
            if (n > CLAW_BLE_PAYLOAD_MAX) {
                n = CLAW_BLE_PAYLOAD_MAX;   /* long-write reassembly is future work */
            }
            lock();
            idx = conn_find_locked(ind->conn_handle);
            unlock();
            uint8_t *copy = (uint8_t *)malloc(n);
            if (copy) {
                memcpy(copy, ind->value, n);
                evt_push(LUA_BLE_EV_DATA, idx, 0, 0, copy, n);
            } else {
                /* write_resp was already sent (ATT requires it synchronously),
                 * so the peer thinks the write succeeded — log the loss rather
                 * than drop it silently. */
                RTK_LOGS(NOTAG, RTK_LOG_ERROR,
                         "[ble] write payload malloc(%d) failed, data dropped\n",
                         (int)n);
            }
        }
        break;
    }

    case RTK_BT_GATTS_EVT_CCCD_IND: {
        rtk_bt_gatts_cccd_ind_t *ind = (rtk_bt_gatts_cccd_ind_t *)data;
        int idx;
        uint8_t on = (ind->value & RTK_BT_GATT_CCC_NOTIFY) ? 1 : 0;
        if (ind->index != CLAW_BLE_ATTR_CCCD) {
            break;
        }
        lock();
        idx = conn_find_locked(ind->conn_handle);
        if (idx >= 0) {
            s_conns[idx].subscribed = on;
        }
        unlock();
        evt_push(LUA_BLE_EV_SUBSCRIBE, idx, on, 0, NULL, 0);
        break;
    }

    case RTK_BT_GATTS_EVT_MTU_EXCHANGE: {
        rtk_bt_gatt_mtu_exchange_ind_t *ind = (rtk_bt_gatt_mtu_exchange_ind_t *)data;
        int idx;
        lock();
        idx = conn_find_locked(ind->conn_handle);
        if (idx >= 0) {
            s_conns[idx].mtu = ind->mtu_size;
        }
        unlock();
        evt_push(LUA_BLE_EV_MTU, idx, 0, ind->mtu_size, NULL, 0);
        break;
    }

    default:
        break;
    }
    return RTK_BT_EVT_CB_OK;
}

/* ---- lifecycle ---------------------------------------------------------- */

int lua_ble_bridge_init(const char *name)
{
    uint16_t ret;

    if (s_enabled) {
        if (name && name[0]) {
            lua_ble_bridge_set_name(name);
        }
        return 0;
    }

    if (!s_lock) {
        if (rtos_mutex_create(&s_lock) != RTK_SUCCESS) {
            RTK_LOGS(NOTAG, RTK_LOG_ERROR, "[ble] mutex create failed\n");
            return RTK_BT_ERR_NO_MEMORY;
        }
    }
    if (!s_evt_queue) {
        if (rtos_queue_create(&s_evt_queue, CLAW_BLE_EVENT_QUEUE_DEPTH,
                              sizeof(lua_ble_evt_t)) != RTK_SUCCESS) {
            RTK_LOGS(NOTAG, RTK_LOG_ERROR, "[ble] event queue create failed\n");
            return RTK_BT_ERR_NO_MEMORY;
        }
    }

    lock();
    memset(s_conns, 0, sizeof(s_conns));
    s_advertising = false;
    unlock();

    if (name && name[0]) {
        strncpy(s_name, name, sizeof(s_name) - 1);
        s_name[sizeof(s_name) - 1] = '\0';
    }

    rtk_bt_app_conf_t conf = {0};
    conf.app_profile_support = RTK_BT_PROFILE_GATTS;
    conf.mtu_size            = CLAW_BLE_MTU;
    conf.master_init_mtu_req = true;
    conf.slave_init_mtu_req  = false;
    conf.user_def_service    = false;   /* use stack's builtin GAP/GATT service */
    conf.cccd_not_check      = false;   /* enforce CCCD subscription before notify */

    ret = rtk_bt_enable(&conf);
    if (ret != 0) {
        RTK_LOGS(NOTAG, RTK_LOG_ERROR, "[ble] rtk_bt_enable failed: 0x%08x\n",
                 (unsigned int)ret);
        return (int)ret;
    }

    ret = rtk_bt_evt_register_callback(RTK_BT_LE_GP_GAP, ble_gap_cb);
    if (ret != 0) {
        RTK_LOGS(NOTAG, RTK_LOG_ERROR, "[ble] register GAP cb failed: 0x%08x\n",
                 (unsigned int)ret);
        rtk_bt_disable();
        return (int)ret;
    }

    ret = rtk_bt_evt_register_callback(RTK_BT_LE_GP_GATTS, ble_gatts_cb);
    if (ret != 0) {
        RTK_LOGS(NOTAG, RTK_LOG_ERROR, "[ble] register GATTS cb failed: 0x%08x\n",
                 (unsigned int)ret);
        rtk_bt_disable();
        return (int)ret;
    }

    /* Register the hard-wired fff0/fff1 transparent service. */
    s_service.type            = GATT_SERVICE_OVER_BLE;
    s_service.server_info     = 0;
    s_service.user_data       = NULL;
    s_service.register_status = 0;
    ret = rtk_bt_gatts_register_service(&s_service);
    if (ret != 0) {
        RTK_LOGS(NOTAG, RTK_LOG_ERROR, "[ble] register service failed: 0x%08x\n",
                 (unsigned int)ret);
        rtk_bt_disable();
        return (int)ret;
    }

    rtk_bt_le_gap_set_device_name((const uint8_t *)s_name);

    s_enabled = true;
    RTK_LOGS(NOTAG, RTK_LOG_INFO, "[ble] enabled, name='%s'\n", s_name);
    return 0;
}

int lua_ble_bridge_deinit(void)
{
    if (!s_enabled) {
        return 0;
    }
    if (s_advertising) {
        rtk_bt_le_gap_stop_adv();
    }
    rtk_bt_evt_unregister_callback(RTK_BT_LE_GP_GATTS);
    rtk_bt_evt_unregister_callback(RTK_BT_LE_GP_GAP);
    rtk_bt_disable();

    lock();
    memset(s_conns, 0, sizeof(s_conns));
    s_advertising = false;
    unlock();

    /* Drain any leftover events (free their payloads). */
    if (s_evt_queue) {
        lua_ble_evt_t ev;
        while (rtos_queue_receive(s_evt_queue, &ev, 0) == RTK_SUCCESS) {
            if (ev.payload) {
                free(ev.payload);
            }
        }
    }

    s_enabled = false;
    RTK_LOGS(NOTAG, RTK_LOG_INFO, "[ble] disabled\n");
    return 0;
}

bool lua_ble_bridge_is_enabled(void)
{
    return s_enabled;
}

/* ---- GAP helpers -------------------------------------------------------- */

int lua_ble_bridge_get_addr(char *out, size_t out_len)
{
    rtk_bt_le_addr_t addr = {(rtk_bt_le_addr_type_t)0, {0}};
    uint16_t ret;

    if (!s_enabled) {
        return RTK_BT_ERR_NOT_READY;
    }
    ret = rtk_bt_le_gap_get_bd_addr(&addr);
    if (ret != 0) {
        return (int)ret;
    }
    rtk_bt_le_addr_to_str(&addr, out, (uint32_t)out_len);
    return 0;
}

int lua_ble_bridge_set_name(const char *name)
{
    if (!name || !name[0]) {
        return RTK_BT_ERR_PARAM_INVALID;
    }
    strncpy(s_name, name, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';
    if (!s_enabled) {
        return 0;   /* stored; applied at next init */
    }
    return (int)rtk_bt_le_gap_set_device_name((const uint8_t *)s_name);
}

int lua_ble_bridge_adv_start(void)
{
    if (!s_enabled) {
        return RTK_BT_ERR_NOT_READY;
    }

#if defined(RTK_BLE_5_0_USE_EXTENDED_ADV) && RTK_BLE_5_0_USE_EXTENDED_ADV
    /* This module targets the non-extended-adv config (LE Audio/ISO off). If a
     * build enables extended adv, the legacy path below is not compiled — fail
     * loudly rather than silently doing nothing. */
    RTK_LOGS(NOTAG, RTK_LOG_ERROR,
             "[ble] adv_start: extended-adv build not supported\n");
    return RTK_BT_ERR_UNSUPPORTED;
#else
    uint16_t ret;
    uint8_t  name_len = (uint8_t)strlen(s_name);

    /* AD structure 1: Flags (LE General Discoverable + BR/EDR not supported).
     * AD structure 2: Complete list of 16-bit Service UUIDs (fff0, LE order). */
    uint8_t adv_data[] = {
        0x02, 0x01, 0x06,
        0x03, 0x03, (uint8_t)(CLAW_BLE_SVC_UUID & 0xFF),
                    (uint8_t)((CLAW_BLE_SVC_UUID >> 8) & 0xFF),
    };

    /* Scan response: Complete Local Name (clamped to fit a 31-byte PDU). */
    uint8_t scan_rsp[31];
    if (name_len > (uint8_t)(sizeof(scan_rsp) - 2)) {
        name_len = (uint8_t)(sizeof(scan_rsp) - 2);
    }
    scan_rsp[0] = (uint8_t)(name_len + 1);
    scan_rsp[1] = 0x09;                 /* Complete Local Name */
    memcpy(&scan_rsp[2], s_name, name_len);

    rtk_bt_le_adv_param_t adv_param = {0};
    adv_param.interval_min  = CLAW_BLE_ADV_INTERVAL_MIN;
    adv_param.interval_max  = CLAW_BLE_ADV_INTERVAL_MAX;
    adv_param.type          = RTK_BT_LE_ADV_TYPE_IND;   /* connectable + scannable */
    adv_param.own_addr_type = RTK_BT_LE_ADDR_TYPE_PUBLIC;
    adv_param.channel_map   = RTK_BT_LE_ADV_CHNL_ALL;
    adv_param.filter_policy = RTK_BT_LE_ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;

    ret = rtk_bt_le_gap_set_adv_data(adv_data, sizeof(adv_data));
    if (ret != 0) {
        return (int)ret;
    }
    ret = rtk_bt_le_gap_set_scan_rsp_data(scan_rsp, (uint32_t)(name_len + 2));
    if (ret != 0) {
        return (int)ret;
    }
    ret = rtk_bt_le_gap_start_adv(&adv_param);
    if (ret != 0) {
        return (int)ret;
    }
    /* s_advertising is set true on the ADV_START_IND event (authoritative). */
    return 0;
#endif
}

int lua_ble_bridge_adv_stop(void)
{
    if (!s_enabled) {
        return RTK_BT_ERR_NOT_READY;
    }
    return (int)rtk_bt_le_gap_stop_adv();
}

int lua_ble_bridge_disconnect(int conn_index)
{
    uint16_t handle;

    if (!s_enabled) {
        return RTK_BT_ERR_NOT_READY;
    }
    lock();
    handle = conn_handle_locked(conn_index);
    unlock();
    if (handle == 0xFFFF) {
        return RTK_BT_ERR_NO_CONNECTION;
    }
    return (int)rtk_bt_le_gap_disconnect(handle);
}

int lua_ble_bridge_notify(int conn_index, const uint8_t *data, uint16_t len)
{
    uint16_t handle;
    uint16_t mtu;
    bool     subscribed;

    if (!s_enabled) {
        return RTK_BT_ERR_NOT_READY;
    }
    if (!data || len == 0) {
        return RTK_BT_ERR_PARAM_INVALID;
    }
    if (len > CLAW_BLE_PAYLOAD_MAX) {
        return RTK_BT_ERR_PDU_SIZE_INVALID;
    }

    lock();
    handle     = conn_handle_locked(conn_index);
    mtu        = (conn_index >= 0 && conn_index < CLAW_BLE_MAX_CONN)
                 ? s_conns[conn_index].mtu : 0;
    subscribed = (conn_index >= 0 && conn_index < CLAW_BLE_MAX_CONN)
                 ? s_conns[conn_index].subscribed : false;
    unlock();

    if (handle == 0xFFFF) {
        return RTK_BT_ERR_NO_CONNECTION;
    }
    if (!subscribed) {
        /* Peer has not enabled fff1 notify; the stack would reject the send. */
        return RTK_BT_ERR_STATE_INVALID;
    }
    /* Single-packet notify is limited to MTU - 3 (ATT header). Fragmentation is
     * future work; reject over-long payloads with a clear error. */
    if (len > (uint16_t)(mtu - 3)) {
        return RTK_BT_ERR_PDU_SIZE_INVALID;
    }

    rtk_bt_gatts_ntf_and_ind_param_t ntf = {0};
    ntf.app_id      = CLAW_BLE_APP_ID;
    ntf.conn_handle = handle;
    ntf.index       = CLAW_BLE_ATTR_CHAR_VAL;
    ntf.data        = data;
    ntf.len         = len;
    ntf.seq         = 0;
    return (int)rtk_bt_gatts_notify(&ntf);
}

/* ---- event poll (consumer: Lua job) ------------------------------------- */

int lua_ble_bridge_poll(lua_ble_evt_t *out, uint32_t timeout_ms)
{
    if (!s_evt_queue || !out) {
        return -1;
    }
    if (rtos_queue_receive(s_evt_queue, out, timeout_ms) == RTK_SUCCESS) {
        return 1;
    }
    return 0;
}

void lua_ble_bridge_evt_free(lua_ble_evt_t *evt)
{
    if (evt && evt->payload) {
        free(evt->payload);
        evt->payload = NULL;
    }
}

void lua_ble_bridge_stats(lua_ble_stats_t *out)
{
    if (!out) {
        return;
    }
    uint8_t count = 0;
    lock();
    for (int i = 0; i < CLAW_BLE_MAX_CONN; i++) {
        if (s_conns[i].used) {
            count++;
        }
    }
    out->advertising = s_advertising;
    unlock();
    out->enabled     = s_enabled;
    out->conn_count  = count;
    out->max_conn    = CLAW_BLE_MAX_CONN;
}

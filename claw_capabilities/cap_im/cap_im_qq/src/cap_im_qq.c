/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * QQ Bot via WebSocket long connection (QQ official bot API).
 *
 * Flow:
 *   1. POST bots.qq.com/app/getAppAccessToken → access_token
 *   2. GET  api.sgroup.qq.com/gateway         → WSS URL
 *   3. Connect WSS, poll loop
 *   4. On OP_HELLO(10) → send OP_IDENTIFY(2)
 *   5. On OP_DISPATCH(0) → parse C2C / GROUP_AT message → publish to event router
 *   6. Periodic OP_HEARTBEAT(1) at server-specified interval
 *   7. On disconnect → delay + reconnect
 */
#include "cap_im_qq.h"
#include "claw_cap.h"
#include "claw_compat.h"
#include "claw_config.h"
#include "claw_wifi_mgr.h"
#include "claw_event_publisher.h"
#include "claw_im_dispatch.h"
#include "llm_agent_http.h"
#include "os_wrapper.h"
#include "websocket/wsclient_api.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "cap_im_qq";

/* ---- Constants ---- */

#define QQ_TOKEN_HOST             "bots.qq.com"
#define QQ_TOKEN_PATH             "/app/getAppAccessToken"
#define QQ_API_HOST               "api.sgroup.qq.com"
#define QQ_GATEWAY_PATH           "/gateway"
#define QQ_MSG_PATH_C2C_FMT       "/v2/users/%s/messages"
#define QQ_MSG_PATH_GROUP_FMT     "/v2/groups/%s/messages"

#define QQ_WS_HOST_MAX            128
#define QQ_WS_PATH_MAX            512
#define QQ_TOKEN_BUF_LEN          512
#define QQ_AUTH_HDR_MAX           544   /* "QQBot " + token */
#define QQ_TX_BUF                 512
#define QQ_WS_RX_BUF              4096
#define QQ_WS_QUEUE_SIZE          4
#define QQ_RECONNECT_DELAY_MS     5000
#define QQ_WAIT_CONFIG_DELAY_MS   30000
#define QQ_TOKEN_REFRESH_MARGIN_S 300
#define QQ_DEDUP_CACHE_SIZE       32
#define QQ_MSG_CHUNK_MAX          1500
#define QQ_TASK_STACK_WORDS       (12 * 1024 / 4)
#define QQ_TASK_PRIORITY          2

#define QQ_OP_DISPATCH            0
#define QQ_OP_HEARTBEAT           1
#define QQ_OP_IDENTIFY            2
#define QQ_OP_RECONNECT           7
#define QQ_OP_INVALID_SESSION     9
#define QQ_OP_HELLO               10
#define QQ_OP_HEARTBEAT_ACK       11

#define QQ_INTENTS_C2C_MSG        (1u << 30)
#define QQ_INTENTS_GROUP_MSG      (1u << 25)
#define QQ_INTENTS                (QQ_INTENTS_C2C_MSG | QQ_INTENTS_GROUP_MSG)

/* ---- Module state ---- */

static char              s_app_id[64];
static char              s_app_secret[128];
static int8_t            s_msg_type;

static rtos_mutex_t      s_token_mutex;
static rtos_mutex_t      s_start_mutex;
static char             *s_token;
static uint32_t          s_token_ts_ms;
static uint32_t          s_token_expires_in_s;

static wsclient_context *s_ws_client;
static rtos_task_t       s_ws_task;
static volatile bool     s_ws_stop;
static volatile bool     s_ws_connected;
static volatile int      s_hb_interval_ms;
static volatile int      s_last_seq;
static volatile bool     s_identify_pending;

static char              s_ws_host[QQ_WS_HOST_MAX];
static char              s_ws_path[QQ_WS_PATH_MAX];
static uint16_t          s_ws_port;

static uint64_t          s_seen_hashes[QQ_DEDUP_CACHE_SIZE];
static int               s_seen_idx;


static volatile bool     s_wifi_ready;
static volatile bool     s_started;

/* ---- Dedup ---- */

static uint64_t qq_fnv1a64(const char *s)
{
    uint64_t h = 14695981039346656037ULL;
    while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ULL; }
    return h;
}

static bool qq_dedup_check_and_record(const char *msg_id)
{
    uint64_t key = qq_fnv1a64(msg_id);
    for (int i = 0; i < QQ_DEDUP_CACHE_SIZE; i++) {
        if (s_seen_hashes[i] == key) return true;
    }
    s_seen_hashes[s_seen_idx] = key;
    s_seen_idx = (s_seen_idx + 1) % QQ_DEDUP_CACHE_SIZE;
    return false;
}

/* ---- API response helpers ---- */

/* Returns the QQ API error code, 0 on success or non-JSON response */
static int qq_parse_api_code(const char *resp_buf)
{
    if (!resp_buf) return 0;
    cJSON *r = cJSON_Parse(resp_buf);
    if (!r) return 0;
    cJSON *code_j = cJSON_GetObjectItem(r, "code");
    int code = (cJSON_IsNumber(code_j)) ? (int)code_j->valuedouble : 0;
    if (code != 0) {
        cJSON *msg_j = cJSON_GetObjectItem(r, "message");
        RTK_LOGW(TAG, "QQ API err code=%d msg=%.80s\n", code,
                 (cJSON_IsString(msg_j) && msg_j->valuestring) ? msg_j->valuestring : "");
    }
    cJSON_Delete(r);
    return code;
}

/* Token-related QQ API error codes that warrant a token refresh and retry */
static bool qq_is_token_error(int code)
{
    return (code == 11241 || code == 11242 || code == 40001 || code == 304001);
}

/* ---- UTF-8 safe chunking ---- */

static size_t qq_utf8_chunk_end(const char *text, size_t offset, size_t max_bytes)
{
    const char *p = text + offset;
    size_t remaining = strlen(p);
    if (remaining <= max_bytes) return remaining;
    size_t n = max_bytes;
    while (n > 0 && ((uint8_t)p[n] & 0xC0) == 0x80) n--;
    return n;
}

/* ---- Token management ---- */

static int qq_get_token_copy(char *buf, size_t size)
{
    rtos_mutex_take(s_token_mutex, 0xFFFFFFFFUL);
    int ret = RTK_FAIL;
    if (s_token && s_token[0] && strlen(s_token) + 7 < size) {
        strlcpy(buf, "QQBot ", size);
        strlcat(buf, s_token, size);
        ret = RTK_SUCCESS;
    }
    rtos_mutex_give(s_token_mutex);
    return ret;
}

static int qq_fetch_token(void)
{
    rtos_mutex_take(s_token_mutex, 0xFFFFFFFFUL);
    uint32_t now_ms = rtos_time_get_current_system_time_ms();
    uint32_t age_s  = (now_ms - s_token_ts_ms) / 1000;
    bool needs_refresh = (s_token == NULL || s_token[0] == '\0' ||
                          age_s + QQ_TOKEN_REFRESH_MARGIN_S >= s_token_expires_in_s);
    if (!needs_refresh) {
        rtos_mutex_give(s_token_mutex);
        return RTK_SUCCESS;
    }
    rtos_mutex_give(s_token_mutex);

    /* Snapshot credentials under the mutex before the long HTTP call */
    char local_app_id[sizeof(s_app_id)];
    char local_app_secret[sizeof(s_app_secret)];
    rtos_mutex_take(s_token_mutex, 0xFFFFFFFFUL);
    strlcpy(local_app_id,     s_app_id,     sizeof(local_app_id));
    strlcpy(local_app_secret, s_app_secret, sizeof(local_app_secret));
    rtos_mutex_give(s_token_mutex);

    cJSON *body_obj = cJSON_CreateObject();
    if (!body_obj) return RTK_FAIL;
    cJSON_AddStringToObject(body_obj, "appId",        local_app_id);
    cJSON_AddStringToObject(body_obj, "clientSecret", local_app_secret);
    char *body_str = cJSON_PrintUnformatted(body_obj);
    cJSON_Delete(body_obj);
    if (!body_str) return RTK_FAIL;

    llm_http_resp_t resp;
    if (llm_http_resp_init(&resp) != 0) { free(body_str); return RTK_FAIL; }

    int ret = llm_http_post_no_auth(QQ_TOKEN_HOST, QQ_TOKEN_PATH,
                                    body_str, strlen(body_str), &resp);
    free(body_str);

    if (ret != 0 || resp.len == 0) {
        RTK_LOGE(TAG, "token request failed (ret=%d)\n", ret);
        llm_http_resp_free(&resp);
        return RTK_FAIL;
    }

    cJSON *root = cJSON_Parse(resp.buf);
    llm_http_resp_free(&resp);
    if (!root) { RTK_LOGE(TAG, "token response parse failed\n"); return RTK_FAIL; }

    cJSON *token_j   = cJSON_GetObjectItem(root, "access_token");
    cJSON *expires_j = cJSON_GetObjectItem(root, "expires_in");

    if (!cJSON_IsString(token_j) || !token_j->valuestring[0]) {
        RTK_LOGE(TAG, "token response missing access_token\n");
        cJSON_Delete(root);
        return RTK_FAIL;
    }

    if (strlen(token_j->valuestring) >= QQ_TOKEN_BUF_LEN) {
        RTK_LOGE(TAG, "token too long\n");
        cJSON_Delete(root);
        return RTK_FAIL;
    }

    rtos_mutex_take(s_token_mutex, 0xFFFFFFFFUL);
    /* Double-check: another task may have refreshed the token while we were doing the HTTP call */
    uint32_t now2_ms  = rtos_time_get_current_system_time_ms();
    uint32_t age2_s   = (now2_ms - s_token_ts_ms) / 1000;
    bool still_stale  = (s_token == NULL || s_token[0] == '\0' ||
                         age2_s + QQ_TOKEN_REFRESH_MARGIN_S >= s_token_expires_in_s);
    if (still_stale) {
        if (!s_token) s_token = malloc(QQ_TOKEN_BUF_LEN);
        if (s_token) {
            strlcpy(s_token, token_j->valuestring, QQ_TOKEN_BUF_LEN);
            s_token_ts_ms        = now2_ms;
            s_token_expires_in_s = cJSON_IsNumber(expires_j)
                                   ? (uint32_t)expires_j->valueint : 7200;
        }
    }
    rtos_mutex_give(s_token_mutex);

    cJSON_Delete(root);
    if (!s_token) return RTK_FAIL;

    RTK_LOGI(TAG, "token refreshed (%.16s...)\n", s_token);
    return RTK_SUCCESS;
}

/* ---- WS URL parsing ---- */

static bool qq_parse_ws_url(const char *url,
                             char *host_out, size_t host_max,
                             char *path_out, size_t path_max,
                             uint16_t *port_out)
{
    const char *p = strstr(url, "://");
    if (!p) return false;
    p += 3;

    const char *slash = strpbrk(p, "/?");
    size_t authority_len = slash ? (size_t)(slash - p) : strlen(p);
    if (authority_len == 0) return false;

    /* Split host:port if present */
    const char *colon = memchr(p, ':', authority_len);
    size_t hlen = colon ? (size_t)(colon - p) : authority_len;
    if (hlen == 0 || hlen >= host_max) return false;
    memcpy(host_out, p, hlen);
    host_out[hlen] = '\0';
    *port_out = colon ? (uint16_t)atoi(colon + 1) : 443;

    const char *path = slash ? slash : "/";
    if (strlen(path) >= path_max) return false;
    strlcpy(path_out, path, path_max);
    return true;
}

static int qq_fetch_gateway_url(void)
{
    char auth_hdr[QQ_AUTH_HDR_MAX];
    if (qq_get_token_copy(auth_hdr, sizeof(auth_hdr)) != RTK_SUCCESS) return RTK_FAIL;

    llm_http_resp_t resp;
    if (llm_http_resp_init(&resp) != 0) return RTK_FAIL;

    int ret = llm_http_get_auth(QQ_API_HOST, QQ_GATEWAY_PATH, auth_hdr, &resp);
    if (ret != 0 || resp.len == 0) {
        RTK_LOGE(TAG, "gateway request failed (ret=%d)\n", ret);
        llm_http_resp_free(&resp);
        return RTK_FAIL;
    }

    cJSON *root = cJSON_Parse(resp.buf);
    llm_http_resp_free(&resp);
    if (!root) { RTK_LOGE(TAG, "gateway response parse failed\n"); return RTK_FAIL; }

    cJSON *url_j = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(url_j)) {
        RTK_LOGE(TAG, "gateway response missing url\n");
        cJSON_Delete(root);
        return RTK_FAIL;
    }

    if (!qq_parse_ws_url(url_j->valuestring,
                         s_ws_host, sizeof(s_ws_host),
                         s_ws_path, sizeof(s_ws_path),
                         &s_ws_port)) {
        RTK_LOGE(TAG, "gateway URL parse failed: %.80s\n", url_j->valuestring);
        cJSON_Delete(root);
        return RTK_FAIL;
    }

    cJSON_Delete(root);
    RTK_LOGI(TAG, "gateway: host=%s path=%.40s\n", s_ws_host, s_ws_path);
    return RTK_SUCCESS;
}

/* ---- WS send helpers ---- */

static int qq_ws_send_json(const char *json_str)
{
    if (!s_ws_client || !json_str) return -1;
    return ws_send((char *)json_str, (int)strlen(json_str), 1, s_ws_client);
}

static bool qq_send_identify(void)
{
    char *auth_hdr = malloc(QQ_AUTH_HDR_MAX);
    if (!auth_hdr) return false;
    if (qq_get_token_copy(auth_hdr, QQ_AUTH_HDR_MAX) != RTK_SUCCESS) {
        free(auth_hdr); return false;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) { free(auth_hdr); return false; }
    cJSON_AddNumberToObject(root, "op", QQ_OP_IDENTIFY);
    cJSON *d = cJSON_CreateObject();
    if (!d) { cJSON_Delete(root); free(auth_hdr); return false; }
    cJSON_AddStringToObject(d, "token",   auth_hdr);
    cJSON_AddNumberToObject(d, "intents", (double)QQ_INTENTS);
    cJSON_AddItemToObject(root, "d", d);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(auth_hdr);

    if (!json_str) return false;
    int rc = qq_ws_send_json(json_str);
    free(json_str);
    if (rc < 0) { RTK_LOGW(TAG, "IDENTIFY send failed\n"); return false; }
    RTK_LOGI(TAG, "IDENTIFY sent\n");
    return true;
}

static int qq_send_heartbeat(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;
    cJSON_AddNumberToObject(root, "op", QQ_OP_HEARTBEAT);
    if (s_last_seq >= 0)
        cJSON_AddItemToObject(root, "d", cJSON_CreateNumber(s_last_seq));
    else
        cJSON_AddItemToObject(root, "d", cJSON_CreateNull());

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return -1;
    int rc = qq_ws_send_json(json_str);
    free(json_str);
    return rc;
}

/* ---- Message dispatch ---- */

static void qq_handle_dispatch(const char *event_type, const cJSON *d)
{
    if (!event_type || !d) return;

    cJSON *id_j = cJSON_GetObjectItem(d, "id");
    if (!cJSON_IsString(id_j)) return;
    const char *id_str = id_j->valuestring;

    if (qq_dedup_check_and_record(id_str)) {
        RTK_LOGD(TAG, "dup msg %s\n", id_str);
        return;
    }

    char chat_id[192];
    const char *sender_id = "";

    if (strcmp(event_type, "C2C_MESSAGE_CREATE") == 0) {
        cJSON *author = cJSON_GetObjectItem(d, "author");
        cJSON *openid = author ? cJSON_GetObjectItem(author, "user_openid") : NULL;
        if (!cJSON_IsString(openid)) return;
        sender_id = openid->valuestring;
        DiagSnPrintf(chat_id, sizeof(chat_id), "c2c:%s", openid->valuestring);
    } else if (strcmp(event_type, "GROUP_AT_MESSAGE_CREATE") == 0) {
        cJSON *grp_id = cJSON_GetObjectItem(d, "group_openid");
        cJSON *author = cJSON_GetObjectItem(d, "author");
        cJSON *openid = author ? cJSON_GetObjectItem(author, "member_openid") : NULL;
        if (!cJSON_IsString(grp_id)) return;
        sender_id = cJSON_IsString(openid) ? openid->valuestring : "";
        DiagSnPrintf(chat_id, sizeof(chat_id), "group:%s", grp_id->valuestring);
    } else {
        return;
    }

    cJSON *content_j = cJSON_GetObjectItem(d, "content");
    const char *content = (cJSON_IsString(content_j) && content_j->valuestring)
                          ? content_j->valuestring : "";

    /* Strip leading @bot mention prefix if present */
    while (*content == ' ' || *content == '\n') content++;

    RTK_LOGI(TAG, "recv [%s] from %s: %.60s\n", chat_id, sender_id, content);
    claw_event_dispatcher_publish_message("qq_gateway", "qq",
                                          chat_id, content, sender_id, id_str);
}

/* ---- WS receive callback ---- */

static void qq_ws_on_data(wsclient_context **ctx, int len, enum opcode_type opcode)
{
    if (len <= 0 || !(*ctx)->receivedData) return;
    if (opcode != TEXT_FRAME) return;

    cJSON *root = cJSON_ParseWithLength((const char *)(*ctx)->receivedData, (size_t)len);
    if (!root) { RTK_LOGW(TAG, "WS JSON parse failed\n"); return; }

    cJSON *op_j = cJSON_GetObjectItem(root, "op");
    cJSON *s_j  = cJSON_GetObjectItem(root, "s");
    cJSON *t_j  = cJSON_GetObjectItem(root, "t");
    cJSON *d_j  = cJSON_GetObjectItem(root, "d");

    if (cJSON_IsNumber(s_j)) s_last_seq = s_j->valueint;

    int op = cJSON_IsNumber(op_j) ? op_j->valueint : -1;

    switch (op) {
    case QQ_OP_HELLO:
        if (cJSON_IsObject(d_j)) {
            cJSON *hb = cJSON_GetObjectItem(d_j, "heartbeat_interval");
            if (cJSON_IsNumber(hb)) {
                int interval = hb->valueint;
                if (interval < 5000)  interval = 5000;
                if (interval > 120000) interval = 120000;
                s_hb_interval_ms = interval;
            }
        }
        s_identify_pending = true;
        RTK_LOGI(TAG, "HELLO, hb_interval=%d ms\n", s_hb_interval_ms);
        break;

    case QQ_OP_DISPATCH:
        if (cJSON_IsString(t_j) && cJSON_IsObject(d_j))
            qq_handle_dispatch(t_j->valuestring, d_j);
        break;

    case QQ_OP_HEARTBEAT_ACK:
        RTK_LOGD(TAG, "HEARTBEAT_ACK\n");
        break;

    case QQ_OP_RECONNECT:
        RTK_LOGW(TAG, "server requested reconnect, reconnecting\n");
        s_ws_stop = true;
        break;

    case QQ_OP_INVALID_SESSION: {
        bool resumable = cJSON_IsBool(d_j) && cJSON_IsTrue(d_j);
        RTK_LOGW(TAG, "invalid session (resumable=%d), reconnecting\n", resumable);
        s_ws_stop = true;
        break;
    }

    default:
        RTK_LOGD(TAG, "op=%d ignored\n", op);
        break;
    }

    cJSON_Delete(root);
}

/* ---- WS task ---- */

static void qq_ws_task(void *arg)
{
    (void)arg;
    ws_dispatch(qq_ws_on_data);

    while (true) {
        if (!s_app_id[0] || !s_app_secret[0]) {
            RTK_LOGI(TAG, "no credentials, waiting\n");
            rtos_time_delay_ms(QQ_WAIT_CONFIG_DELAY_MS);
            continue;
        }

        if (claw_wifi_mgr_get_state() != CLAW_WIFI_STATE_CONNECTED) {
            RTK_LOGI(TAG, "WiFi not ready, waiting\n");
            if (s_ws_stop) break;
            rtos_time_delay_ms(2000);
            continue;
        }

        if (qq_fetch_token() != RTK_SUCCESS) {
            rtos_time_delay_ms(QQ_RECONNECT_DELAY_MS);
            continue;
        }
        if (qq_fetch_gateway_url() != RTK_SUCCESS) {
            rtos_time_delay_ms(QQ_RECONNECT_DELAY_MS);
            continue;
        }

        char ws_url[QQ_WS_HOST_MAX + 8];
        DiagSnPrintf(ws_url, sizeof(ws_url), "wss://%s", s_ws_host);

        s_ws_stop          = false;
        s_ws_connected     = false;
        s_identify_pending = false;
        s_hb_interval_ms   = 30000;
        s_last_seq         = -1;

        wsclient_context *wsc = create_wsclient(ws_url, s_ws_port, s_ws_path, NULL,
                                                QQ_TX_BUF, QQ_WS_RX_BUF,
                                                QQ_WS_QUEUE_SIZE);
        if (!wsc) {
            RTK_LOGE(TAG, "create_wsclient failed\n");
            rtos_time_delay_ms(QQ_RECONNECT_DELAY_MS);
            continue;
        }

        wsclient_set_fun_ops(wsc);
        ws_setsockopt_timeout(10000, 10000, 15000);
        /* Detect dead TCP connections in ~45s (idle 30s + 3×5s probes)
         * instead of waiting 4 heartbeat cycles ≈ 2.75 min for buffer full. */
        ws_setsockopt_keepalive(30, 5, 3);
        RTK_LOGI(TAG, "connecting to %s ...\n", s_ws_host);
        if (ws_connect_url(wsc) < 0) {
            RTK_LOGE(TAG, "ws_connect_url failed\n");
            /* ws_connect_url already called client_close on failure; only free the struct */
            ws_free(wsc);
            rtos_time_delay_ms(QQ_RECONNECT_DELAY_MS);
            continue;
        }

        s_ws_client    = wsc;
        s_ws_connected = true;
        RTK_LOGI(TAG, "WS connected\n");

        uint32_t last_hb_ms = rtos_time_get_current_system_time_ms();

        while (!s_ws_stop && wsc && wsc->readyState != WSC_CLOSED) {
            ws_poll(200, &wsc);
            if (!wsc) {
                s_ws_connected = false;
                break;
            }

            if (s_identify_pending) {
                if (qq_send_identify()) {
                    s_identify_pending = false;
                    last_hb_ms = rtos_time_get_current_system_time_ms();
                } else {
                    s_ws_stop = true;
                }
            }

            uint32_t now_ms = rtos_time_get_current_system_time_ms();
            if (s_ws_connected &&
                (now_ms - last_hb_ms) >= (uint32_t)s_hb_interval_ms) {
                if (qq_send_heartbeat() < 0) {
                    RTK_LOGW(TAG, "heartbeat send failed, reconnecting\n");
                    s_ws_stop = true;
                }
                last_hb_ms = now_ms;
            }
        }

        s_ws_connected = false;
        s_ws_client    = NULL;
        /* Graceful close: send CLOSE frame if still open, then drain until WSC_CLOSED */
        if (wsc && wsc->readyState == WSC_OPEN)
            ws_close(&wsc);
        for (int i = 0; wsc && wsc->readyState != WSC_CLOSED && i < 20; i++)
            ws_poll(200, &wsc);
        /* Free the wsclient_context struct (buffers freed by client_close above) */
        if (wsc) { ws_free(wsc); wsc = NULL; }
        RTK_LOGW(TAG, "WS disconnected, retry in %d ms\n", QQ_RECONNECT_DELAY_MS);
        rtos_time_delay_ms(QQ_RECONNECT_DELAY_MS);
    }

    rtos_task_delete(NULL);
}

/* ---- try_start ---- */

static void qq_try_start(void)
{
    rtos_mutex_take(s_start_mutex, 0xFFFFFFFFUL);
    bool should_start = (!s_started && s_wifi_ready && s_app_id[0] && s_app_secret[0]);
    if (should_start) s_started = true;
    rtos_mutex_give(s_start_mutex);

    if (!should_start) return;
    if (rtos_task_create(&s_ws_task, "qq_ws", qq_ws_task,
                         NULL, QQ_TASK_STACK_WORDS, QQ_TASK_PRIORITY) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "task create failed\n");
        rtos_mutex_take(s_start_mutex, 0xFFFFFFFFUL);
        s_started = false;
        rtos_mutex_give(s_start_mutex);
    } else {
        RTK_LOGI(TAG, "WS task started\n");
    }
}

/* ---- Config save callback ---- */

static void qq_on_config_saved(void)
{
    const claw_config_t *cfg = claw_config_get();

    rtos_mutex_take(s_token_mutex, 0xFFFFFFFFUL);
    bool creds_changed = (strcmp(cfg->qq.app_id,     s_app_id)     != 0 ||
                          strcmp(cfg->qq.app_secret, s_app_secret) != 0);
    if (creds_changed) {
        strlcpy(s_app_id,     cfg->qq.app_id,     sizeof(s_app_id));
        strlcpy(s_app_secret, cfg->qq.app_secret, sizeof(s_app_secret));
        s_token_expires_in_s = 0;
    }
    rtos_mutex_give(s_token_mutex);
    s_msg_type = cfg->qq.msg_type;

    if (creds_changed)
        s_ws_stop = true;
    qq_try_start();
}

static void qq_on_wifi_connected(void)
{
    s_wifi_ready = true;
    qq_try_start();
}

/* ---- Cap execute: qq_send_message ---- */

static int cap_qq_send_message_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char **output)
{
    (void)ctx;
    return claw_im_cap_execute_send_text(input_json, output, cap_im_qq_send);
}

/* ---- Cap group descriptors ---- */

static const claw_cap_descriptor_t s_qq_caps[] = {
    {
        .id          = "qq_send_message",
        .name        = "qq_send_message",
        .family      = "im_qq",
        .description = "Send a text message to a QQ chat.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
              "\"chat_id\":{\"type\":\"string\","
                "\"description\":\"QQ chat ID, format: c2c:{openid} or group:{openid}\"},"
              "\"text\":{\"type\":\"string\",\"description\":\"Message text\"}"
            "},"
            "\"required\":[\"chat_id\",\"text\"]}",
        .execute = cap_qq_send_message_execute,
    },
};

static const claw_cap_group_t s_qq_group = {
    .group_id         = "im_qq",
    .plugin_name      = "cap_im_qq",
    .version          = "1",
    .descriptors      = s_qq_caps,
    .descriptor_count = 1,
};

/* ---- Public API ---- */

void cap_im_qq_send(const char *chat_id, const char *text)
{
    if (!chat_id || !text) return;

    /* Parse chat_id: "type:openid" */
    const char *p1 = strchr(chat_id, ':');
    if (!p1) { RTK_LOGW(TAG, "send: invalid chat_id\n"); return; }
    p1++;

    char openid[64] = {0};
    strlcpy(openid, p1, sizeof(openid));

    char path[128];
    if (strncmp(chat_id, "c2c:", 4) == 0)
        DiagSnPrintf(path, sizeof(path), QQ_MSG_PATH_C2C_FMT, openid);
    else if (strncmp(chat_id, "group:", 6) == 0)
        DiagSnPrintf(path, sizeof(path), QQ_MSG_PATH_GROUP_FMT, openid);
    else {
        RTK_LOGW(TAG, "send: unknown chat_id type\n");
        return;
    }

    char *auth_hdr = malloc(QQ_AUTH_HDR_MAX);
    if (!auth_hdr) return;

    size_t offset = 0;
    while (text[offset]) {
        size_t chunk_bytes = qq_utf8_chunk_end(text, offset, QQ_MSG_CHUNK_MAX);
        if (chunk_bytes == 0) break;

        char *chunk_str = malloc(chunk_bytes + 1);
        if (!chunk_str) break;
        memcpy(chunk_str, text + offset, chunk_bytes);
        chunk_str[chunk_bytes] = '\0';

        bool sent = false;
        for (int attempt = 0; attempt < 2 && !sent; attempt++) {
            if (qq_fetch_token() != RTK_SUCCESS ||
                    qq_get_token_copy(auth_hdr, QQ_AUTH_HDR_MAX) != RTK_SUCCESS) {
                RTK_LOGE(TAG, "send: token unavailable\n");
                break;
            }

            /* Active (proactive) message: no msg_id / msg_seq */
            cJSON *body = cJSON_CreateObject();
            if (!body) break;
            cJSON_AddNumberToObject(body, "msg_type", s_msg_type);
            cJSON_AddStringToObject(body, "content",  chunk_str);
            char *body_str = cJSON_PrintUnformatted(body);
            cJSON_Delete(body);
            if (!body_str) break;

            llm_http_resp_t resp;
            if (llm_http_resp_init(&resp) != 0) { free(body_str); break; }
            int ret = llm_http_post_auth(QQ_API_HOST, path,
                                         body_str, strlen(body_str),
                                         auth_hdr, &resp);
            free(body_str);

            if (ret != 0) {
                RTK_LOGW(TAG, "send http failed (ret=%d)\n", ret);
                llm_http_resp_free(&resp);
                break;
            }

            int api_code = qq_parse_api_code(resp.buf);
            llm_http_resp_free(&resp);

            if (api_code == 0) {
                sent = true;
                RTK_LOGI(TAG, "sent to %s\n", chat_id);
            } else if (qq_is_token_error(api_code) && attempt == 0) {
                RTK_LOGW(TAG, "token error (%d), refreshing\n", api_code);
                rtos_mutex_take(s_token_mutex, 0xFFFFFFFFUL);
                s_token_expires_in_s = 0;
                rtos_mutex_give(s_token_mutex);
                /* attempt++ retries with fresh token */
            } else {
                break;
            }
        }

        free(chunk_str);
        if (!sent) break;
        offset += chunk_bytes;
    }

    free(auth_hdr);
}

int cap_im_qq_init(const cap_im_qq_config_t *cfg)
{
    if (!cfg) return RTK_ERR_BADARG;

    strlcpy(s_app_id,     cfg->app_id,     sizeof(s_app_id));
    strlcpy(s_app_secret, cfg->app_secret, sizeof(s_app_secret));
    s_msg_type = cfg->msg_type;

    if (rtos_mutex_create(&s_token_mutex) != RTK_SUCCESS ||
        rtos_mutex_create(&s_start_mutex) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "mutex create failed\n");
        return RTK_FAIL;
    }

    claw_im_dispatch_register_with_flags("qq", cap_im_qq_send, 0, "qq_send_message");
    claw_config_register_on_save(qq_on_config_saved);

    int err = claw_cap_register_group(&s_qq_group);
    if (err != RTK_SUCCESS) {
        RTK_LOGE(TAG, "claw_cap_register_group failed: %d\n", err);
        return err;
    }

    RTK_LOGI(TAG, "initialized\n");
    return RTK_SUCCESS;
}

int cap_im_qq_start(void)
{
    claw_wifi_mgr_register_on_connected(qq_on_wifi_connected);
    return RTK_SUCCESS;
}

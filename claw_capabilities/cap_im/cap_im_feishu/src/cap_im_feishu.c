/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Feishu IM via WebSocket Long Connection (device-initiated, works behind NAT).
 *
 * Flow:
 *   1. POST /callback/ws/endpoint → get WSS URL + ClientConfig
 *   2. Connect via wsclient (TLS)
 *   3. Poll loop: receive protobuf frames, send periodic pings
 *   4. On event frame: parse JSON, publish to claw_event_router, ACK
 *   5. On disconnect: reconnect after RECONNECT_DELAY_MS
 */
#include "cap_im_feishu.h"
#include "claw_cap.h"
#include "claw_cap_registry.h"
#include "claw_compat.h"
#include "claw_config.h"
#include "claw_wifi_mgr.h"
#include "claw_event_publisher.h"
#include "claw_im_dispatch.h"
#include "claw_ws_router.h"
#include "llm_agent_http.h"
#include "os_wrapper.h"
#include "websocket/wsclient_api.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "cap_im_feishu";

/* ---- Constants ---- */

#define FEISHU_RECONNECT_DELAY_MS   3000
#define FEISHU_WAIT_CONFIG_DELAY_MS 30000  /* poll interval when credentials not yet set */
#define FEISHU_CONNECT_TIMEOUT_MS   15000
#define FEISHU_WS_TX_BUF            2048
#define FEISHU_WS_RX_BUF            8192
#define FEISHU_WS_QUEUE_SIZE        4
#define FEISHU_WS_HOST_MAX          128
#define FEISHU_WS_PATH_MAX          512
#define FEISHU_DEDUP_CACHE_SIZE     32
#define FEISHU_PB_MAX_HEADERS       8

/* ---- Module state ---- */

static cap_im_feishu_config_t s_cfg;
static char                   s_token[512];        /* tenant_access_token */
static char                   s_token_app_id[64];  /* app_id used for current token */
static uint32_t               s_token_ts;          /* rtos_time_get_current_system_time_ms() when token was fetched */
static rtos_mutex_t           s_token_mutex;

/* WebSocket connection state */
static wsclient_context *s_ws_client;
static rtos_task_t       s_ws_task;
static volatile bool     s_ws_stop;       /* true = tear down the WS loop and exit the task */
static volatile bool     s_ws_reconnect;  /* true = drop the current connection and reconnect */
static volatile bool     s_ws_connected;
/* Credentials the live WS loop last connected with; used to detect changes. */
static char              s_conn_app_id[64];
static char              s_conn_app_secret[64];
static int               s_ws_service_id;
static int               s_ws_ping_interval_ms  = 120000;
static int               s_ws_reconnect_ms      = 30000;
static char              s_ws_host[FEISHU_WS_HOST_MAX];
static char              s_ws_path[FEISHU_WS_PATH_MAX];
static int               s_ws_port;

/* Dedup cache (accessed only from WS task).
 * Stores hash + id prefix to avoid false-positive suppression on hash collision. */
typedef struct { uint64_t hash; char id[20]; } feishu_dedup_entry_t;
static feishu_dedup_entry_t s_seen_entries[FEISHU_DEDUP_CACHE_SIZE];
static size_t               s_seen_idx;

/* ---- Protobuf types ---- */

typedef struct {
    char key[32];
    char value[128];
} feishu_hdr_t;

typedef struct {
    uint64_t      seq_id;
    uint64_t      log_id;
    int32_t       service;
    int32_t       method;
    feishu_hdr_t  headers[FEISHU_PB_MAX_HEADERS];
    size_t        header_count;
    const uint8_t *payload;
    size_t         payload_len;
} feishu_frame_t;

/* ---- Protobuf decoder ---- */

static bool pb_read_varint(const uint8_t *buf, size_t len, size_t *pos, uint64_t *out)
{
    uint64_t val   = 0;
    int      shift = 0;

    while (*pos < len && shift <= 63) {
        uint8_t b = buf[(*pos)++];
        val |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) { *out = val; return true; }
        shift += 7;
    }
    return false;
}

static bool pb_skip(const uint8_t *buf, size_t len, size_t *pos, uint8_t wire)
{
    uint64_t sz = 0;
    switch (wire) {
    case 0: return pb_read_varint(buf, len, pos, &sz);
    case 1: if (*pos + 8 > len) return false; *pos += 8; return true;
    case 2:
        if (!pb_read_varint(buf, len, pos, &sz) || *pos + (size_t)sz > len) return false;
        *pos += (size_t)sz; return true;
    case 5: if (*pos + 4 > len) return false; *pos += 4; return true;
    default: return false;
    }
}

static bool pb_parse_header(const uint8_t *buf, size_t len, feishu_hdr_t *hdr)
{
    size_t pos = 0;
    memset(hdr, 0, sizeof(*hdr));
    while (pos < len) {
        uint64_t tag = 0, sz = 0;
        if (!pb_read_varint(buf, len, &pos, &tag)) return false;
        uint8_t  wire  = (uint8_t)(tag & 7);
        uint32_t field = (uint32_t)(tag >> 3);
        if (wire != 2) { if (!pb_skip(buf, len, &pos, wire)) return false; continue; }
        if (!pb_read_varint(buf, len, &pos, &sz) || pos + (size_t)sz > len) return false;
        size_t cp = (size_t)sz;
        if (field == 1) {
            if (cp >= sizeof(hdr->key))   cp = sizeof(hdr->key)   - 1;
            memcpy(hdr->key,   buf + pos, cp); hdr->key[cp]   = '\0';
        } else if (field == 2) {
            if (cp >= sizeof(hdr->value)) cp = sizeof(hdr->value) - 1;
            memcpy(hdr->value, buf + pos, cp); hdr->value[cp] = '\0';
        }
        pos += (size_t)sz;
    }
    return true;
}

static bool pb_parse_frame(const uint8_t *buf, size_t len, feishu_frame_t *f)
{
    size_t pos = 0;
    memset(f, 0, sizeof(*f));
    while (pos < len) {
        uint64_t tag = 0, val = 0, sz = 0;
        if (!pb_read_varint(buf, len, &pos, &tag)) return false;
        uint8_t  wire  = (uint8_t)(tag & 7);
        uint32_t field = (uint32_t)(tag >> 3);

        if      (field == 1 && wire == 0) { if (!pb_read_varint(buf, len, &pos, &f->seq_id))  return false; }
        else if (field == 2 && wire == 0) { if (!pb_read_varint(buf, len, &pos, &f->log_id))  return false; }
        else if (field == 3 && wire == 0) {
            if (!pb_read_varint(buf, len, &pos, &val)) return false;
            f->service = (int32_t)val;
        }
        else if (field == 4 && wire == 0) {
            if (!pb_read_varint(buf, len, &pos, &val)) return false;
            f->method = (int32_t)val;
        }
        else if (field == 5 && wire == 2) {
            if (!pb_read_varint(buf, len, &pos, &sz) || pos + (size_t)sz > len) return false;
            if (f->header_count < FEISHU_PB_MAX_HEADERS)
                pb_parse_header(buf + pos, (size_t)sz, &f->headers[f->header_count++]);
            pos += (size_t)sz;
        }
        else if (field == 8 && wire == 2) {
            if (!pb_read_varint(buf, len, &pos, &sz) || pos + (size_t)sz > len) return false;
            f->payload     = buf + pos;
            f->payload_len = (size_t)sz;
            pos += (size_t)sz;
        }
        else { if (!pb_skip(buf, len, &pos, wire)) return false; }
    }
    return true;
}

static const char *pb_hdr_val(const feishu_frame_t *f, const char *key)
{
    for (size_t i = 0; i < f->header_count; i++)
        if (strcmp(f->headers[i].key, key) == 0) return f->headers[i].value;
    return NULL;
}

/* ---- Protobuf encoder ---- */

static bool pb_write_varint(uint8_t *buf, size_t cap, size_t *pos, uint64_t val)
{
    do {
        uint8_t b = (uint8_t)(val & 0x7f);
        if (*pos >= cap) return false;
        val >>= 7;
        if (val) b |= 0x80;
        buf[(*pos)++] = b;
    } while (val);
    return true;
}

static bool pb_write_bytes(uint8_t *buf, size_t cap, size_t *pos,
                           uint32_t field, const uint8_t *data, size_t len)
{
    if (!pb_write_varint(buf, cap, pos, ((uint64_t)field << 3) | 2) ||
        !pb_write_varint(buf, cap, pos, (uint64_t)len) ||
        *pos + len > cap) return false;
    memcpy(buf + *pos, data, len);
    *pos += len;
    return true;
}

static bool pb_write_str(uint8_t *buf, size_t cap, size_t *pos,
                         uint32_t field, const char *text)
{
    const char *s = text ? text : "";
    return pb_write_bytes(buf, cap, pos, field, (const uint8_t *)s, strlen(s));
}

static bool pb_write_hdr(uint8_t *buf, size_t cap, size_t *pos, const feishu_hdr_t *h)
{
    uint8_t tmp[256];
    size_t  tp = 0;
    if (!pb_write_str(tmp, sizeof(tmp), &tp, 1, h->key) ||
        !pb_write_str(tmp, sizeof(tmp), &tp, 2, h->value)) return false;
    return pb_write_bytes(buf, cap, pos, 5, tmp, tp);
}

static int feishu_ws_send_frame(const feishu_frame_t *f,
                                const uint8_t *payload, size_t payload_len)
{
    uint8_t buf[1024];
    size_t  pos = 0;

    if (!s_ws_client) return -1;

    if (!pb_write_varint(buf, sizeof(buf), &pos, (uint64_t)1 << 3) ||
        !pb_write_varint(buf, sizeof(buf), &pos, f->seq_id)        ||
        !pb_write_varint(buf, sizeof(buf), &pos, (uint64_t)2 << 3) ||
        !pb_write_varint(buf, sizeof(buf), &pos, f->log_id)        ||
        !pb_write_varint(buf, sizeof(buf), &pos, (uint64_t)3 << 3) ||
        !pb_write_varint(buf, sizeof(buf), &pos, (uint64_t)(uint32_t)f->service) ||
        !pb_write_varint(buf, sizeof(buf), &pos, (uint64_t)4 << 3) ||
        !pb_write_varint(buf, sizeof(buf), &pos, (uint64_t)(uint32_t)f->method))
        return -1;

    for (size_t i = 0; i < f->header_count; i++)
        if (!pb_write_hdr(buf, sizeof(buf), &pos, &f->headers[i])) return -1;

    if (payload && payload_len > 0)
        if (!pb_write_bytes(buf, sizeof(buf), &pos, 8, payload, payload_len)) return -1;

    return ws_sendBinary(buf, (int)pos, 1, s_ws_client);
}

/* ---- Token management ---- */

static int feishu_refresh_token(void)
{
    const claw_config_t *cfg     = claw_config_get();
    const char          *app_id  = cfg->feishu.app_id;
    const char          *app_sec = cfg->feishu.app_secret;

    if (app_id[0] == '\0' || app_sec[0] == '\0') {
        RTK_LOGD(TAG, "app_id or app_secret not configured\n");
        return RTK_FAIL;
    }

    cJSON *body_obj = cJSON_CreateObject();
    if (!body_obj) return RTK_ERR_NOMEM;
    cJSON_AddStringToObject(body_obj, "app_id",     app_id);
    cJSON_AddStringToObject(body_obj, "app_secret", app_sec);
    char *body_str = cJSON_PrintUnformatted(body_obj);
    cJSON_Delete(body_obj);
    if (!body_str) return RTK_ERR_NOMEM;

    llm_http_resp_t resp;
    if (llm_http_resp_init(&resp) != 0) { free(body_str); return RTK_FAIL; }

    int ret = llm_http_post_no_auth(
        "open.feishu.cn",
        "/open-apis/auth/v3/tenant_access_token/internal",
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

    cJSON *code_j  = cJSON_GetObjectItem(root, "code");
    cJSON *token_j = cJSON_GetObjectItem(root, "tenant_access_token");

    if (!cJSON_IsNumber(code_j) || (int)code_j->valuedouble != 0 ||
        !cJSON_IsString(token_j)) {
        RTK_LOGE(TAG, "token response error (code=%d)\n",
                 cJSON_IsNumber(code_j) ? (int)code_j->valuedouble : -1);
        cJSON_Delete(root);
        return RTK_FAIL;
    }

    /* Protect shared token state during write */
    rtos_mutex_take(s_token_mutex, 0xFFFFFFFFUL);
    strlcpy(s_token, token_j->valuestring, sizeof(s_token));
    strlcpy(s_token_app_id, claw_config_get()->feishu.app_id, sizeof(s_token_app_id));
    s_token_ts = rtos_time_get_current_system_time_ms();
    rtos_mutex_give(s_token_mutex);
    cJSON_Delete(root);

    RTK_LOGI(TAG, "token refreshed (%.16s...)\n", s_token);
    return RTK_SUCCESS;
}

static int feishu_get_token(char *buf, size_t buf_len)
{
    rtos_mutex_take(s_token_mutex, 0xFFFFFFFFUL);

    uint32_t now    = rtos_time_get_current_system_time_ms();
    uint32_t age_s  = (now - s_token_ts) / 1000;
    bool app_id_changed = (strcmp(s_token_app_id, claw_config_get()->feishu.app_id) != 0);
    bool needs_refresh  = (s_token[0] == '\0' ||
                           age_s >= s_cfg.token_refresh_interval_s ||
                           app_id_changed);

    if (!needs_refresh) {
        strlcpy(buf, s_token, buf_len);
        rtos_mutex_give(s_token_mutex);
        return RTK_SUCCESS;
    }

    /* Release mutex before blocking HTTP call to avoid priority inversion.
     * feishu_refresh_token() re-acquires the mutex internally for the write. */
    rtos_mutex_give(s_token_mutex);

    int err = feishu_refresh_token();
    if (err != RTK_SUCCESS) return err;

    rtos_mutex_take(s_token_mutex, 0xFFFFFFFFUL);
    strlcpy(buf, s_token, buf_len);
    rtos_mutex_give(s_token_mutex);
    return RTK_SUCCESS;
}

/* ---- Outbound send ---- */

void cap_im_feishu_send(const char *chat_id, const char *text)
{
    if (!chat_id || !text) return;

    /* Heap-allocate the 512-byte token buffer to keep feishu_ws_task
     * stack usage lower (was 512B of stack per send call). */
    char *token = malloc(512);
    if (!token) return;
    if (feishu_get_token(token, 512) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "send: no valid token\n");
        free(token);
        return;
    }

    /* Build inner content string */
    cJSON *inner = cJSON_CreateObject();
    if (!inner) { free(token); return; }
    cJSON_AddStringToObject(inner, "text", text);
    char *inner_str = cJSON_PrintUnformatted(inner);
    cJSON_Delete(inner);
    if (!inner_str) { free(token); return; }

    /* Build outer body */
    cJSON *outer = cJSON_CreateObject();
    if (!outer) { free(inner_str); free(token); return; }
    cJSON_AddStringToObject(outer, "receive_id", chat_id);
    cJSON_AddStringToObject(outer, "msg_type",   "text");
    cJSON_AddStringToObject(outer, "content",    inner_str);
    free(inner_str);
    char *body_str = cJSON_PrintUnformatted(outer);
    cJSON_Delete(outer);
    if (!body_str) { free(token); return; }

    /* receive_id_type: open_id for ou_ (personal), chat_id for oc_ / others */
    const char *id_type = (strncmp(chat_id, "ou_", 3) == 0) ? "open_id" : "chat_id";
    char resource[80];
    DiagSnPrintf(resource, sizeof(resource),
             "/open-apis/im/v1/messages?receive_id_type=%s", id_type);

    llm_http_resp_t resp;
    if (llm_http_resp_init(&resp) != 0) { free(body_str); free(token); return; }

    int ret = llm_http_post_bearer(
        "open.feishu.cn", resource,
        body_str, strlen(body_str), token, &resp);

    if (ret != 0)
        RTK_LOGE(TAG, "send failed (ret=%d)\n", ret);
    else
        RTK_LOGI(TAG, "sent to %s\n", chat_id);

    llm_http_resp_free(&resp);
    free(body_str);
    free(token);
}

/* ---- Cap execute: feishu_send_message ---- */

static int cap_feishu_send_message_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char **output)
{
    (void)ctx;
    return claw_im_cap_execute_send_text(input_json, output, cap_im_feishu_send);
}

/* ---- Dedup ---- */

static uint64_t fnv1a64(const char *s)
{
    uint64_t h = 14695981039346656037ULL;
    while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ULL; }
    return h;
}

static bool feishu_dedup(const char *msg_id)
{
    uint64_t key = fnv1a64(msg_id);
    for (size_t i = 0; i < FEISHU_DEDUP_CACHE_SIZE; i++) {
        /* The stored id holds only the first sizeof(id)-1 chars of msg_id
         * (Feishu message_ids are ~35 chars, longer than the buffer), so this
         * tie-breaker must compare over the stored prefix length — a full
         * strcmp() never matches the truncated copy and defeats dedup entirely. */
        if (s_seen_entries[i].hash == key &&
            strncmp(s_seen_entries[i].id, msg_id, sizeof(s_seen_entries[i].id) - 1) == 0)
            return true;
    }
    s_seen_entries[s_seen_idx].hash = key;
    strncpy(s_seen_entries[s_seen_idx].id, msg_id, sizeof(s_seen_entries[s_seen_idx].id) - 1);
    s_seen_entries[s_seen_idx].id[sizeof(s_seen_entries[s_seen_idx].id) - 1] = '\0';
    s_seen_idx = (s_seen_idx + 1) % FEISHU_DEDUP_CACHE_SIZE;
    return false;
}

/* ---- Event processing ---- */

static void feishu_handle_message(cJSON *event)
{
    cJSON *message     = cJSON_GetObjectItem(event, "message");
    if (!cJSON_IsObject(message)) return;

    cJSON *sender      = cJSON_GetObjectItem(event, "sender");
    cJSON *sid_obj     = cJSON_IsObject(sender)   ? cJSON_GetObjectItem(sender,  "sender_id") : NULL;
    cJSON *open_id_j   = cJSON_IsObject(sid_obj)  ? cJSON_GetObjectItem(sid_obj, "open_id")   : NULL;
    cJSON *msg_id_j    = cJSON_GetObjectItem(message, "message_id");
    cJSON *chat_id_j   = cJSON_GetObjectItem(message, "chat_id");
    cJSON *chat_type_j = cJSON_GetObjectItem(message, "chat_type");
    cJSON *msg_type_j  = cJSON_GetObjectItem(message, "message_type");
    cJSON *content_j   = cJSON_GetObjectItem(message, "content");

    if (!cJSON_IsString(chat_id_j) || !cJSON_IsString(content_j)) return;

    const char *msg_id    = cJSON_IsString(msg_id_j)    ? msg_id_j->valuestring    : "";
    const char *chat_id   = chat_id_j->valuestring;
    const char *chat_type = cJSON_IsString(chat_type_j) ? chat_type_j->valuestring : "p2p";
    const char *msg_type  = cJSON_IsString(msg_type_j)  ? msg_type_j->valuestring  : "text";
    const char *sender_id = cJSON_IsString(open_id_j)   ? open_id_j->valuestring   : "";

    if (msg_id[0] && feishu_dedup(msg_id)) {
        RTK_LOGD(TAG, "skip dup %s\n", msg_id);
        return;
    }

    /* P2P: route replies to the sender's open_id so responses reach them;
     * group chats: route to chat_id so the whole group sees the reply. */
    const char *route_id = (strcmp(chat_type, "p2p") == 0 && sender_id[0])
                           ? sender_id : chat_id;

    if (strcmp(msg_type, "text") != 0) {
        RTK_LOGI(TAG, "ignore msg_type=%s msg=%s\n", msg_type, msg_id);
        return;
    }

    /* content is a JSON string; extract the "text" field inside it */
    cJSON *inner = cJSON_Parse(content_j->valuestring);
    if (!inner) return;

    cJSON *text_j = cJSON_GetObjectItem(inner, "text");
    if (cJSON_IsString(text_j) && text_j->valuestring[0]) {
        const char *text = text_j->valuestring;
        /* Strip bot @mention prefix that Feishu prepends in group chats */
        if (strncmp(text, "@_user_1 ", 9) == 0) text += 9;
        while (*text == ' ' || *text == '\n') text++;
        if (text[0]) {
            RTK_LOGI(TAG, "recv [%s] from %s: %.60s\n", chat_id, sender_id, text);
            claw_event_dispatcher_publish_message(
                "cap_im_feishu", "feishu",
                route_id, text, sender_id, msg_id);
        }
    }
    cJSON_Delete(inner);
}

static void feishu_process_event_json(const uint8_t *data, size_t len)
{
    cJSON *root = cJSON_ParseWithLength((const char *)data, len);
    if (!root) { RTK_LOGW(TAG, "WS event JSON parse failed\n"); return; }

    cJSON *event  = cJSON_GetObjectItem(root, "event");
    cJSON *header = cJSON_GetObjectItem(root, "header");
    cJSON *etype  = cJSON_IsObject(header) ? cJSON_GetObjectItem(header, "event_type") : NULL;

    if (cJSON_IsObject(event) &&
        (!etype || (cJSON_IsString(etype) &&
                    strcmp(etype->valuestring, "im.message.receive_v1") == 0))) {
        feishu_handle_message(event);
    }

    cJSON_Delete(root);
}

static void feishu_handle_frame(const uint8_t *buf, int len)
{
    feishu_frame_t frame;
    if (!pb_parse_frame(buf, (size_t)len, &frame)) {
        RTK_LOGW(TAG, "PB frame parse failed\n");
        return;
    }

    const char *type = pb_hdr_val(&frame, "type");

    /* method==0: control frame (pong / handshake ack) */
    if (frame.method == 0) {
        if (type && strcmp(type, "pong") == 0 &&
            frame.payload && frame.payload_len > 0) {
            cJSON *cfg = cJSON_ParseWithLength((const char *)frame.payload,
                                               frame.payload_len);
            if (cfg) {
                cJSON *ping_j = cJSON_GetObjectItem(cfg, "PingInterval");
                if (cJSON_IsNumber(ping_j))
                    s_ws_ping_interval_ms = ping_j->valueint * 1000;
                cJSON_Delete(cfg);
            }
        }
        return;
    }

    /* method!=0: only process frames with type=="event" */
    if (!type || strcmp(type, "event") != 0 ||
        !frame.payload || frame.payload_len == 0) return;

    feishu_process_event_json(frame.payload, frame.payload_len);

    /* ACK: echo back the frame header with {"code":200} payload */
    static const char ack[] = "{\"code\":200}";
    feishu_ws_send_frame(&frame, (const uint8_t *)ack, strlen(ack));
}

/* ---- WS dispatch callback (called from ws_poll context) ---- */

static void feishu_ws_on_data(wsclient_context **ctx, int len, enum opcode_type op)
{
    if (op != BINARY_FRAME || len <= 0 || !(*ctx)->receivedData) return;
    feishu_handle_frame((*ctx)->receivedData, len);
}

/* ---- WS config pull ---- */

static bool feishu_parse_ws_url(const char *url)
{
    const char *p = url;
    int port = 443;

    if (strncmp(p, "wss://", 6) == 0) { port = 443; p += 6; }
    else if (strncmp(p, "ws://", 5) == 0) { port = 80;  p += 5; }
    else return false;

    const char *slash = strchr(p, '/');
    const char *colon = slash ? memchr(p, ':', (size_t)(slash - p))
                               : strchr(p, ':');

    size_t hlen;
    if (colon && (!slash || colon < slash)) {
        hlen = (size_t)(colon - p);
        port = atoi(colon + 1);
    } else {
        hlen = slash ? (size_t)(slash - p) : strlen(p);
    }

    if (hlen == 0 || hlen >= sizeof(s_ws_host)) return false;
    memcpy(s_ws_host, p, hlen);
    s_ws_host[hlen] = '\0';

    const char *path = slash ? slash : "/";
    if (strlen(path) >= sizeof(s_ws_path)) return false;
    strlcpy(s_ws_path, path, sizeof(s_ws_path));
    s_ws_port = port;
    return true;
}

static int feishu_pull_ws_config(void)
{
    const claw_config_t *cfg = claw_config_get();

    if (cfg->feishu.app_id[0] == '\0' || cfg->feishu.app_secret[0] == '\0') {
        RTK_LOGW(TAG, "credentials not configured\n");
        return RTK_FAIL;
    }

    /* Snapshot the credentials this connection attempt uses, so a later
     * config save can detect a change and trigger a reconnect. */
    strlcpy(s_conn_app_id,     cfg->feishu.app_id,     sizeof(s_conn_app_id));
    strlcpy(s_conn_app_secret, cfg->feishu.app_secret, sizeof(s_conn_app_secret));

    cJSON *body = cJSON_CreateObject();
    if (!body) return RTK_ERR_NOMEM;
    /* Feishu WS endpoint uses capitalized AppID/AppSecret */
    cJSON_AddStringToObject(body, "AppID",     cfg->feishu.app_id);
    cJSON_AddStringToObject(body, "AppSecret", cfg->feishu.app_secret);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) return RTK_ERR_NOMEM;

    llm_http_resp_t resp;
    if (llm_http_resp_init(&resp) != 0) { free(body_str); return RTK_FAIL; }

    int ret = llm_http_post_no_auth(
        "open.feishu.cn", "/callback/ws/endpoint",
        body_str, strlen(body_str), &resp);
    free(body_str);

    if (ret != 0 || resp.len == 0) {
        RTK_LOGE(TAG, "WS config request failed (ret=%d)\n", ret);
        llm_http_resp_free(&resp);
        return RTK_FAIL;
    }

    cJSON *root = cJSON_Parse(resp.buf);
    llm_http_resp_free(&resp);
    if (!root) { RTK_LOGE(TAG, "WS config parse failed\n"); return RTK_FAIL; }

    cJSON *code_j = cJSON_GetObjectItem(root, "code");
    cJSON *data_j = cJSON_GetObjectItem(root, "data");
    cJSON *url_j  = cJSON_IsObject(data_j) ? cJSON_GetObjectItem(data_j, "URL")          : NULL;
    cJSON *ccfg_j = cJSON_IsObject(data_j) ? cJSON_GetObjectItem(data_j, "ClientConfig") : NULL;

    if (!cJSON_IsNumber(code_j) || (int)code_j->valuedouble != 0 ||
        !cJSON_IsString(url_j)) {
        RTK_LOGE(TAG, "WS config invalid (code=%d)\n",
                 cJSON_IsNumber(code_j) ? (int)code_j->valuedouble : -1);
        cJSON_Delete(root);
        return RTK_FAIL;
    }

    if (!feishu_parse_ws_url(url_j->valuestring)) {
        RTK_LOGE(TAG, "WS URL parse failed: %.80s\n", url_j->valuestring);
        cJSON_Delete(root);
        return RTK_FAIL;
    }

    /* Extract service_id from path query string */
    char *sid = strstr(s_ws_path, "service_id=");
    s_ws_service_id = sid ? atoi(sid + strlen("service_id=")) : 0;

    if (cJSON_IsObject(ccfg_j)) {
        cJSON *ping_j  = cJSON_GetObjectItem(ccfg_j, "PingInterval");
        cJSON *recon_j = cJSON_GetObjectItem(ccfg_j, "ReconnectInterval");
        if (cJSON_IsNumber(ping_j))  s_ws_ping_interval_ms = ping_j->valueint  * 1000;
        if (cJSON_IsNumber(recon_j)) s_ws_reconnect_ms     = recon_j->valueint * 1000;
    }

    cJSON_Delete(root);
    RTK_LOGI(TAG, "WS config: %s:%d service_id=%d ping=%ds\n",
             s_ws_host, s_ws_port, s_ws_service_id, s_ws_ping_interval_ms / 1000);
    return RTK_SUCCESS;
}

/* ---- WS task ---- */

static void feishu_ws_task(void *arg)
{
    (void)arg;

    while (!s_ws_stop) {
        s_ws_reconnect = false;
        if (feishu_pull_ws_config() != RTK_SUCCESS) {
            rtos_time_delay_ms(s_ws_stop ? 0 : FEISHU_RECONNECT_DELAY_MS);
            continue;
        }

        char ws_url[FEISHU_WS_HOST_MAX + 8];
        snprintf(ws_url, sizeof(ws_url), "%s%s",
                 s_ws_port == 443 ? "wss://" : "ws://", s_ws_host);
        /* libwsclient prepends '/' in "GET /%s", so strip leading '/' from path */
        char *ws_path = (s_ws_path[0] == '/') ? s_ws_path + 1 : s_ws_path;
        wsclient_context *wsc = create_wsclient(
            ws_url, s_ws_port, ws_path, NULL,
            FEISHU_WS_TX_BUF, FEISHU_WS_RX_BUF, FEISHU_WS_QUEUE_SIZE);
        if (!wsc) {
            RTK_LOGE(TAG, "create_wsclient failed\n");
            rtos_time_delay_ms(s_ws_stop ? 0 : FEISHU_RECONNECT_DELAY_MS);
            continue;
        }

        wsclient_set_fun_ops(wsc);
        ws_setsockopt_timeout(10000, 10000, FEISHU_CONNECT_TIMEOUT_MS);
        ws_setsockopt_keepalive(30, 5, 3);

        RTK_LOGI(TAG, "connecting to %s:%d ...\n", s_ws_host, s_ws_port);
        if (ws_connect_url(wsc) < 0) {
            RTK_LOGE(TAG, "ws_connect_url failed\n");
            ws_close(&wsc);
            rtos_time_delay_ms(s_ws_stop ? 0 : FEISHU_RECONNECT_DELAY_MS);
            continue;
        }

        s_ws_client    = wsc;
        s_ws_connected = true;
        /* Route this connection's inbound frames to feishu_ws_on_data. Must
         * happen before the ws_poll() loop and be paired with an unregister
         * below before the context is torn down. */
        claw_ws_router_register(wsc, feishu_ws_on_data, NULL, NULL);
        RTK_LOGI(TAG, "WS connected (service_id=%d)\n", s_ws_service_id);

        uint32_t last_ping_ms = rtos_time_get_current_system_time_ms();

        while (!s_ws_stop && !s_ws_reconnect && wsc != NULL &&
               ws_getReadyState(wsc) != WSC_CLOSED &&
               ws_getReadyState(wsc) != WSC_CLOSING) {

            ws_poll(200, &wsc);
            if (!wsc) break;

            uint32_t now_ms = rtos_time_get_current_system_time_ms();
            if ((int)(now_ms - last_ping_ms) >= s_ws_ping_interval_ms) {
                feishu_frame_t ping = {0};
                ping.service           = s_ws_service_id;
                ping.header_count      = 1;
                strlcpy(ping.headers[0].key,   "type", sizeof(ping.headers[0].key));
                strlcpy(ping.headers[0].value, "ping", sizeof(ping.headers[0].value));
                feishu_ws_send_frame(&ping, NULL, 0);
                last_ping_ms = now_ms;
                RTK_LOGD(TAG, "ping sent\n");
            }
        }

        s_ws_connected = false;
        s_ws_client    = NULL;
        /* Stop routing before the context is torn down / freed. */
        claw_ws_router_unregister(wsc);
        if (wsc) ws_close(&wsc);
        RTK_LOGW(TAG, "WS disconnected\n");

        /* Skip the backoff when a reconnect was explicitly requested
         * (e.g. credentials changed) so the new config takes effect at once. */
        if (!s_ws_stop && !s_ws_reconnect)
            rtos_time_delay_ms(FEISHU_RECONNECT_DELAY_MS);
    }

    s_ws_task = NULL;
    rtos_task_delete(NULL);
}

/* ---- Cap group descriptors ---- */

static const claw_cap_descriptor_t s_feishu_caps[] = {
    {
        .id          = "feishu_send_message",
        .name        = "feishu_send_message",
        .family      = "im_feishu",
        .description = "Send a text message to a Feishu chat",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"chat_id\":{\"type\":\"string\",\"description\":\"Feishu chat ID (oc_xxx or ou_xxx)\"},"
            "\"text\":{\"type\":\"string\",\"description\":\"Message text to send\"}"
            "},"
            "\"required\":[\"chat_id\",\"text\"]}",
        .execute = cap_feishu_send_message_execute,
    },
};

static const claw_cap_group_t s_feishu_group = {
    .group_id         = "im_feishu",
    .plugin_name      = "cap_im_feishu",
    .version          = "1",
    .descriptors      = s_feishu_caps,
    .descriptor_count = 1,
};

/* ---- Public API ---- */

static void feishu_on_config_saved(void);   /* forward declarations */
static void feishu_on_wifi_connected(void);
static void feishu_try_start(void);

int cap_im_feishu_init(const cap_im_feishu_config_t *cfg)
{
    if (!cfg) return RTK_ERR_BADARG;

    _memset(&s_cfg,        0, sizeof(s_cfg));
    _memset(s_token,       0, sizeof(s_token));
    _memset(s_token_app_id, 0, sizeof(s_token_app_id));
    s_token_ts = 0;
    s_ws_stop      = false;
    s_ws_reconnect = false;
    _memset(s_conn_app_id,     0, sizeof(s_conn_app_id));
    _memset(s_conn_app_secret, 0, sizeof(s_conn_app_secret));

    s_cfg = *cfg;

    int err_mutex = rtos_mutex_create(&s_token_mutex);
    if (err_mutex != RTK_SUCCESS) {
        RTK_LOGE(TAG, "mutex create failed\n");
        return RTK_ERR_NOMEM;
    }

    claw_im_dispatch_register_with_flags("feishu", cap_im_feishu_send, 0, "feishu_send_message");
    claw_config_register_on_save(feishu_on_config_saved);

    int err = claw_cap_register_group(&s_feishu_group);
    if (err != RTK_SUCCESS) {
        RTK_LOGE(TAG, "claw_cap_register_group failed: %d\n", err);
        return err;
    }

    RTK_LOGI(TAG, "initialized (WS long connection mode)\n");
    return RTK_SUCCESS;
}

/* Internal: start WS task only when BOTH credentials AND WiFi are ready. */
static void feishu_try_start(void)
{
    if (s_ws_task) return;  /* already running */

    const claw_config_t *cfg = claw_config_get();
    if (cfg->feishu.app_id[0] == '\0' || cfg->feishu.app_secret[0] == '\0') return;
    if (claw_wifi_mgr_get_state() != CLAW_WIFI_STATE_CONNECTED) return;

    s_ws_stop      = false;
    s_ws_reconnect = false;
    if (rtos_task_create(&s_ws_task, "feishu_ws", feishu_ws_task,
                         NULL, 12 * 1024, 2) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "WS task create failed\n");
        return;
    }
    RTK_LOGI(TAG, "WS task started\n");
}

int cap_im_feishu_start(void)
{
    /* Register callbacks — task starts when both conditions are met */
    claw_wifi_mgr_register_on_connected(feishu_on_wifi_connected);
    return RTK_SUCCESS;
}

static void feishu_on_config_saved(void)
{
    /* Not running yet: start if credentials are now present (first-time entry). */
    if (!s_ws_task) {
        feishu_try_start();
        return;
    }

    const claw_config_t *cfg = claw_config_get();

    /* Credentials cleared → stop the WS loop cleanly (hot-disable). */
    if (cfg->feishu.app_id[0] == '\0' || cfg->feishu.app_secret[0] == '\0') {
        s_ws_stop = true;
        return;
    }

    /* Credentials changed → invalidate the token and force the persistent loop
     * to reconnect with the new ones. Unrelated config saves are ignored. */
    if (strcmp(cfg->feishu.app_id,     s_conn_app_id)     != 0 ||
        strcmp(cfg->feishu.app_secret, s_conn_app_secret) != 0) {
        rtos_mutex_take(s_token_mutex, 0xFFFFFFFFUL);
        s_token[0] = '\0';
        s_token_ts = 0;
        rtos_mutex_give(s_token_mutex);
        s_ws_reconnect = true;
    }
}

static void feishu_on_wifi_connected(void)
{
    feishu_try_start();
}

/* ---- Lifecycle registration (claw_cap_registry): IO phase ----
 * WS long-connection (no HTTP route); registers the "feishu" channel + its own
 * wifi hook from start(). Mirrors the former ameba_claw_main.c wiring. */
static void im_feishu_on_io(const claw_config_t *cfg)
{
    cap_im_feishu_config_t c = CAP_IM_FEISHU_DEFAULT_CONFIG();
    if (cfg->feishu.app_id[0])     strlcpy(c.app_id,     cfg->feishu.app_id,     sizeof(c.app_id));
    if (cfg->feishu.app_secret[0]) strlcpy(c.app_secret, cfg->feishu.app_secret, sizeof(c.app_secret));
    cap_im_feishu_init(&c);
    cap_im_feishu_start();
}
CLAW_CAP_REGISTER(im_feishu, {
    .group = "im_feishu",
    .order = 140,
    .on_io = im_feishu_on_io,
});

/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_im_local.h"
#include "ameba_claw_defs.h"
#include "claw_http_server.h"
#include "claw_event_publisher.h"
#include "claw_im_dispatch.h"
#include "claw_cap.h"
#include "claw_utf8.h"
#include <cJSON.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "cap_im_local";

/* ---- Message ring buffer ---- */

#define MSG_BUF_MAX  CLAW_IM_LOCAL_MSG_BUF_MAX
#define MSG_TEXT_MAX CLAW_IM_LOCAL_MSG_TEXT_MAX

typedef struct {
    uint64_t id;
    char     role[12];    /* "user" or "assistant" */
    char     chat_id[64];
    char    *text;        /* heap-allocated full message, NULL when empty */
} local_msg_t;

static local_msg_t s_msgs[MSG_BUF_MAX];
static int         s_msg_head  = 0;   /* next write position */
static int         s_msg_count = 0;   /* total messages stored (up to MSG_BUF_MAX) */
static uint64_t    s_next_id   = 1;

/* os_wrapper mutex for ring buffer */
static rtos_mutex_t s_mutex = NULL;

static uint64_t msg_push(const char *role, const char *chat_id, const char *text)
{
    if (!s_mutex) return 0;
    rtos_mutex_take(s_mutex, 0xFFFFFFFFUL);

    local_msg_t *m = &s_msgs[s_msg_head];
    uint64_t id = s_next_id;
    m->id = s_next_id++;
    strlcpy(m->role,    role    ? role    : "user",  sizeof(m->role));
    strlcpy(m->chat_id, chat_id ? chat_id : "",      sizeof(m->chat_id));

    /* Free the previous occupant of this ring slot, then allocate exactly
     * what this message needs (capped at MSG_TEXT_MAX). */
    if (m->text) { free(m->text); m->text = NULL; }
    const char *src = text ? text : "";
    size_t want = strlen(src) + 1;
    size_t cap  = want > MSG_TEXT_MAX ? MSG_TEXT_MAX : want;
    m->text = malloc(cap);
    if (m->text) {
        claw_utf8_truncate_copy(m->text, cap, src);
    }

    s_msg_head = (s_msg_head + 1) % MSG_BUF_MAX;
    if (s_msg_count < MSG_BUF_MAX) s_msg_count++;

    rtos_mutex_give(s_mutex);
    return id;
}

/* Serialize all ring-buffer messages with id > since into a JSON array.
 * Caller must free() the returned string. Shared by /updates and the
 * WebSocket history snapshot. Returns NULL on failure. */
static char *build_msgs_json(uint64_t since)
{
    if (!s_mutex) return NULL;
    rtos_mutex_take(s_mutex, 0xFFFFFFFFUL);

    cJSON *arr = cJSON_CreateArray();
    if (arr) {
        int start = s_msg_count < MSG_BUF_MAX ? 0 : s_msg_head;
        for (int n = 0; n < s_msg_count; n++) {
            int idx = (start + n) % MSG_BUF_MAX;
            if (s_msgs[idx].id > since) {
                cJSON *obj = cJSON_CreateObject();
                if (obj) {
                    cJSON_AddNumberToObject(obj, "id",      (double)s_msgs[idx].id);
                    cJSON_AddStringToObject(obj, "role",    s_msgs[idx].role);
                    cJSON_AddStringToObject(obj, "chat_id", s_msgs[idx].chat_id);
                    cJSON_AddStringToObject(obj, "text",    s_msgs[idx].text ? s_msgs[idx].text : "");
                    cJSON_AddItemToArray(arr, obj);
                }
            }
        }
    }

    rtos_mutex_give(s_mutex);

    char *json_str = arr ? cJSON_PrintUnformatted(arr) : NULL;
    cJSON_Delete(arr);
    return json_str;
}

/* ---- URL decode helper ---- */

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode_n(const char *src, size_t src_max,
                          char *dst, size_t dst_size)
{
    size_t i = 0, s = 0;
    while (s < src_max && src[s] && i + 1 < dst_size) {
        if (src[s] == '%' && s + 2 < src_max && src[s+1] && src[s+2]) {
            int hi = hex_digit(src[s + 1]);
            int lo = hex_digit(src[s + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[i++] = (char)(hi << 4 | lo);
                s += 3;
                continue;
            }
        }
        dst[i++] = (src[s] == '+') ? ' ' : src[s];
        s++;
    }
    dst[i] = '\0';
}

/* Parse "key=value&key2=value2" form body. Returns pointer to value or NULL. */
static void parse_form_field(const char *body, const char *key,
                              char *out, size_t out_size)
{
    size_t klen = strlen(key);
    const char *p = body;
    out[0] = '\0';

    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *val_start = p + klen + 1;
            const char *amp = strchr(val_start, '&');
            size_t vlen = amp ? (size_t)(amp - val_start) : strlen(val_start);
            url_decode_n(val_start, vlen, out, out_size);
            return;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
}

/* ---- HTTP handlers ---- */

static void handle_send(const claw_http_request_t *req,
                         claw_http_send_fn_t send_fn, int sock)
{
    char chat_id[64] = {0};

    /* Allocate text buffer from heap based on body length so that URL-decoded
     * content never gets truncated mid-UTF-8-sequence.  The decoded length is
     * always <= the encoded length, so body_len+1 bytes is always sufficient. */
    size_t body_len = req->body ? strlen(req->body) : 0;
    size_t text_cap = body_len + 1;
    char *text = malloc(text_cap);
    if (!text) {
        send_fn(sock, 500, "text/plain", "oom", 3);
        return;
    }
    text[0] = '\0';

    parse_form_field(req->body, "chat_id", chat_id, sizeof(chat_id));
    parse_form_field(req->body, "text",    text,    text_cap);

    if (text[0] == '\0') {
        free(text);
        send_fn(sock, 400, "text/plain", "missing text", 12);
        return;
    }

    if (chat_id[0] == '\0') {
        strlcpy(chat_id, "local", sizeof(chat_id));
    }

    RTK_LOGI(TAG, "recv [%s]: %.60s\n", chat_id, text);

    /* Store user message locally */
    msg_push("user", chat_id, text);

    /* Publish to event router → LLM agent */
    claw_event_dispatcher_publish_message("cap_im_local", "local",
                                      chat_id, text, NULL, NULL);

    free(text);
    send_fn(sock, 200, "text/plain", "ok", 2);
}

static void handle_updates(const claw_http_request_t *req,
                            claw_http_send_fn_t send_fn, int sock)
{
    /* Parse ?since=N from query string */
    uint64_t since = 0;
    char since_str[32] = {0};
    parse_form_field(req->query, "since", since_str, sizeof(since_str));
    if (since_str[0]) {
        since = (uint64_t)strtoull(since_str, NULL, 10);
    }

    char *json_str = build_msgs_json(since);
    if (json_str) {
        send_fn(sock, 200, "application/json", json_str, strlen(json_str));
        free(json_str);
    } else {
        send_fn(sock, 200, "application/json", "[]", 2);
    }
}

/* ---- WebSocket handlers (port 80, /ws/chat) ---- */

/* On open: push the full message history as a JSON array snapshot so the
 * client can render past conversation, then receive live pushes. */
static void ws_on_open(claw_ws_conn_t *c)
{
    char *j = build_msgs_json(0);
    if (j) {
        claw_ws_send_text(c, j, strlen(j));
        free(j);
    }
}

/* On message: client sent a user message. Accept either a JSON object
 * {text, chat_id} or a raw text payload. */
static void ws_on_message(claw_ws_conn_t *c, const char *data, size_t len, int is_text)
{
    (void)c;
    if (!is_text || !data || len == 0) return;

    char chat_id[64] = {0};
    char *text = malloc(CLAW_IM_LOCAL_WS_TEXT_MAX);
    if (!text) return;
    text[0] = '\0';

    cJSON *root = cJSON_ParseWithLength(data, len);
    if (root) {
        cJSON *jt = cJSON_GetObjectItem(root, "text");
        cJSON *jc = cJSON_GetObjectItem(root, "chat_id");
        if (cJSON_IsString(jt) && jt->valuestring) {
            claw_utf8_truncate_copy(text, CLAW_IM_LOCAL_WS_TEXT_MAX,
                                    jt->valuestring);
        }
        if (cJSON_IsString(jc) && jc->valuestring) {
            strlcpy(chat_id, jc->valuestring, sizeof(chat_id));
        }
        cJSON_Delete(root);
    } else {
        /* Raw text fallback: cap at WS_TEXT_MAX with UTF-8-safe boundary */
        size_t n = len < CLAW_IM_LOCAL_WS_TEXT_MAX - 1
                   ? len : CLAW_IM_LOCAL_WS_TEXT_MAX - 1;
        n = claw_utf8_safe_len(data, n);
        memcpy(text, data, n);
        text[n] = '\0';
    }

    if (text[0] == '\0') { free(text); return; }
    if (chat_id[0] == '\0') strlcpy(chat_id, "local", sizeof(chat_id));

    RTK_LOGI(TAG, "ws recv [%s]: %.60s\n", chat_id, text);

    msg_push("user", chat_id, text);
    claw_event_dispatcher_publish_message("cap_im_local", "local",
                                      chat_id, text, NULL, NULL);
    free(text);
}

/* ---- Public API ---- */

void cap_im_local_send(const char *chat_id, const char *text)
{
    if (!text) return;
    RTK_LOGI(TAG, "assistant→[%s]: %.60s\n", chat_id ? chat_id : "?", text);

    uint64_t id = msg_push("assistant", chat_id, text);

    /* Push to any connected WebSocket clients */
    cJSON *obj = cJSON_CreateObject();
    if (obj) {
        cJSON_AddNumberToObject(obj, "id",      (double)id);
        cJSON_AddStringToObject(obj, "role",    "assistant");
        cJSON_AddStringToObject(obj, "chat_id", chat_id ? chat_id : "local");
        cJSON_AddStringToObject(obj, "text",    text);
        char *j = cJSON_PrintUnformatted(obj);
        cJSON_Delete(obj);
        if (j) {
            claw_ws_broadcast_text("/ws/chat", j, strlen(j));
            free(j);
        }
    }
}

/* ---- Cap group: local_send_text ---- */

static int local_send_text_execute(const char *input_json,
                                    const claw_cap_call_context_t *ctx,
                                    char **output)
{
    (void)ctx;
    return claw_im_cap_execute_send_text(input_json, output, cap_im_local_send);
}

static const claw_cap_descriptor_t s_local_caps[] = {
    {
        .id          = "local_send_text",
        .name        = "local_send_text",
        .family      = "im_local",
        .description = "Send a text message to the local WebUI chat.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"chat_id\":{\"type\":\"string\",\"description\":\"Local session chat ID\"},"
            "\"text\":{\"type\":\"string\",\"description\":\"Message text to send\"}"
            "},"
            "\"required\":[\"chat_id\",\"text\"]}",
        .execute     = local_send_text_execute,
    },
};

static const claw_cap_group_t s_local_group = {
    .group_id         = "im_local",
    .plugin_name      = "cap_im_local",
    .version          = "1",
    .descriptors      = s_local_caps,
    .descriptor_count = 1,
};

int cap_im_local_init(const cap_im_local_config_t *cfg)
{
    (void)cfg;

    int err = rtos_mutex_create(&s_mutex);
    if (err != RTK_SUCCESS) {
        RTK_LOGE(TAG, "mutex create failed\n");
        return RTK_ERR_NOMEM;
    }

    /* Register HTTP routes */
    claw_http_server_add_route(HTTP_POST, "/send",    handle_send);
    claw_http_server_add_route(HTTP_GET,  "/updates", handle_updates);

    /* Register WebSocket route on port 80 for real-time chat */
    claw_http_server_add_ws_route("/ws/chat", ws_on_open, ws_on_message, NULL);

    /* local: WebUI is an interactive chat — tool_trace clutters Markdown.
     * EPHEMERAL_SESSION: the chat_id is a browser session ID that disappears
     * when the tab closes; the LLM must not embed it in scheduler jobs. */
    claw_im_dispatch_register_with_flags("local", cap_im_local_send,
        CLAW_IM_CHANNEL_FLAG_SILENT_TRACE | CLAW_IM_CHANNEL_FLAG_EPHEMERAL_SESSION,
        "local_send_text");

    /* Register LLM-visible cap tool — consistent with other IM channels */
    claw_cap_register_group(&s_local_group);

    RTK_LOGI(TAG, "init OK\n");
    return RTK_SUCCESS;
}

int cap_im_local_start(void)
{
    RTK_LOGI(TAG, "started (WebIM ready after http_server_start)\n");
    return RTK_SUCCESS;
}

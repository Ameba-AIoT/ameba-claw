/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_im_local.h"
#include "ameba_claw_defs.h"
#include "claw_http_server.h"
#include "claw_event_publisher.h"
#include "cap_session_mgr.h"
#include "claw_im_dispatch.h"
#include "claw_cap.h"
#include "claw_utf8.h"
/* Forward declaration — avoids pulling in claw_memory.h's transitive deps */
char *claw_memory_read_session_json(const char *session_id,
                                    char *first_user_text, size_t first_size);
#include <cJSON.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "cap_im_local";

/* ---- Message ring buffer ---- */

#define MSG_BUF_MAX       CLAW_IM_LOCAL_MSG_BUF_MAX
#define MSG_TEXT_MAX      CLAW_IM_LOCAL_MSG_TEXT_MAX
/* Per-entry text limit inside the heap-allocated snapshot array.
 * Texts longer than this are truncated in the snapshot; the live
 * WS push (cap_im_local_send) always uses the full text. */
#define MSG_SNAP_TEXT_MAX 512

typedef struct {
    uint64_t id;
    char     role[12];    /* "user" or "assistant" */
    char     chat_id[64];
    char     alias[40];
    char    *text;        /* heap-allocated full message, NULL when empty */
} local_msg_t;

static local_msg_t s_msgs[MSG_BUF_MAX];
static int         s_msg_head  = 0;   /* next write position */
static int         s_msg_count = 0;   /* total messages stored (up to MSG_BUF_MAX) */
static uint64_t    s_next_id   = 1;

/* os_wrapper mutex for ring buffer */
static rtos_mutex_t s_mutex = NULL;

static uint64_t msg_push(const char *role, const char *chat_id,
                          const char *alias, const char *text)
{
    if (!s_mutex) return 0;
    rtos_mutex_take(s_mutex, 0xFFFFFFFFUL);

    local_msg_t *m = &s_msgs[s_msg_head];
    uint64_t id = s_next_id;
    m->id = s_next_id++;
    strlcpy(m->role,    role    ? role    : "user",  sizeof(m->role));
    strlcpy(m->chat_id, chat_id ? chat_id : "",      sizeof(m->chat_id));
    strlcpy(m->alias, alias ? alias : "", sizeof(m->alias));

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

    char alias[40] = {0};
    cap_session_mgr_get_current("local", "local", alias, sizeof(alias));

    /* Store user message locally */
    msg_push("user", chat_id, alias, text);

    /* Publish to event router → LLM agent */
    claw_event_dispatcher_publish_message("cap_im_local", "local",
                                      chat_id, text, NULL, alias);

    free(text);
    send_fn(sock, 200, "text/plain", "ok", 2);
}

/* ---- WebSocket handlers (port 80, /ws/chat) ---- */

/* Snapshot entry used to copy ring-buffer data out before releasing the
 * mutex, so that cJSON heap allocations happen outside the critical section.
 * text[] is a fixed-size field (truncated to MSG_SNAP_TEXT_MAX) so that no
 * heap allocation is needed while the mutex is held. */
typedef struct {
    uint64_t id;
    char     role[12];
    char     chat_id[64];
    char     alias[40];
    char     text[MSG_SNAP_TEXT_MAX];
} msg_snapshot_t;

/* Send a {type:"snapshot", alias:"...", messages:[...]} envelope to one
 * WebSocket connection.  Iterates the full ring buffer.
 * Must be called with the mutex NOT held (takes it internally). */
static void push_session_history(claw_ws_conn_t *c, const char *alias)
{
    if (!s_mutex) return;

    /* Step 1: copy ring-buffer metadata under the mutex; do NOT allocate
     * any heap memory (including cJSON nodes) while the mutex is held.
     * text[] is a fixed-size field — no heap call needed here. */
    msg_snapshot_t *snap = malloc(sizeof(msg_snapshot_t) * MSG_BUF_MAX);
    if (!snap) return;

    int count;
    rtos_mutex_take(s_mutex, 0xFFFFFFFFUL);
    count = s_msg_count;
    int start = count < MSG_BUF_MAX ? 0 : s_msg_head;
    for (int n = 0; n < count; n++) {
        int idx = (start + n) % MSG_BUF_MAX;
        snap[n].id = s_msgs[idx].id;
        strlcpy(snap[n].role,    s_msgs[idx].role,    sizeof(snap[n].role));
        strlcpy(snap[n].chat_id, s_msgs[idx].chat_id, sizeof(snap[n].chat_id));
        strlcpy(snap[n].alias,   s_msgs[idx].alias,   sizeof(snap[n].alias));
        strlcpy(snap[n].text,
                s_msgs[idx].text ? s_msgs[idx].text : "",
                sizeof(snap[n].text));
    }
    rtos_mutex_give(s_mutex);

    /* Step 2: build JSON from the snapshot — no mutex held. */
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(snap);
        return;
    }
    cJSON_AddStringToObject(root, "type",  "snapshot");
    cJSON_AddStringToObject(root, "alias", alias ? alias : "");

    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        /* OOM: abort rather than sending a malformed envelope without
         * a 'messages' field. */
        cJSON_Delete(root);
        free(snap);
        return;
    }
    for (int n = 0; n < count; n++) {
        if (alias && alias[0] && strcmp(snap[n].alias, alias) != 0) {
            continue;
        }
        cJSON *obj = cJSON_CreateObject();
        if (obj) {
            cJSON_AddNumberToObject(obj, "id",      (double)snap[n].id);
            cJSON_AddStringToObject(obj, "role",    snap[n].role);
            cJSON_AddStringToObject(obj, "chat_id", snap[n].chat_id);
            cJSON_AddStringToObject(obj, "alias",   snap[n].alias);
            cJSON_AddStringToObject(obj, "text",    snap[n].text);
            cJSON_AddItemToArray(arr, obj);
        }
    }
    /* Fallback: if no in-RAM messages found for this alias (e.g. after reboot),
     * reload from the persisted session file so the chat is not blank. */
    if (cJSON_GetArraySize(arr) == 0 && alias && alias[0]) {
        char session_id[96];
        snprintf(session_id, sizeof(session_id), "local:local:%s", alias);
        char *hist_json = claw_memory_read_session_json(session_id, NULL, 0);
        if (hist_json) {
            cJSON *hist_arr = cJSON_Parse(hist_json);
            free(hist_json);
            if (hist_arr && cJSON_IsArray(hist_arr)) {
                cJSON_Delete(arr);
                arr = hist_arr;
            } else {
                cJSON_Delete(hist_arr);
            }
        }
    }

    cJSON_AddItemToObject(root, "messages", arr);
    free(snap);

    char *j = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (j) {
        claw_ws_send_text(c, j, strlen(j));
        free(j);
    }
}

/* On open: client immediately sends {type:"sync", alias:X} which triggers
 * push_session_history; no server-side auto-push needed here. */
static void ws_on_open(claw_ws_conn_t *c)
{
    (void)c;
}

/* On message: client sent a user message. Accept either a JSON object
 * {text, alias} or {type, alias} control frame, or a raw text payload. */
static void ws_on_message(claw_ws_conn_t *c, const char *data, size_t len,
                           int is_text)
{
    if (!is_text || !data || len == 0) return;

    /* Try to parse as JSON object */
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (root) {
        cJSON *jtype  = cJSON_GetObjectItem(root, "type");
        cJSON *jtext  = cJSON_GetObjectItem(root, "text");
        cJSON *jalias = cJSON_GetObjectItem(root, "alias");

        /* {type:"sync", alias:"X"} — replay history for alias X */
        if (cJSON_IsString(jtype) && strcmp(jtype->valuestring, "sync") == 0) {
            char sync_alias[40] = {0};
            if (cJSON_IsString(jalias) && jalias->valuestring && jalias->valuestring[0]) {
                strlcpy(sync_alias, jalias->valuestring, sizeof(sync_alias));
            }
            cJSON_Delete(root);
            /* If client doesn't know the alias yet, resolve it server-side so the
             * snapshot carries the real current alias and the frontend filter works. */
            if (!sync_alias[0]) {
                if (cap_session_mgr_get_current("local", "local",
                                                 sync_alias, sizeof(sync_alias)) != RTK_SUCCESS
                        || !sync_alias[0]) {
                    strlcpy(sync_alias, "default", sizeof(sync_alias));
                }
            }
            push_session_history(c, sync_alias);
            return;
        }

        /* {text:"...", alias:"X"} — user message, route to alias X */
        if (cJSON_IsString(jtext) && jtext->valuestring && jtext->valuestring[0]) {
            char alias[40] = {0};
            if (cJSON_IsString(jalias) && jalias->valuestring) {
                strlcpy(alias, jalias->valuestring, sizeof(alias));
            }

            char *text = malloc(CLAW_IM_LOCAL_WS_TEXT_MAX);
            if (!text) { cJSON_Delete(root); return; }
            claw_utf8_truncate_copy(text, CLAW_IM_LOCAL_WS_TEXT_MAX,
                                    jtext->valuestring);
            cJSON_Delete(root);

            if (text[0] == '\0') { free(text); return; }

            RTK_LOGI(TAG, "ws recv [local:%s]: %.60s\n", alias, text);

            msg_push("user", "local", alias, text);
            claw_event_dispatcher_publish_message("cap_im_local", "local",
                                              "local", text, NULL, alias);
            free(text);
            return;
        }

        cJSON_Delete(root);
        return;
    }

    /* Raw text fallback: no alias, use empty string */
    size_t n = len < CLAW_IM_LOCAL_WS_TEXT_MAX - 1
               ? len : CLAW_IM_LOCAL_WS_TEXT_MAX - 1;
    n = claw_utf8_safe_len(data, n);

    char *text = malloc(n + 1);
    if (!text) return;
    memcpy(text, data, n);
    text[n] = '\0';

    if (text[0] == '\0') { free(text); return; }

    RTK_LOGI(TAG, "ws recv [local]: %.60s\n", text);

    msg_push("user", "local", "", text);
    claw_event_dispatcher_publish_message("cap_im_local", "local",
                                      "local", text, NULL, NULL);
    free(text);
}

/* ---- Public API ---- */

void cap_im_local_clear_alias(const char *alias)
{
    if (!s_mutex || !alias || !alias[0]) return;
    rtos_mutex_take(s_mutex, 0xFFFFFFFFUL);
    for (int i = 0; i < MSG_BUF_MAX; i++) {
        if (strcmp(s_msgs[i].alias, alias) == 0) {
            if (s_msgs[i].text) { free(s_msgs[i].text); s_msgs[i].text = NULL; }
            s_msgs[i].alias[0] = '\0';
            s_msgs[i].role[0]  = '\0';
        }
    }
    rtos_mutex_give(s_mutex);
}

/* Core send: push assistant message to ring buffer and broadcast to WS clients.
 * alias may be NULL or empty — empty alias is broadcast to ALL connected clients
 * (frontend: d.alias="" is falsy, bypasses the alias filter).  This is the
 * correct behaviour for system messages (/list, /new replies) sent before any
 * chat_map exists. */
void cap_im_local_send_for_alias(const char *alias, const char *text)
{
    if (!text) return;
    if (!alias) alias = "";

    RTK_LOGI(TAG, "assistant→[local:%s]: %.60s\n", alias, text);

    uint64_t id = msg_push("assistant", "local", alias, text);

    cJSON *obj = cJSON_CreateObject();
    if (obj) {
        cJSON_AddNumberToObject(obj, "id",      (double)id);
        cJSON_AddStringToObject(obj, "role",    "assistant");
        cJSON_AddStringToObject(obj, "chat_id", "local");
        cJSON_AddStringToObject(obj, "alias",   alias);
        cJSON_AddStringToObject(obj, "text",    text);
        char *j = cJSON_PrintUnformatted(obj);
        cJSON_Delete(obj);
        if (j) {
            claw_ws_broadcast_text("/ws/chat", j, strlen(j));
            free(j);
        }
    }
}

/* Fallback entry point registered with claw_im_dispatch for the "local" channel.
 * Uses get_current only when no alias hint is available (legacy / non-WS path).
 * If get_current fails (no chat_map yet), alias stays "" — empty alias broadcasts
 * to all WS clients so system messages (/list, /new replies) are visible. */
void cap_im_local_send(const char *chat_id, const char *text)
{
    (void)chat_id;
    if (!text) return;
    char alias[40] = {0};
    cap_session_mgr_get_current("local", "local", alias, sizeof(alias));
    /* alias is "" on failure — that is intentional, see cap_im_local_send_for_alias */
    cap_im_local_send_for_alias(alias, text);
}

/* ---- Cap group: local_send_text ---- */

/* Called when the LLM itself invokes the local_send_text tool during a session.
 * ctx->session_id = "local:local:<alias>"; extract the alias from there so the
 * progress message lands in the correct session instead of whatever get_current
 * happens to return at that moment. */
static int local_send_text_execute(const char *input_json,
                                    const claw_cap_call_context_t *ctx,
                                    char **output)
{
    char alias[40] = {0};
    if (ctx && ctx->session_id) {
        const char *p = strrchr(ctx->session_id, ':');
        if (p && p[1]) strlcpy(alias, p + 1, sizeof(alias));
    }

    if (!alias[0]) {
        /* No session context — fall back to generic path */
        return claw_im_cap_execute_send_text(input_json, output, cap_im_local_send);
    }

    /* Parse text from input_json directly (mirrors claw_im_cap_execute_send_text) */
    if (!input_json) {
        claw_cap_set_output(output, "{\"error\":\"missing input\"}");
        return RTK_ERR_BADARG;
    }
    cJSON *root = cJSON_ParseWithLength(input_json, strlen(input_json));
    if (!root) {
        claw_cap_set_output(output, "{\"error\":\"invalid JSON\"}");
        return RTK_ERR_BADARG;
    }
    cJSON *text_j = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(text_j)) {
        cJSON_Delete(root);
        claw_cap_set_output(output, "{\"error\":\"text required\"}");
        return RTK_ERR_BADARG;
    }
    cap_im_local_send_for_alias(alias, text_j->valuestring);
    cJSON_Delete(root);
    return claw_cap_set_output(output, "{\"ok\":true}");
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
            "\"text\":{\"type\":\"string\",\"description\":\"Message text to send\"}"
            "},"
            "\"required\":[\"text\"]}",
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

    /* Register WebSocket route on port 80 for real-time chat */
    claw_http_server_add_ws_route("/ws/chat", ws_on_open, ws_on_message, NULL);

    /* local: WebUI is an interactive chat — tool_trace clutters Markdown.
     * EPHEMERAL_SESSION: the chat_id is a browser session ID that disappears
     * when the tab closes; the LLM must not embed it in scheduler jobs. */
    /* NO_ACK: the generic "working on it..." ACK goes through cap_im_local_send →
     * cap_session_mgr_get_current, which returns the server-current session rather
     * than the originating session alias.  The WebUI gets real-time progress from
     * on_tool_progress (which now routes via source_message_id), so the ACK adds
     * no value and causes cross-session contamination. */
    claw_im_dispatch_register_with_flags("local", cap_im_local_send,
        CLAW_IM_CHANNEL_FLAG_SILENT_TRACE | CLAW_IM_CHANNEL_FLAG_EPHEMERAL_SESSION |
        CLAW_IM_CHANNEL_FLAG_NO_ACK,
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

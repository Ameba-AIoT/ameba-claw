/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ameba_soc.h"
#include "wechat_bot_api.h"
#include "wechat_bot_http.h"
#include "wechat_bot_token.h"
#include "cJSON.h"
#include "platform_stdlib.h"
#include "os_wrapper.h"
#include <mbedtls/base64.h>
#include <mbedtls/aes.h>
#include "llm_agent_http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/stat.h>

/* ---------- helpers ---------- */

static const char *json_str(const cJSON *item)
{
    return (item && cJSON_IsString(item)) ? item->valuestring : NULL;
}

static int json_int(const cJSON *item, int fallback)
{
    return (item && cJSON_IsNumber(item)) ? (int)item->valuedouble : fallback;
}

static void context_remember(wechat_bot_state_t *state,
                             const char *chat_id, const char *ctx_token)
{
    size_t i;

    if (!chat_id || !chat_id[0] || !ctx_token || !ctx_token[0]) {
        return;
    }

    /* Update existing entry */
    for (i = 0; i < WECHAT_CONTEXT_CACHE_SIZE; i++) {
        if (strcmp(state->context_cache[i].chat_id, chat_id) == 0) {
            strncpy(state->context_cache[i].context_token, ctx_token,
                    sizeof(state->context_cache[i].context_token) - 1);
            state->context_cache[i].context_token[sizeof(state->context_cache[i].context_token) - 1] = '\0';
            return;
        }
    }

    /* Insert at next slot (ring) */
    strncpy(state->context_cache[state->context_idx].chat_id, chat_id,
            sizeof(state->context_cache[state->context_idx].chat_id) - 1);
    state->context_cache[state->context_idx].chat_id[
        sizeof(state->context_cache[state->context_idx].chat_id) - 1] = '\0';
    strncpy(state->context_cache[state->context_idx].context_token, ctx_token,
            sizeof(state->context_cache[state->context_idx].context_token) - 1);
    state->context_cache[state->context_idx].context_token[
        sizeof(state->context_cache[state->context_idx].context_token) - 1] = '\0';
    state->context_idx = (state->context_idx + 1) % WECHAT_CONTEXT_CACHE_SIZE;
}

static const char *context_lookup(wechat_bot_state_t *state, const char *chat_id)
{
    size_t i;

    for (i = 0; i < WECHAT_CONTEXT_CACHE_SIZE; i++) {
        if (strcmp(state->context_cache[i].chat_id, chat_id) == 0 &&
            state->context_cache[i].context_token[0]) {
            return state->context_cache[i].context_token;
        }
    }
    return NULL;
}

static void build_x_wechat_uin(char *buf, size_t buf_size)
{
    uint32_t value = (uint32_t)rand();
    char decimal[16];
    unsigned char encoded[32];
    size_t out_len = 0;

    DiagSnPrintf(decimal, sizeof(decimal), "%" PRIu32, value);
    mbedtls_base64_encode(encoded, sizeof(encoded), &out_len,
                          (const unsigned char *)decimal, strlen(decimal));
    if (out_len + 1 > buf_size) {
        out_len = buf_size - 1;
    }
    _memcpy(buf, encoded, out_len);
    buf[out_len] = '\0';
}

static void build_client_id(char *buf, size_t buf_size)
{
    uint32_t a = (uint32_t)rand();
    uint32_t b = (uint32_t)rand();
    DiagSnPrintf(buf, buf_size, "amebawx-%08" PRIx32 "%08" PRIx32, a, b);
}

/* Build the common headers array for WeChat API requests.
 * Returns number of headers added (pairs of name, value, NULL-terminated).
 * Caller must provide headers array with room for at least 10 entries. */
static int build_common_headers(const char *headers[], int max_pairs,
                                const char *app_id,
                                const char *token, int use_auth)
{
    int count = 0;
    static char x_uin[64];
    static char auth_buf[WECHAT_API_TOKEN_SIZE + 16];

    /* iLink-App-Id */
    if (count < max_pairs) {
        headers[count * 2] = "iLink-App-Id";
        headers[count * 2 + 1] = (app_id && app_id[0]) ? app_id : WECHAT_DEFAULT_APP_ID;
        count++;
    }
    /* iLink-App-ClientVersion */
    if (count < max_pairs) {
        headers[count * 2] = "iLink-App-ClientVersion";
        headers[count * 2 + 1] = WECHAT_DEFAULT_CLIENT_VER;
        count++;
    }
    /* X-WECHAT-UIN */
    if (count < max_pairs) {
        build_x_wechat_uin(x_uin, sizeof(x_uin));
        headers[count * 2] = "X-WECHAT-UIN";
        headers[count * 2 + 1] = x_uin;
        count++;
    }
    /* Auth headers */
    if (use_auth && token && token[0]) {
        if (count < max_pairs) {
            headers[count * 2] = "AuthorizationType";
            headers[count * 2 + 1] = "ilink_bot_token";
            count++;
        }
        if (count < max_pairs) {
            DiagSnPrintf(auth_buf, sizeof(auth_buf), "Bearer %s", token);
            headers[count * 2] = "Authorization";
            headers[count * 2 + 1] = auth_buf;
            count++;
        }
    }

    headers[count * 2] = NULL;
    headers[count * 2 + 1] = NULL;
    return count;
}

/* Parse host from URL like "https://ilinkai.weixin.qq.com" */
static void parse_host_from_url(const char *url, char *host, size_t host_size)
{
    const char *start = url;
    const char *end;

    if (strncmp(start, "https://", 8) == 0) {
        start += 8;
    } else if (strncmp(start, "http://", 7) == 0) {
        start += 7;
    }

    end = strchr(start, '/');
    if (end) {
        size_t len = (size_t)(end - start);
        if (len >= host_size) {
            len = host_size - 1;
        }
        _memcpy(host, start, len);
        host[len] = '\0';
    } else {
        strncpy(host, start, host_size - 1);
        host[host_size - 1] = '\0';
    }
}

/* ---------- QR Fetch (single request, no polling) ---------- */

int wechat_api_qr_fetch(wechat_bot_state_t *state,
                        char *qr_url, size_t qr_url_size,
                        char *qr_id,  size_t qr_id_size)
{
    char host[128];
    char resource[256];
    const char *headers[12];
    wechat_http_resp_t resp = {0};
    cJSON *root = NULL;
    const char *qrcode = NULL;
    const char *qrcode_img_content = NULL;

    if (!state || !qr_url || !qr_id) return -1;

    parse_host_from_url(state->base_url, host, sizeof(host));
    DiagSnPrintf(resource, sizeof(resource), "/ilink/bot/get_bot_qrcode?bot_type=3");
    build_common_headers(headers, 5, state->app_id, NULL, 0);

    if (wechat_http_resp_init(&resp) != 0) return -2;

    if (wechat_http_get(host, resource, headers, &resp) != 0) {
        wechat_http_resp_free(&resp);
        return -3;
    }

    root = cJSON_Parse(resp.buf);
    wechat_http_resp_free(&resp);
    if (!root) return -4;

    qrcode             = json_str(cJSON_GetObjectItemCaseSensitive(root, "qrcode"));
    qrcode_img_content = json_str(cJSON_GetObjectItemCaseSensitive(root, "qrcode_img_content"));

    if (!qrcode || !qrcode_img_content) {
        cJSON_Delete(root);
        return -5;
    }

    strncpy(qr_url, qrcode_img_content, qr_url_size - 1);
    qr_url[qr_url_size - 1] = '\0';
    strncpy(qr_id, qrcode, qr_id_size - 1);
    qr_id[qr_id_size - 1] = '\0';

    cJSON_Delete(root);
    DiagPrintf("[wechat] QR fetched: id=%s\n", qr_id);
    return 0;
}

/* ---------- QR Poll Once ---------- */

int wechat_api_qr_poll_once(wechat_bot_state_t *state,
                             wechat_http_session_t *session,
                             const char *qr_id)
{
    char resource[256];
    const char *headers[12];
    wechat_http_resp_t resp = {0};
    cJSON *root = NULL;
    const char *status;
    int result;

    if (!state || !session || !qr_id) return -1;

    DiagSnPrintf(resource, sizeof(resource), "/ilink/bot/get_qrcode_status?qrcode=%s", qr_id);
    build_common_headers(headers, 5, state->app_id, NULL, 0);

    if (wechat_http_resp_init(&resp) != 0) return -2;

    if (wechat_http_session_get(session, resource, headers, &resp) != 0) {
        wechat_http_resp_free(&resp);
        return -3;
    }

    root = cJSON_Parse(resp.buf);
    wechat_http_resp_free(&resp);
    if (!root) return -4;

    status = json_str(cJSON_GetObjectItemCaseSensitive(root, "status"));
    if (!status) { cJSON_Delete(root); return -5; }

    if (strcmp(status, "wait") == 0) {
        result = WECHAT_QR_WAIT;
    } else if (strcmp(status, "scanned") == 0) {
        result = WECHAT_QR_SCANNED;
    } else if (strcmp(status, "scaned_but_redirect") == 0) {
        const char *rh = json_str(cJSON_GetObjectItemCaseSensitive(root, "redirect_host"));
        if (rh && rh[0]) {
            DiagSnPrintf(state->base_url, sizeof(state->base_url), "https://%s", rh);
            DiagPrintf("[wechat] QR redirect -> %s\n", state->base_url);
        }
        result = WECHAT_QR_REDIRECTED;
    } else if (strcmp(status, "expired") == 0) {
        result = WECHAT_QR_EXPIRED;
    } else if (strcmp(status, "confirmed") == 0) {
        const char *bot_token = json_str(cJSON_GetObjectItemCaseSensitive(root, "bot_token"));
        const char *baseurl   = json_str(cJSON_GetObjectItemCaseSensitive(root, "baseurl"));
        if (bot_token) {
            strncpy(state->token, bot_token, sizeof(state->token) - 1);
            state->token[sizeof(state->token) - 1] = '\0';
        }
        if (baseurl && baseurl[0]) {
            strncpy(state->base_url, baseurl, sizeof(state->base_url) - 1);
            state->base_url[sizeof(state->base_url) - 1] = '\0';
        }
        wechat_token_save(state->token);
        DiagPrintf("[wechat] Login confirmed. Base URL: %s\n", state->base_url);
        result = WECHAT_QR_CONFIRMED;
    } else {
        result = WECHAT_QR_WAIT;
    }

    cJSON_Delete(root);
    return result;
}

/* ---------- QR Login ---------- */

int wechat_api_qr_login(wechat_bot_state_t *state)
{
    char host[128];
    char resource[256];
    const char *headers[12];
    wechat_http_resp_t resp = {0};
    cJSON *root = NULL;
    const char *qrcode = NULL;
    const char *qrcode_img_content = NULL;
    char current_base[WECHAT_API_BASE_URL_SIZE];
    char qr_id[96];
    int retry;

    if (!state) {
        return -1;
    }

    strncpy(current_base, WECHAT_DEFAULT_BASE_URL, sizeof(current_base) - 1);
    current_base[sizeof(current_base) - 1] = '\0';

    for (retry = 0; ; retry++) {
        int elapsed;
        int qr_ttl_sec = 5 * 60;

        /* Step 1: get QR code */
        DiagPrintf("[wechat] Fetching QR code (attempt %d)...\n", retry + 1);

        parse_host_from_url(current_base, host, sizeof(host));
        DiagSnPrintf(resource, sizeof(resource),
                 "/ilink/bot/get_bot_qrcode?bot_type=3");

        build_common_headers(headers, 5, state->app_id, NULL, 0);

        if (wechat_http_resp_init(&resp) != 0) {
            DiagPrintf("[wechat] resp init failed\n");
            return -2;
        }

        if (wechat_http_get(host, resource, headers, &resp) != 0) {
            DiagPrintf("[wechat] get_bot_qrcode request failed\n");
            wechat_http_resp_free(&resp);
            return -3;
        }

        root = cJSON_Parse(resp.buf);
        wechat_http_resp_free(&resp);

        if (!root) {
            DiagPrintf("[wechat] get_bot_qrcode JSON parse failed\n");
            return -4;
        }

        qrcode = json_str(cJSON_GetObjectItemCaseSensitive(root, "qrcode"));
        qrcode_img_content = json_str(
            cJSON_GetObjectItemCaseSensitive(root, "qrcode_img_content"));

        if (!qrcode || !qrcode_img_content) {
            DiagPrintf("[wechat] get_bot_qrcode missing fields\n");
            cJSON_Delete(root);
            return -5;
        }

        strncpy(qr_id, qrcode, sizeof(qr_id) - 1);
        qr_id[sizeof(qr_id) - 1] = '\0';

        DiagPrintf("\n[wechat] ============================================================\n");
        DiagPrintf("[wechat] QR URL: %s\n", qrcode_img_content);
        DiagPrintf("[wechat] QR ID: %s\n", qr_id);
        DiagPrintf("[wechat] Open the URL above in a browser, then scan with WeChat.\n");
        DiagPrintf("[wechat] ============================================================\n\n");

        cJSON_Delete(root);
        root = NULL;

        /* Step 2: poll QR status — reuse TLS session for fast polling */
        {
            wechat_http_session_t *qr_session;
            parse_host_from_url(current_base, host, sizeof(host));
            qr_session = wechat_http_session_open(host);
            if (!qr_session) {
                DiagPrintf("[wechat] failed to open QR poll session\n");
                continue;
            }

        for (elapsed = 0; elapsed < qr_ttl_sec; elapsed++) {
            const char *status;
            const char *redirect_host;
            const char *bot_token;
            const char *baseurl;

            rtos_time_delay_ms(1000);

            DiagSnPrintf(resource, sizeof(resource),
                     "/ilink/bot/get_qrcode_status?qrcode=%s", qr_id);

            build_common_headers(headers, 5, state->app_id, NULL, 0);

            if (wechat_http_resp_init(&resp) != 0) {
                DiagPrintf("[wechat] poll resp init failed\n");
                continue;
            }

            if (wechat_http_session_get(qr_session, resource, headers, &resp) != 0) {
                DiagPrintf("[wechat] get_qrcode_status request failed\n");
                wechat_http_resp_free(&resp);
                continue;
            }

            DiagPrintf("[wechat] poll resp (%u bytes)\n", (unsigned)resp.len);

            root = cJSON_Parse(resp.buf);
            wechat_http_resp_free(&resp);

            if (!root) {
                DiagPrintf("[wechat] poll JSON parse failed\n");
                continue;
            }

            status = json_str(cJSON_GetObjectItemCaseSensitive(root, "status"));

            if (!status) {
                DiagPrintf("[wechat] poll no status field\n");
                cJSON_Delete(root);
                root = NULL;
                continue;
            }

            if (strcmp(status, "wait") == 0) {
                DiagPrintf("[wechat] Waiting for scan...\n");
            } else if (strcmp(status, "scanned") == 0) {
                DiagPrintf("[wechat] QR scanned, waiting for confirmation...\n");
            } else if (strcmp(status, "scaned_but_redirect") == 0) {
                redirect_host = json_str(
                    cJSON_GetObjectItemCaseSensitive(root, "redirect_host"));
                if (redirect_host && redirect_host[0]) {
                    DiagSnPrintf(current_base, sizeof(current_base),
                             "https://%s", redirect_host);
                    DiagPrintf("[wechat] Redirected to %s\n", current_base);
                }
            } else if (strcmp(status, "expired") == 0) {
                DiagPrintf("[wechat] QR expired, refreshing...\n");
                cJSON_Delete(root);
                root = NULL;
                break;  /* retry outer loop */
            } else if (strcmp(status, "confirmed") == 0) {
                DiagPrintf("[wechat] Login confirmed!\n");
                bot_token = json_str(
                    cJSON_GetObjectItemCaseSensitive(root, "bot_token"));

                if (bot_token) {
                    strncpy(state->token, bot_token,
                            sizeof(state->token) - 1);
                    state->token[sizeof(state->token) - 1] = '\0';
                }

                baseurl = json_str(
                    cJSON_GetObjectItemCaseSensitive(root, "baseurl"));
                if (baseurl && baseurl[0]) {
                    strncpy(state->base_url, baseurl,
                            sizeof(state->base_url) - 1);
                    state->base_url[sizeof(state->base_url) - 1] = '\0';
                } else {
                    strncpy(state->base_url, current_base,
                            sizeof(state->base_url) - 1);
                    state->base_url[sizeof(state->base_url) - 1] = '\0';
                }

                cJSON_Delete(root);
                wechat_http_session_close(qr_session);
                wechat_token_save(state->token);
                DiagPrintf("[wechat] Token saved. Base URL: %s\n", state->base_url);
                return 0;
            }

            cJSON_Delete(root);
            root = NULL;
        }

        wechat_http_session_close(qr_session);
        } /* end session block */

        /* If we got here via break (expired), retry outer loop */
    }

    DiagPrintf("[wechat] QR login failed after %d retries\n", retry);
    return -1;
}

/* ---------- Open persistent poll session ---------- */

wechat_http_session_t *wechat_api_open_poll_session(wechat_bot_state_t *state)
{
    char host[128];
    if (!state || !state->base_url[0]) {
        return NULL;
    }
    parse_host_from_url(state->base_url, host, sizeof(host));
    return wechat_http_session_open(host);
}

/* ---------- Poll (getupdates) ---------- */

int wechat_api_poll(wechat_bot_state_t *state,
                    void (*item_callback)(const wechat_item_t *item,
                                         void *user_data),
                    void *user_data,
                    wechat_http_session_t *poll_session)
{
    char host[128];
    char resource[128];
    const char *headers[12];
    wechat_http_resp_t resp = {0};
    cJSON *root = NULL;
    cJSON *msgs = NULL;
    cJSON *msg = NULL;
    char *body_json = NULL;
    int i, msg_count;
    int ret;

    if (!state || !state->token[0]) {
        return -1;
    }

    /* Build request JSON */
    root = cJSON_CreateObject();
    if (!root) {
        return -2;
    }

    cJSON_AddStringToObject(root, "get_updates_buf",
                            state->sync_buf ? state->sync_buf : "");

    {
        cJSON *base_info = cJSON_AddObjectToObject(root, "base_info");
        if (!base_info) {
            cJSON_Delete(root);
            return -2;
        }
        cJSON_AddStringToObject(base_info, "channel_version", "ameba-wechat-bot");
    }

    body_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    root = NULL;

    if (!body_json) {
        return -2;
    }

    parse_host_from_url(state->base_url, host, sizeof(host));
    DiagSnPrintf(resource, sizeof(resource), "/ilink/bot/getupdates");

    build_common_headers(headers, 5, state->app_id, state->token, 1);

    if (wechat_http_resp_init(&resp) != 0) {
        free(body_json);
        return -2;
    }

    if (poll_session) {
        ret = wechat_http_session_post(poll_session, resource,
                                       "application/json",
                                       body_json, strlen(body_json),
                                       headers, &resp);
    } else {
        ret = wechat_http_post(host, resource,
                               "application/json",
                               body_json, strlen(body_json),
                               headers, &resp);
    }
    free(body_json);

    if (ret != 0) {
        wechat_http_resp_free(&resp);
        return -3;
    }

    root = cJSON_Parse(resp.buf);
    wechat_http_resp_free(&resp);

    if (!root) {
        return -4;
    }

    /* Check for error response */
    if (json_int(cJSON_GetObjectItemCaseSensitive(root, "ret"), 0) != 0 ||
        json_int(cJSON_GetObjectItemCaseSensitive(root, "errcode"), 0) != 0) {
        const char *errmsg = json_str(cJSON_GetObjectItemCaseSensitive(root, "errmsg"));
        DiagPrintf("[wechat] getupdates error: %s\n", errmsg ? errmsg : "(no errmsg)");
        cJSON_Delete(root);
        return -5;
    }

    /* Update sync buffer (cursor) */
    {
        const char *next_sync = json_str(
            cJSON_GetObjectItemCaseSensitive(root, "get_updates_buf"));
        if (next_sync) {
            char *dup = strdup(next_sync);
            if (dup) {
                free(state->sync_buf);
                state->sync_buf = dup;
            }
        }
    }

    /* Process messages */
    msgs = cJSON_GetObjectItemCaseSensitive(root, "msgs");
    if (!msgs || !cJSON_IsArray(msgs)) {
        cJSON_Delete(root);
        return 0;
    }

    msg_count = cJSON_GetArraySize(msgs);
    for (i = 0; i < msg_count; i++) {
        const char *from_user_id;
        const char *group_id;
        const char *ctx_token;
        const char *chat_id;
        cJSON *item_list;
        int j, item_count;

        msg = cJSON_GetArrayItem(msgs, i);

        from_user_id = json_str(
            cJSON_GetObjectItemCaseSensitive(msg, "from_user_id"));
        group_id = json_str(
            cJSON_GetObjectItemCaseSensitive(msg, "group_id"));
        ctx_token = json_str(
            cJSON_GetObjectItemCaseSensitive(msg, "context_token"));

        chat_id = (group_id && group_id[0]) ? group_id : from_user_id;
        if (!chat_id || !chat_id[0]) {
            continue;
        }

        if (ctx_token && ctx_token[0]) {
            context_remember(state, chat_id, ctx_token);
        }

        /* Extract text items (type=1) */
        item_list = cJSON_GetObjectItemCaseSensitive(msg, "item_list");
        if (!item_list || !cJSON_IsArray(item_list)) {
            continue;
        }

        item_count = cJSON_GetArraySize(item_list);
        for (j = 0; j < item_count; j++) {
            cJSON *raw_item = cJSON_GetArrayItem(item_list, j);
            int type = json_int(cJSON_GetObjectItemCaseSensitive(raw_item, "type"), -1);

            if (!item_callback) continue;

            wechat_item_t wi;
            _memset(&wi, 0, sizeof(wi));
            wi.type      = type;
            wi.chat_id   = chat_id;
            wi.sender_id = from_user_id ? from_user_id : chat_id;
            wi.msg_id    = json_str(cJSON_GetObjectItemCaseSensitive(raw_item, "msg_id"));

            if (type == 1) {
                cJSON *text_item = cJSON_GetObjectItemCaseSensitive(raw_item, "text_item");
                if (text_item) {
                    wi.text = json_str(cJSON_GetObjectItemCaseSensitive(text_item, "text"));
                }
                if (wi.text && wi.text[0]) {
                    item_callback(&wi, user_data);
                }
            } else if (type == 2) {
                /* Image: extract URL and AES key from image_item.media */
                cJSON *img  = cJSON_GetObjectItemCaseSensitive(raw_item, "image_item");
                cJSON *med  = img ? cJSON_GetObjectItemCaseSensitive(img, "media") : NULL;
                if (img && med) {
                    wi.image_aeskey = json_str(cJSON_GetObjectItemCaseSensitive(img, "aeskey"));
                    wi.image_url    = json_str(cJSON_GetObjectItemCaseSensitive(med, "full_url"));
                    wi.image_size   = json_int(cJSON_GetObjectItemCaseSensitive(med, "mid_size"), 0);
                    if (wi.image_url && wi.image_aeskey) {
                        item_callback(&wi, user_data);
                    }
                }
            }
            /* Other message types (voice, video, etc.) are not yet handled */
        }
    }

    cJSON_Delete(root);
    return 0;
}

/* ---------- Send Message ---------- */

int wechat_api_send_text(wechat_bot_state_t *state,
                         const char *chat_id,
                         const char *text)
{
    char host[128];
    char resource[128];
    const char *headers[12];
    wechat_http_resp_t resp = {0};
    cJSON *root = NULL;
    cJSON *msg = NULL;
    cJSON *item_list = NULL;
    cJSON *item = NULL;
    cJSON *text_item = NULL;
    char client_id[48];
    const char *ctx_token = NULL;
    char *body_json = NULL;
    int ret;

    if (!state || !state->token[0] || !chat_id || !text) {
        return -1;
    }

    /* Build message JSON */
    msg = cJSON_CreateObject();
    if (!msg) {
        return -2;
    }

    build_client_id(client_id, sizeof(client_id));
    cJSON_AddStringToObject(msg, "from_user_id", "");
    cJSON_AddStringToObject(msg, "to_user_id", chat_id);
    cJSON_AddStringToObject(msg, "client_id", client_id);
    cJSON_AddNumberToObject(msg, "message_type", 2);
    cJSON_AddNumberToObject(msg, "message_state", 2);

    ctx_token = context_lookup(state, chat_id);
    if (ctx_token) {
        cJSON_AddStringToObject(msg, "context_token", ctx_token);
    }

    item_list = cJSON_AddArrayToObject(msg, "item_list");
    item = cJSON_CreateObject();
    text_item = cJSON_CreateObject();

    if (!item_list || !item || !text_item) {
        cJSON_Delete(msg);
        return -2;
    }

    cJSON_AddStringToObject(text_item, "text", text);
    cJSON_AddNumberToObject(item, "type", 1);
    cJSON_AddItemToObject(item, "text_item", text_item);
    cJSON_AddItemToArray(item_list, item);

    /* Wrap in outer object */
    root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(msg);
        return -2;
    }

    cJSON_AddItemToObject(root, "msg", msg);

    {
        cJSON *base_info = cJSON_AddObjectToObject(root, "base_info");
        if (!base_info) {
            cJSON_Delete(root);
            return -2;
        }
        cJSON_AddStringToObject(base_info, "channel_version", "ameba-wechat-bot");
    }

    body_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!body_json) {
        return -2;
    }

    parse_host_from_url(state->base_url, host, sizeof(host));
    DiagSnPrintf(resource, sizeof(resource), "/ilink/bot/sendmessage");

    build_common_headers(headers, 5, state->app_id, state->token, 1);

    if (wechat_http_resp_init(&resp) != 0) {
        free(body_json);
        return -2;
    }

    ret = wechat_http_post(host, resource,
                           "application/json",
                           body_json, strlen(body_json),
                           headers, &resp);
    free(body_json);

    if (ret != 0) {
        wechat_http_resp_free(&resp);
        DiagPrintf("[wechat] sendmessage: HTTP error %d\n", ret);
        return -3;
    }

    /* Check application-level result.  ret=-2 / errmsg="rate limited" means
     * the platform's per-turn message quota was exceeded. */
    if (resp.buf) {
        cJSON *r = cJSON_Parse(resp.buf);
        if (r) {
            int app_ret = (int)json_int(
                cJSON_GetObjectItemCaseSensitive(r, "ret"), 0);
            if (app_ret != 0) {
                const char *msg = json_str(
                    cJSON_GetObjectItemCaseSensitive(r, "errmsg"));
                DiagPrintf("[wechat] sendmessage error ret=%d errmsg=%s\n",
                           app_ret, msg ? msg : "(null)");
                cJSON_Delete(r);
                wechat_http_resp_free(&resp);
                /* -2 = rate limited; surface as distinct error code */
                return (app_ret == -2) ? -6 : -5;
            }
            cJSON_Delete(r);
        }
    }
    wechat_http_resp_free(&resp);
    return 0;
}

/* ---------- State Init/Cleanup ---------- */

void wechat_api_state_init(wechat_bot_state_t *state)
{
    if (!state) {
        return;
    }
    _memset(state, 0, sizeof(*state));
    strncpy(state->base_url, WECHAT_DEFAULT_BASE_URL,
            sizeof(state->base_url) - 1);
    strncpy(state->app_id, WECHAT_DEFAULT_APP_ID,
            sizeof(state->app_id) - 1);
    state->poll_timeout_ms = 35000;
}

void wechat_api_state_cleanup(wechat_bot_state_t *state)
{
    if (!state) {
        return;
    }
    if (state->sync_buf) {
        free(state->sync_buf);
        state->sync_buf = NULL;
    }
}

/* ---- Download + AES-128-ECB decrypt WeChat image -------------------------
 *
 * WeChat iLink encrypts images with AES-128-ECB before uploading to CDN.
 * The aeskey is a 32-char hex string (16 bytes).  ECB has no IV; each
 * 16-byte block is decrypted independently — ideal for streaming.
 *
 * RTL8721F has a hardware AES engine; mbedtls_aes_crypt_ecb() is backed
 * by the HW via MBEDTLS_AES_ALT, so this is fast and uses no extra heap
 * beyond the 256-byte work buffer.
 *
 * Flow:
 *   1. Parse host/resource from full_url
 *   2. HTTPS GET → stream to VFS in 256-byte (16 blocks) chunks
 *   3. Decrypt each chunk in-place with mbedtls AES-128-ECB
 *   4. Write plaintext to dest_path
 * ----------------------------------------------------------------------- */

/* Convert one hex nibble → value.  Returns -1 on invalid input. */
static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse 32-char hex string → 16 bytes.  Returns 0 on success. */
static int parse_aeskey(const char *hex, uint8_t key[16])
{
    int i;
    if (!hex) return -1;
    for (i = 0; i < 16; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        key[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

int wechat_api_download_decrypt_image(const char *full_url,
                                       const char *aeskey_hex,
                                       const char *dest_path,
                                       size_t     *out_bytes)
{
    uint8_t         aes_key[16];
    mbedtls_aes_context aes_ctx;
    uint8_t        *enc_buf  = NULL;
    uint8_t        *dec_buf  = NULL;
    FILE           *fp       = NULL;
    size_t          written  = 0;
    int             ret      = -1;

    if (!full_url || !aeskey_hex || !dest_path) return -1;

    if (parse_aeskey(aeskey_hex, aes_key) != 0) {
        DiagPrintf("[wechat] download_decrypt: bad aeskey '%s'\n", aeskey_hex);
        return -1;
    }

    /* Parse https://<host><resource> from full_url */
    const char *host_start = full_url;
    if (strncmp(full_url, "https://", 8) == 0)     host_start += 8;
    else if (strncmp(full_url, "http://", 7) == 0) host_start += 7;

    const char *path_start = strchr(host_start, '/');
    char host_buf[128] = {0};
    if (path_start) {
        size_t hlen = (size_t)(path_start - host_start);
        if (hlen >= sizeof(host_buf)) hlen = sizeof(host_buf) - 1;
        memcpy(host_buf, host_start, hlen);
    } else {
        strlcpy(host_buf, host_start, sizeof(host_buf));
        path_start = "/";
    }

    /* Work buffer: 256 bytes = 16 AES blocks.  Encrypt and decrypt buffers
     * are separate so mbedtls doesn't alias input/output (defensive). */
    enc_buf = (uint8_t *)malloc(256);
    dec_buf = (uint8_t *)malloc(256);
    if (!enc_buf || !dec_buf) { ret = -2; goto cleanup; }

    /* Init HW AES context — decryption key setup */
    mbedtls_aes_init(&aes_ctx);
    if (mbedtls_aes_setkey_dec(&aes_ctx, aes_key, 128) != 0) {
        DiagPrintf("[wechat] download_decrypt: setkey_dec failed\n");
        ret = -3; goto cleanup;
    }

    /* Download encrypted content via HTTPS GET to a temp VFS file, then
     * open that temp file for reading while writing decrypted output.
     * Avoids keeping the entire image in RAM. */
    char tmp_path[256];
    DiagSnPrintf(tmp_path, sizeof(tmp_path), "vfs:/tmp/wx_enc_%08x", (unsigned)((uintptr_t)full_url & 0xFFFFFFFFu));

    /* Stream download to temp file */
    if (llm_http_get_to_file(host_buf, path_start, tmp_path, 0, NULL) != 0) {
        DiagPrintf("[wechat] download_decrypt: GET failed\n");
        ret = -4; goto cleanup;
    }

    /* Create destination directory */
    {
        char dir_tmp[256];
        strlcpy(dir_tmp, dest_path + 4, sizeof(dir_tmp)); /* skip "vfs:" */
        char *slash = strrchr(dir_tmp, '/');
        if (slash && slash != dir_tmp) {
            *slash = '\0';
            char full_dir[264];
            DiagSnPrintf(full_dir, sizeof(full_dir), "vfs:%s", dir_tmp);
            mkdir(full_dir, 0777);
        }
    }

    /* Open temp (encrypted) and dest (plaintext) files */
    FILE *enc_fp = fopen(tmp_path, "rb");
    if (!enc_fp) { ret = -5; goto cleanup; }

    fp = fopen(dest_path, "wb");
    if (!fp) { fclose(enc_fp); ret = -6; goto cleanup; }

    /* Decrypt 16-block (256-byte) chunks */
    size_t n;
    while ((n = fread(enc_buf, 1, 256, enc_fp)) > 0) {
        /* Round down to AES block size — discard trailing padding bytes */
        size_t blocks = n / 16;
        size_t b;
        for (b = 0; b < blocks; b++) {
            mbedtls_aes_crypt_ecb(&aes_ctx, MBEDTLS_AES_DECRYPT,
                                   enc_buf + b * 16,
                                   dec_buf + b * 16);
        }
        size_t out_n = blocks * 16;
        fwrite(dec_buf, 1, out_n, fp);
        written += out_n;
    }
    fclose(enc_fp);
    remove(tmp_path); /* delete temp encrypted file */
    ret = 0;

cleanup:
    mbedtls_aes_free(&aes_ctx);
    if (fp) fclose(fp);
    free(enc_buf);
    free(dec_buf);
    if (out_bytes) *out_bytes = written;
    if (ret != 0 && dest_path) remove(dest_path); /* clean partial file */
    DiagPrintf("[wechat] download_decrypt: %s -> %s (%u bytes, ret=%d)\n",
               full_url ? "url" : "null", dest_path, (unsigned)written, ret);
    return ret;
}

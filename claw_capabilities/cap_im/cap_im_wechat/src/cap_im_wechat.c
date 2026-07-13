/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_im_wechat.h"
#include "cap_im_attachment.h"
#include "claw_cap.h"
#include "claw_config.h"
#include "claw_wifi_mgr.h"
#include "claw_im_dispatch.h"
#include "claw_event_publisher.h"
#include "wechat_bot_api.h"
#include "wechat_bot_token.h"
#include "wechat_bot_http.h"
#include "os_wrapper.h"
#include "lwip_netconf.h"
#include "wifi_api.h"
#include <cJSON.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TAG "cap_im_wechat"

/* ---- WeChat per-turn message limit constants ----------------------------
 * WeChat iLink bots allow at most 10 outbound messages per reply turn.
 * A "turn" starts when the user sends a message; the counter resets when
 * cap_im_wechat_send() is called (the final reply).
 *
 * Budget breakdown (10 slots total):
 *   - progress msgs : up to WECHAT_PROG_LIMIT slots
 *   - notice        : 1 slot  (sent when progress limit is reached)
 *   - final reply   : 1 slot
 *   → WECHAT_PROG_LIMIT = WECHAT_PLATFORM_LIMIT - 2
 *
 * Note: ACK (sent via cap_im_wechat_send before LLM starts) resets the
 * counter to 0 instead of consuming a slot, so it does not reduce budget.
 * -------------------------------------------------------------------- */
#define WECHAT_PLATFORM_LIMIT  10  /* platform hard limit per turn */
#define WECHAT_PROG_LIMIT      (WECHAT_PLATFORM_LIMIT - 2)  /* 8 progress slots */
#define WECHAT_PROG_SLOTS      4   /* concurrent chat_id entries tracked */
#define WECHAT_MAX_MSG_BYTES   2000 /* mobile WeChat silently drops longer messages */

/* ---- Shared state ---- */

static struct {
    wechat_bot_state_t      bot;
    cap_im_wechat_config_t  cfg;
    rtos_mutex_t            lock;
    cap_im_wechat_state_t   state;
    char                    qr_url[512];
    char                    qr_id[96];
    int                     initialized;
    int                     poll_gen;   /* incremented each time a new polling session starts;
                                         * msg poll task exits when its captured gen mismatches */

    /* Progress rate-limiter: per-chat per-turn counters (protected by lock). */
    struct {
        char chat_id[64];
        int  count;     /* outbound progress messages sent this turn */
    } prog[WECHAT_PROG_SLOTS];
    int prog_next;               /* circular eviction index */
} s;

/* ---- Helpers ---- */

static void get_host_from_url(const char *url, char *host, size_t sz)
{
    const char *p = url;
    size_t n;
    const char *end;

    if (strncmp(p, "https://", 8) == 0)      p += 8;
    else if (strncmp(p, "http://", 7) == 0)  p += 7;

    end = strchr(p, '/');
    if (end) {
        n = (size_t)(end - p);
        if (n >= sz) n = sz - 1;
        _memcpy(host, p, n);
        host[n] = '\0';
    } else {
        strncpy(host, p, sz - 1);
        host[sz - 1] = '\0';
    }
}

/* ---- Download function passed to cap_im_attachment for WeChat images ---- */

typedef struct {
    char aeskey_hex[36]; /* 32-char hex + NUL */
} wechat_dl_ctx_t;

static int wechat_image_download_fn(const char *url,
                                     const char *dest_path,
                                     void       *ctx)
{
    wechat_dl_ctx_t *dctx = (wechat_dl_ctx_t *)ctx;
    size_t bytes = 0;
    int ret = wechat_api_download_decrypt_image(url, dctx->aeskey_hex,
                                                dest_path, &bytes);
    (void)bytes;
    return ret;
}

static int prog_slot(const char *chat_id);  /* defined below */

/* ---- Incoming item callback ---- */

static void on_item(const wechat_item_t *item, void *user_data)
{
    (void)user_data;

    if (item->type == 1) {
        /* Text message — reset our per-turn send counter so this reply turn
         * starts with a fresh local budget.  Note: this only resets the local
         * counter; the platform's own rate-limit window is independent and
         * may not have expired yet. */
        if (item->chat_id && item->chat_id[0]) {
            rtos_mutex_take(s.lock, RTOS_MAX_TIMEOUT);
            int idx = prog_slot(item->chat_id);
            s.prog[idx].count = 0;
            rtos_mutex_give(s.lock);
        }
        DiagPrintf("[wechat] msg from %s chat=%s: %.60s\n",
                   item->sender_id ? item->sender_id : "?",
                   item->chat_id   ? item->chat_id   : "?",
                   item->text      ? item->text      : "");
        claw_event_dispatcher_publish_message(
            "cap_im_wechat", "wechat",
            item->chat_id, item->text,
            item->sender_id, item->msg_id);

    } else if (item->type == 2) {
        /* Image: enqueue download+decrypt job to cap_im_attachment */
        DiagPrintf("[wechat] image from %s chat=%s size=%d\n",
                   item->sender_id ? item->sender_id : "?",
                   item->chat_id   ? item->chat_id   : "?",
                   item->image_size);

        /* Allocate download context (freed by attachment task after download) */
        wechat_dl_ctx_t *dctx = (wechat_dl_ctx_t *)malloc(sizeof(*dctx));
        if (!dctx) {
            DiagPrintf("[wechat] OOM for dl_ctx\n");
            return;
        }
        strlcpy(dctx->aeskey_hex, item->image_aeskey
                ? item->image_aeskey : "", sizeof(dctx->aeskey_hex));

        cap_im_attachment_job_t job;
        _memset(&job, 0, sizeof(job));
        strlcpy(job.platform,       "wechat",         sizeof(job.platform));
        strlcpy(job.source_cap,     "cap_im_wechat",  sizeof(job.source_cap));
        strlcpy(job.source_channel, "wechat",         sizeof(job.source_channel));
        strlcpy(job.chat_id,        item->chat_id   ? item->chat_id   : "", sizeof(job.chat_id));
        strlcpy(job.sender_id,      item->sender_id ? item->sender_id : "", sizeof(job.sender_id));
        strlcpy(job.message_id,     item->msg_id    ? item->msg_id    : "", sizeof(job.message_id));
        strlcpy(job.media_kind,     "photo",          sizeof(job.media_kind));
        strlcpy(job.mime,           "image/jpeg",     sizeof(job.mime));
        job.download_url = item->image_url;  /* enqueue() strdups — no size limit */
        job.download_fn  = wechat_image_download_fn;
        job.download_ctx = dctx;   /* attachment task owns this after enqueue */

        if (cap_im_attachment_enqueue(&job) != RTK_SUCCESS) {
            free(dctx); /* enqueue failed, free ctx ourselves */
        }
    }
}

/* ---- Message poll task (runs after login) ---- */

static void wechat_msg_poll_task(void *param)
{
    int retry_count = 0;
    int my_gen;
    wechat_http_session_t *poll_session = NULL;
    (void)param;

    rtos_mutex_take(s.lock, RTOS_MAX_TIMEOUT);
    my_gen = s.poll_gen;
    rtos_mutex_give(s.lock);

    DiagPrintf("[wechat] Message poll started (gen=%d).\n", my_gen);

    /* Open a persistent TLS session shared across all getupdates calls so that
     * the TLS handshake only happens once (or on reconnect), not every 35 s. */
    poll_session = wechat_api_open_poll_session(&s.bot);

    while (1) {
        int ret = wechat_api_poll(&s.bot, on_item, NULL, poll_session);

        rtos_mutex_take(s.lock, RTOS_MAX_TIMEOUT);
        if (s.poll_gen != my_gen) {
            /* A new login was requested — exit without touching shared state */
            rtos_mutex_give(s.lock);
            break;
        }
        rtos_mutex_give(s.lock);

        if (ret != 0) {
            retry_count++;
            DiagPrintf("[wechat] poll error (%d), retry %d\n", ret, retry_count);
            if (retry_count > s.cfg.max_retry_count) {
                DiagPrintf("[wechat] Too many errors, clearing token.\n");
                rtos_mutex_take(s.lock, RTOS_MAX_TIMEOUT);
                if (s.poll_gen == my_gen) {
                    wechat_token_clear();
                    s.state = CAP_IM_WECHAT_STATE_ERROR;
                }
                rtos_mutex_give(s.lock);
                break;
            }
            rtos_time_delay_ms(s.cfg.poll_retry_delay_ms);
        } else {
            retry_count = 0;
        }
    }

    if (poll_session) {
        wechat_http_session_close(poll_session);
        poll_session = NULL;
    }

    rtos_mutex_take(s.lock, RTOS_MAX_TIMEOUT);
    if (s.poll_gen == my_gen)
        wechat_api_state_cleanup(&s.bot);
    rtos_mutex_give(s.lock);
    rtos_task_delete(NULL);
}

/* ---- QR confirmation poll task ---- */

static void wechat_qr_confirm_task(void *param)
{
    char qr_id[96];
    char host[128];
    wechat_http_session_t *session;
    int elapsed;
    int ttl = 5 * 60;
    (void)param;

    rtos_mutex_take(s.lock, RTOS_MAX_TIMEOUT);
    strncpy(qr_id, s.qr_id, sizeof(qr_id) - 1);
    qr_id[sizeof(qr_id) - 1] = '\0';
    get_host_from_url(s.bot.base_url, host, sizeof(host));
    rtos_mutex_give(s.lock);

    session = wechat_http_session_open(host);
    if (!session) {
        DiagPrintf("[wechat] QR confirm: failed to open session\n");
        rtos_mutex_take(s.lock, RTOS_MAX_TIMEOUT);
        s.state = CAP_IM_WECHAT_STATE_IDLE;
        rtos_mutex_give(s.lock);
        rtos_task_delete(NULL);
        return;
    }

    for (elapsed = 0; elapsed < ttl; elapsed++) {
        int rc;
        rtos_time_delay_ms(1000);

        rc = wechat_api_qr_poll_once(&s.bot, session, qr_id);

        if (rc == WECHAT_QR_CONFIRMED) {
            wechat_http_session_close(session);
            rtos_mutex_take(s.lock, RTOS_MAX_TIMEOUT);
            s.poll_gen++;
            s.state = CAP_IM_WECHAT_STATE_POLLING;
            rtos_mutex_give(s.lock);
            if (rtos_task_create(NULL, "wechat_msg", wechat_msg_poll_task,
                                 NULL, 9216, 1) != RTK_SUCCESS) {
                DiagPrintf("[wechat] failed to start msg poll task\n");
                rtos_mutex_take(s.lock, RTOS_MAX_TIMEOUT);
                s.state = CAP_IM_WECHAT_STATE_ERROR;
                rtos_mutex_give(s.lock);
            }
            rtos_task_delete(NULL);
            return;

        } else if (rc == WECHAT_QR_EXPIRED) {
            DiagPrintf("[wechat] QR expired\n");
            wechat_http_session_close(session);
            rtos_mutex_take(s.lock, RTOS_MAX_TIMEOUT);
            s.state = CAP_IM_WECHAT_STATE_IDLE;
            rtos_mutex_give(s.lock);
            rtos_task_delete(NULL);
            return;

        } else if (rc == WECHAT_QR_REDIRECTED) {
            /* base_url updated inside wechat_api_qr_poll_once; reopen session */
            DiagPrintf("[wechat] QR redirect, reopening session\n");
            wechat_http_session_close(session);
            rtos_mutex_take(s.lock, RTOS_MAX_TIMEOUT);
            get_host_from_url(s.bot.base_url, host, sizeof(host));
            rtos_mutex_give(s.lock);
            session = wechat_http_session_open(host);
            if (!session) {
                rtos_mutex_take(s.lock, RTOS_MAX_TIMEOUT);
                s.state = CAP_IM_WECHAT_STATE_IDLE;
                rtos_mutex_give(s.lock);
                rtos_task_delete(NULL);
                return;
            }
        }
        /* WAIT / SCANNED / error: keep polling */
    }

    DiagPrintf("[wechat] QR confirm timeout\n");
    wechat_http_session_close(session);
    rtos_mutex_take(s.lock, RTOS_MAX_TIMEOUT);
    s.state = CAP_IM_WECHAT_STATE_IDLE;
    rtos_mutex_give(s.lock);
    rtos_task_delete(NULL);
}

/* ---- Startup task (waits for WiFi, auto-login with saved token) ---- */

/* Called by claw_wifi_mgr when STA obtains an IP — replaces wechat_startup_task. */
static void wechat_on_wifi_connected(void)
{
    const claw_config_t *cfg;

    if (!s.initialized) return;

    DiagPrintf("[wechat] WiFi connected.\n");

    rtos_mutex_take(s.lock, RTOS_MAX_TIMEOUT);

    /* Already polling from a previous connection cycle — do nothing. */
    if (s.state == CAP_IM_WECHAT_STATE_POLLING) {
        rtos_mutex_give(s.lock);
        return;
    }

    wechat_api_state_init(&s.bot);
    cfg = claw_config_get();
    if (cfg->wechat.base_url[0])
        strncpy(s.bot.base_url, cfg->wechat.base_url, sizeof(s.bot.base_url) - 1);
    if (cfg->wechat.app_id[0])
        strncpy(s.bot.app_id, cfg->wechat.app_id, sizeof(s.bot.app_id) - 1);

    if (wechat_token_load(s.bot.token, sizeof(s.bot.token)) == 0) {
        DiagPrintf("[wechat] Loaded saved token, starting message poll.\n");
        s.poll_gen++;
        s.state = CAP_IM_WECHAT_STATE_POLLING;
        rtos_mutex_give(s.lock);
        if (rtos_task_create(NULL, "wechat_msg", wechat_msg_poll_task,
                             NULL, 9216, 1) != RTK_SUCCESS) {
            DiagPrintf("[wechat] failed to create msg poll task\n");
            rtos_mutex_take(s.lock, RTOS_MAX_TIMEOUT);
            s.state = CAP_IM_WECHAT_STATE_ERROR;
            rtos_mutex_give(s.lock);
        }
    } else {
        DiagPrintf("[wechat] No saved token, waiting for QR trigger via web UI.\n");
        rtos_mutex_give(s.lock);
    }
}

/* ---- Cap group ---- */

static int execute_wechat_send_text(const char *input_json,
                                     const claw_cap_call_context_t *ctx,
                                     char **output)
{
    (void)ctx;
    return claw_im_cap_execute_send_text(input_json, output, cap_im_wechat_send);
}

static int execute_wechat_status(const char *input_json,
                                  const claw_cap_call_context_t *ctx,
                                  char **output)
{
    (void)input_json;
    (void)ctx;
    char tmp[640];
    cap_im_wechat_get_status_json(tmp, sizeof(tmp));
    char *buf = strdup(tmp);
    if (!buf) {
        *output = NULL;
        return RTK_ERR_NOMEM;
    }
    *output = buf;
    return RTK_SUCCESS;
}

static const claw_cap_descriptor_t s_caps[] = {
    {
        .id          = "wechat_send_text",
        .name        = "wechat_send_text",
        .family      = "im_wechat",
        .description = "Send a WeChat message. Args: chat_id(string), text(string).",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"chat_id\":{\"type\":\"string\",\"description\":\"WeChat chat ID\"},"
            "\"text\":{\"type\":\"string\",\"description\":\"Message text to send\"}"
            "},"
            "\"required\":[\"chat_id\",\"text\"]}",
        .execute     = execute_wechat_send_text,
    },
    {
        .id          = "wechat_status",
        .name        = "wechat_status",
        .family      = "im_wechat",
        .description = "Query WeChat bot running status.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute     = execute_wechat_status,
    },
};

static const claw_cap_group_t s_group = {
    .group_id         = "im_wechat",
    .plugin_name      = "cap_im_wechat",
    .version          = "1",
    .descriptors      = s_caps,
    .descriptor_count = 2,
};

/* ---- Public API ---- */

int cap_im_wechat_init(const cap_im_wechat_config_t *cfg)
{
    _memset(&s, 0, sizeof(s));

    if (cfg) {
        s.cfg = *cfg;
    } else {
        s.cfg.poll_retry_delay_ms = 2000;
        s.cfg.wifi_timeout_ms     = 30000;
        s.cfg.max_retry_count     = 10;
    }

    if (rtos_mutex_create(&s.lock) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "failed to create mutex\n");
        return RTK_FAIL;
    }

    claw_cap_register_group(&s_group);
    /* SILENT_TRACE: suppress the [tool_calls] trace appended to final replies.
     * WeChat mobile rejects messages containing non-UTF-8 or control characters
     * that can appear in skill names inside the trace. */
    claw_im_dispatch_register_with_flags("wechat", cap_im_wechat_send,
        CLAW_IM_CHANNEL_FLAG_SILENT_TRACE, "wechat_send_text");

    s.initialized = 1;
    RTK_LOGI(TAG, "Initialized\n");
    return RTK_SUCCESS;
}

/* Find or allocate a slot for chat_id (must be called with s.lock held).
 * Returns the slot index; evicts the oldest on overflow. */
static int prog_slot(const char *chat_id)
{
    int i;

    for (i = 0; i < WECHAT_PROG_SLOTS; i++) {
        if (s.prog[i].chat_id[0] &&
                strcmp(s.prog[i].chat_id, chat_id) == 0)
            return i;
    }

    /* Not found — claim the next slot (circular eviction) */
    i = s.prog_next;
    s.prog_next = (s.prog_next + 1) % WECHAT_PROG_SLOTS;
    strlcpy(s.prog[i].chat_id, chat_id, sizeof(s.prog[i].chat_id));
    s.prog[i].count = 0;
    return i;
}

static void wechat_send_progress(const char *chat_id,
                                  const char *text,
                                  uint32_t    request_id)
{
    int action;  /* 0=send as-is, 1=send limit notice, -1=drop */
    (void)request_id;  /* window-based; request_id not needed */

    rtos_mutex_take(s.lock, RTOS_MAX_TIMEOUT);
    int idx = prog_slot(chat_id);
    s.prog[idx].count++;
    if (s.prog[idx].count <= WECHAT_PROG_LIMIT)
        action = 0;
    else if (s.prog[idx].count == WECHAT_PROG_LIMIT + 1)
        action = 1;
    else
        action = -1;
    rtos_mutex_give(s.lock);

    if (action < 0) return;

    /* Snapshot bot state under lock, then do HTTP outside the lock.
     * Holding s.lock across a synchronous HTTPS call (hundreds of ms) would
     * block wechat_msg_poll_task and other lock waiters for the full RTT. */
    rtos_mutex_take(s.lock, RTOS_MAX_TIMEOUT);
    wechat_bot_state_t bot_snap = s.bot;
    rtos_mutex_give(s.lock);

    const char *send_text = (action == 1)
        ? "(rate limit reached, further updates suppressed — final reply coming)"
        : text;
    int rc = wechat_api_send_text(&bot_snap, chat_id, send_text);
    if (rc == -6)
        RTK_LOGE(TAG, "progress rate-limited by platform\n");
}

int cap_im_wechat_start(void)
{
    claw_wifi_mgr_register_on_connected(wechat_on_wifi_connected);
    claw_im_dispatch_register_progress("wechat", wechat_send_progress);
    return RTK_SUCCESS;
}

void cap_im_wechat_send(const char *chat_id, const char *text)
{
    if (!s.initialized) return;
    rtos_mutex_take(s.lock, RTOS_MAX_TIMEOUT);

    /* Reset our local per-turn counter so the next turn starts fresh. */
    if (chat_id && chat_id[0]) {
        int idx = prog_slot(chat_id);
        s.prog[idx].count = 0;
    }

    char *truncated = NULL;
    const char *send_text = text;
    if (text && strlen(text) > WECHAT_MAX_MSG_BYTES) {
        truncated = (char *)malloc(WECHAT_MAX_MSG_BYTES + 32);
        if (truncated) {
            /* Copy up to limit, find last UTF-8 boundary, append ellipsis */
            size_t len = WECHAT_MAX_MSG_BYTES;
            while (len > 0 && (text[len] & 0xC0) == 0x80) len--;
            memcpy(truncated, text, len);
            strlcpy(truncated + len, "…（消息过长已截断）", 32);
            send_text = truncated;
            RTK_LOGW(TAG, "reply truncated from %u to %u bytes for mobile\n",
                     (unsigned)strlen(text), (unsigned)len);
        }
    }

    int rc = wechat_api_send_text(&s.bot, chat_id, send_text);
    if (rc == -6) {
        RTK_LOGW(TAG, "final reply rate-limited, retrying in 5 s\n");
        rtos_mutex_give(s.lock);
        rtos_time_delay_ms(5000);
        rtos_mutex_take(s.lock, RTOS_MAX_TIMEOUT);
        rc = wechat_api_send_text(&s.bot, chat_id, send_text);
        if (rc != 0)
            RTK_LOGE(TAG, "final reply retry failed rc=%d\n", rc);
        else
            RTK_LOGI(TAG, "final reply retry succeeded\n");
    } else if (rc != 0) {
        RTK_LOGE(TAG, "send failed rc=%d\n", rc);
    }
    free(truncated);  /* free after all sends complete; send_text may point into truncated */
    rtos_mutex_give(s.lock);
}

int cap_im_wechat_get_qr(char *qr_url, size_t size)
{
    char local_url[512];
    char local_id[96];
    const claw_config_t *cfg;
    int rc;

    if (!s.initialized) return -1;
    if (claw_wifi_mgr_get_state() != CLAW_WIFI_STATE_CONNECTED) return -4;

    rtos_mutex_take(s.lock, RTOS_MAX_TIMEOUT);

    if (s.state == CAP_IM_WECHAT_STATE_POLLING) {
        /* Allow re-login: invalidate the current session and fall through to QR fetch */
        DiagPrintf("[wechat] Re-login requested, stopping current polling session (gen %d).\n", s.poll_gen);
        wechat_token_clear();
        s.poll_gen++;   /* signals wechat_msg_poll_task to exit cleanly */
        s.state = CAP_IM_WECHAT_STATE_IDLE;
    }

    if (s.state == CAP_IM_WECHAT_STATE_QR_PENDING) {
        if (qr_url) {
            strncpy(qr_url, s.qr_url, size - 1);
            qr_url[size - 1] = '\0';
        }
        rtos_mutex_give(s.lock);
        return 0;
    }

    /* IDLE or ERROR: (re-)initialise bot state from current config */
    wechat_api_state_init(&s.bot);
    cfg = claw_config_get();
    if (cfg->wechat.base_url[0])
        strncpy(s.bot.base_url, cfg->wechat.base_url, sizeof(s.bot.base_url) - 1);
    if (cfg->wechat.app_id[0])
        strncpy(s.bot.app_id, cfg->wechat.app_id, sizeof(s.bot.app_id) - 1);

    rtos_mutex_give(s.lock);

    /* Fetch QR code (network call, no mutex held) */
    rc = wechat_api_qr_fetch(&s.bot, local_url, sizeof(local_url),
                              local_id, sizeof(local_id));
    if (rc != 0) {
        DiagPrintf("[wechat] QR fetch failed: %d\n", rc);
        return -3;
    }

    rtos_mutex_take(s.lock, RTOS_MAX_TIMEOUT);
    strncpy(s.qr_url, local_url, sizeof(s.qr_url) - 1);
    s.qr_url[sizeof(s.qr_url) - 1] = '\0';
    strncpy(s.qr_id, local_id, sizeof(s.qr_id) - 1);
    s.qr_id[sizeof(s.qr_id) - 1] = '\0';
    s.state = CAP_IM_WECHAT_STATE_QR_PENDING;
    rtos_mutex_give(s.lock);

    if (qr_url) {
        strncpy(qr_url, local_url, size - 1);
        qr_url[size - 1] = '\0';
    }

    if (rtos_task_create(NULL, "wechat_qr", wechat_qr_confirm_task,
                         NULL, 9216, 1) != RTK_SUCCESS) {
        DiagPrintf("[wechat] failed to spawn QR confirm task\n");
        rtos_mutex_take(s.lock, RTOS_MAX_TIMEOUT);
        s.state = CAP_IM_WECHAT_STATE_IDLE;
        rtos_mutex_give(s.lock);
        return -3;
    }

    return 0;
}

int cap_im_wechat_store_token(const char *token)
{
    if (!token || !token[0])
        return wechat_token_clear();
    return wechat_token_save(token);
}

int cap_im_wechat_get_token(char *buf, size_t size)
{
    if (!buf || size == 0) return -1;
    return wechat_token_load(buf, size);
}

void cap_im_wechat_get_status_json(char *buf, size_t buf_size)
{
    const char *state_str;
    char qr_url_copy[512];
    cap_im_wechat_state_t cur_state;

    rtos_mutex_take(s.lock, RTOS_MAX_TIMEOUT);
    cur_state = s.state;
    strncpy(qr_url_copy, s.qr_url, sizeof(qr_url_copy) - 1);
    qr_url_copy[sizeof(qr_url_copy) - 1] = '\0';
    rtos_mutex_give(s.lock);

    switch (cur_state) {
    case CAP_IM_WECHAT_STATE_QR_PENDING: state_str = "qr_pending"; break;
    case CAP_IM_WECHAT_STATE_POLLING:    state_str = "polling";    break;
    case CAP_IM_WECHAT_STATE_ERROR:      state_str = "error";      break;
    default:                              state_str = "idle";       break;
    }

    if (cur_state == CAP_IM_WECHAT_STATE_QR_PENDING) {
        DiagSnPrintf(buf, buf_size,
                 "{\"state\":\"%s\",\"qr_url\":\"%s\"}", state_str, qr_url_copy);
    } else {
        DiagSnPrintf(buf, buf_size, "{\"state\":\"%s\"}", state_str);
    }
}

/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_im_telegram.h"
#include "cap_im_attachment.h"
#include "claw_cap.h"
#include "claw_compat.h"
#include "claw_config.h"
#include "claw_wifi_mgr.h"
#include "claw_event_publisher.h"
#include "claw_im_dispatch.h"
#include "llm_agent_http.h"
#include "os_wrapper.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "cap_im_telegram";

/* ---- Static runtime state ---- */

static struct {
    long  poll_timeout_sec;
    long  last_update_id;
    int   running;
    rtos_task_t task;
} s;

/* ---- Send message ---- */

void cap_im_telegram_send(const char *chat_id, const char *text)
{
    if (!chat_id || !text) return;
    const char *token = claw_config_get()->telegram.bot_token;
    if (token[0] == '\0') {
        RTK_LOGW(TAG, "bot_token not set, skipping send\n");
        return;
    }

    char resource[192];
    DiagSnPrintf(resource, sizeof(resource), "/bot%s/sendMessage", token);

    cJSON *body = cJSON_CreateObject();
    if (!body) return;
    cJSON_AddStringToObject(body, "chat_id", chat_id);
    cJSON_AddStringToObject(body, "text", text);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) return;

    llm_http_resp_t resp;
    if (llm_http_resp_init(&resp) != 0) {
        free(body_str);
        return;
    }

    int ret = llm_http_post_no_auth("api.telegram.org", resource,
                                    body_str, strlen(body_str), &resp);
    if (ret != 0) {
        RTK_LOGE(TAG, "sendMessage failed (ret=%d)\n", ret);
    }

    llm_http_resp_free(&resp);
    free(body_str);
}

/* ---- Cap execute: telegram_send_text ---- */

static int cap_telegram_send_text_execute(const char *input_json,
                                           const claw_cap_call_context_t *ctx,
                                           char **output)
{
    (void)ctx;
    return claw_im_cap_execute_send_text(input_json, output, cap_im_telegram_send);
}

/* ---- Long-polling task ---- */

static void telegram_poll_task(void *arg)
{
    (void)arg;
    RTK_LOGI(TAG, "Telegram poll task started\n");

    llm_http_session_t *session = NULL;

    /* Task only starts when token is configured — no need to re-check inside loop */
    while (s.running) {
        const char *token = claw_config_get()->telegram.bot_token;

        /* Open persistent session on first use (or after failure) */
        if (!session) {
            session = llm_http_session_open("api.telegram.org");
            if (!session) {
                rtos_time_delay_ms(5000);
                continue;
            }
        }

        char resource[192];
        DiagSnPrintf(resource, sizeof(resource), "/bot%s/getUpdates", token);

        cJSON *req = cJSON_CreateObject();
        if (!req) {
            rtos_time_delay_ms(5000);
            continue;
        }
        cJSON_AddNumberToObject(req, "timeout", (double)s.poll_timeout_sec);
        cJSON_AddNumberToObject(req, "offset",  (double)(s.last_update_id + 1));
        char *req_str = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);
        if (!req_str) {
            rtos_time_delay_ms(5000);
            continue;
        }

        llm_http_resp_t resp;
        if (llm_http_resp_init(&resp) != 0) {
            free(req_str);
            rtos_time_delay_ms(5000);
            continue;
        }

        int ret = llm_http_session_post_no_auth(session, resource,
                                                req_str, strlen(req_str), &resp);
        free(req_str);

        if (ret == 0 && resp.len > 0) {
            cJSON *root = cJSON_Parse(resp.buf);
            if (root) {
                cJSON *ok_j     = cJSON_GetObjectItem(root, "ok");
                cJSON *results  = cJSON_GetObjectItem(root, "result");
                if (cJSON_IsTrue(ok_j) && cJSON_IsArray(results)) {
                    cJSON *update;
                    cJSON_ArrayForEach(update, results) {
                        cJSON *uid_j = cJSON_GetObjectItem(update, "update_id");
                        if (uid_j && cJSON_IsNumber(uid_j)) {
                            s.last_update_id = (long)uid_j->valuedouble;
                        }

                        cJSON *msg = cJSON_GetObjectItem(update, "message");
                        if (!msg) continue;

                        cJSON *chat   = cJSON_GetObjectItem(msg, "chat");
                        cJSON *from   = cJSON_GetObjectItem(msg, "from");
                        cJSON *text_j = cJSON_GetObjectItem(msg, "text");

                        if (!chat) continue;

                        /* chat.id is a number; convert to string */
                        cJSON *chat_id_j = cJSON_GetObjectItem(chat, "id");
                        if (!chat_id_j || !cJSON_IsNumber(chat_id_j)) continue;

                        char chat_id_str[32];
                        DiagSnPrintf(chat_id_str, sizeof(chat_id_str), "%ld",
                                 (long)chat_id_j->valuedouble);

                        /* message_id */
                        char msg_id_str[32] = "";
                        cJSON *mid_j = cJSON_GetObjectItem(msg, "message_id");
                        if (mid_j && cJSON_IsNumber(mid_j)) {
                            DiagSnPrintf(msg_id_str, sizeof(msg_id_str), "%ld",
                                         (long)mid_j->valuedouble);
                        }

                        char sender_str[64] = "";
                        char sender_id_str[32] = "";
                        if (from) {
                            cJSON *fn = cJSON_GetObjectItem(from, "first_name");
                            if (fn && cJSON_IsString(fn)) {
                                strlcpy(sender_str, fn->valuestring, sizeof(sender_str));
                            }
                            cJSON *fid = cJSON_GetObjectItem(from, "id");
                            if (fid && cJSON_IsNumber(fid)) {
                                DiagSnPrintf(sender_id_str, sizeof(sender_id_str),
                                             "%ld", (long)fid->valuedouble);
                            }
                        }

                        /* ---- Attachment detection ---- */
                        /* caption is shared across photo/document/voice/video */
                        char caption_str[256] = "";
                        cJSON *cap_j = cJSON_GetObjectItem(msg, "caption");
                        if (cap_j && cJSON_IsString(cap_j)) {
                            strlcpy(caption_str, cap_j->valuestring, sizeof(caption_str));
                        }

                        /* Helper: resolve a file_id → download URL via getFile API */
                        /* We inline the file_id → path resolution in the enqueue step below */

                        /* Check each media type in priority order */
                        static const struct {
                            const char *key;       /* JSON field in message */
                            const char *kind;      /* cap_im_attachment kind */
                            const char *mime_hint; /* fallback MIME */
                        } s_media_types[] = {
                            { "photo",   "photo",    "image/jpeg" },
                            { "document","document", ""           },
                            { "voice",   "voice",    "audio/ogg"  },
                            { "video",   "video",    "video/mp4"  },
                            { "audio",   "voice",    "audio/mp3"  },
                        };
                        for (int mt = 0;
                             mt < (int)(sizeof(s_media_types)/sizeof(s_media_types[0]));
                             mt++) {
                            cJSON *med = cJSON_GetObjectItem(msg, s_media_types[mt].key);
                            if (!med) continue;

                            /* photo is an array — use the last (highest-res) entry */
                            const char *file_id = NULL;
                            const char *orig_fname = "";
                            const char *mime_hint = s_media_types[mt].mime_hint;

                            if (cJSON_IsArray(med)) {
                                cJSON *last = cJSON_GetArrayItem(med,
                                    cJSON_GetArraySize(med) - 1);
                                if (last) {
                                    cJSON *fi = cJSON_GetObjectItem(last, "file_id");
                                    if (fi && cJSON_IsString(fi)) file_id = fi->valuestring;
                                }
                            } else if (cJSON_IsObject(med)) {
                                cJSON *fi = cJSON_GetObjectItem(med, "file_id");
                                if (fi && cJSON_IsString(fi)) file_id = fi->valuestring;
                                cJSON *fn = cJSON_GetObjectItem(med, "file_name");
                                if (fn && cJSON_IsString(fn)) orig_fname = fn->valuestring;
                                cJSON *mt2 = cJSON_GetObjectItem(med, "mime_type");
                                if (mt2 && cJSON_IsString(mt2)) mime_hint = mt2->valuestring;
                            }

                            if (!file_id) continue;

                            /* Resolve file_id → file_path via getFile, then build URL */
                            /* We do this inline (synchronous) in the poll task.
                             * getFile is a cheap API call (~100 ms), acceptable here. */
                            const char *token = claw_config_get()->telegram.bot_token;
                            char get_file_res[192];
                            DiagSnPrintf(get_file_res, sizeof(get_file_res),
                                         "/bot%s/getFile", token);
                            char gf_body[128];
                            DiagSnPrintf(gf_body, sizeof(gf_body),
                                         "{\"file_id\":\"%s\"}", file_id);
                            llm_http_resp_t gf_resp;
                            char dl_url[384] = "";
                            if (llm_http_resp_init(&gf_resp) == 0) {
                                if (llm_http_post_no_auth("api.telegram.org",
                                        get_file_res, gf_body, strlen(gf_body),
                                        &gf_resp) == 0 && gf_resp.len > 0) {
                                    cJSON *gr = cJSON_Parse(gf_resp.buf);
                                    if (gr) {
                                        cJSON *res = cJSON_GetObjectItem(gr, "result");
                                        cJSON *fp2 = res ?
                                            cJSON_GetObjectItem(res, "file_path") : NULL;
                                        if (fp2 && cJSON_IsString(fp2)) {
                                            DiagSnPrintf(dl_url, sizeof(dl_url),
                                                "https://api.telegram.org/file/bot%s/%s",
                                                token, fp2->valuestring);
                                        }
                                        cJSON_Delete(gr);
                                    }
                                }
                                llm_http_resp_free(&gf_resp);
                            }

                            if (!dl_url[0]) {
                                RTK_LOGW(TAG, "getFile failed for file_id %s\n", file_id);
                                break; /* skip this attachment */
                            }

                            cap_im_attachment_job_t job;
                            _memset(&job, 0, sizeof(job));
                            strlcpy(job.platform,       "telegram",         sizeof(job.platform));
                            strlcpy(job.source_cap,     "cap_im_telegram",  sizeof(job.source_cap));
                            strlcpy(job.source_channel, "telegram",         sizeof(job.source_channel));
                            strlcpy(job.chat_id,        chat_id_str,        sizeof(job.chat_id));
                            strlcpy(job.sender_id,      sender_id_str,      sizeof(job.sender_id));
                            strlcpy(job.message_id,     msg_id_str,         sizeof(job.message_id));
                            strlcpy(job.media_kind,     s_media_types[mt].kind, sizeof(job.media_kind));
                            strlcpy(job.caption,        caption_str,        sizeof(job.caption));
                            job.download_url = dl_url;  /* enqueue() strdups — no size limit */
                            strlcpy(job.original_filename, orig_fname,      sizeof(job.original_filename));
                            strlcpy(job.mime,           mime_hint,          sizeof(job.mime));
                            job.download_fn  = NULL; /* plain HTTPS GET */
                            job.download_ctx = NULL;

                            cap_im_attachment_enqueue(&job);
                            break; /* one attachment type per message */
                        }

                        /* ---- Text message ---- */
                        if (!cJSON_IsString(text_j)) continue;

                        RTK_LOGI(TAG, "msg from %s chat=%s: %.60s\n",
                                 sender_str[0] ? sender_str : "?",
                                 chat_id_str, text_j->valuestring);

                        claw_event_dispatcher_publish_message(
                            "cap_im_telegram", "telegram",
                            chat_id_str,
                            text_j->valuestring,
                            sender_str[0] ? sender_str : NULL,
                            NULL);
                    }
                }
                cJSON_Delete(root);
            }
        }

        llm_http_resp_free(&resp);

        if (ret != 0) {
            RTK_LOGW(TAG, "getUpdates failed (ret=%d), retrying in 5s\n", ret);
            /* Drop the session so it is re-established on next iteration */
            llm_http_session_close(session);
            session = NULL;
            rtos_time_delay_ms(5000);
        }
        /* On success with long-poll, loop immediately for next poll cycle */
    }

if (session) llm_http_session_close(session);
    rtos_task_delete(NULL);
}

/* ---- Cap group descriptors ---- */

static const claw_cap_descriptor_t s_telegram_caps[] = {
    {
        .id          = "telegram_send_text",
        .name        = "telegram_send_text",
        .family      = "im_telegram",
        .description = "Send a Telegram message. Requires chat_id and text fields.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"chat_id\":{\"type\":\"string\",\"description\":\"Telegram chat ID (numeric string)\"},"
            "\"text\":{\"type\":\"string\",\"description\":\"Message text to send\"}"
            "},"
            "\"required\":[\"chat_id\",\"text\"]}",
        .execute = cap_telegram_send_text_execute,
    },
};

static const claw_cap_group_t s_telegram_group = {
    .group_id         = "im_telegram",
    .plugin_name      = "cap_im_telegram",
    .version          = "1",
    .descriptors      = s_telegram_caps,
    .descriptor_count = 1,
};

/* ---- Public API ---- */

/* Forward declarations */
static void telegram_on_config_saved(void);
static void telegram_on_wifi_connected(void);
static void telegram_try_start(void);

int cap_im_telegram_init(const cap_im_telegram_config_t *config)
{
    if (!config) return RTK_ERR_BADARG;

    _memset(&s, 0, sizeof(s));

    s.poll_timeout_sec = (config->poll_timeout_sec > 0) ? config->poll_timeout_sec : 25;
    s.last_update_id   = 0;
    s.running          = 0;
    s.task             = NULL;

    int err = claw_cap_register_group(&s_telegram_group);
    if (err != RTK_SUCCESS) {
        RTK_LOGE(TAG, "claw_cap_register_group failed: %d\n", err);
        return err;
    }

    claw_im_dispatch_register_with_flags("telegram", cap_im_telegram_send, 0, "telegram_send_text");
    claw_im_dispatch_register_media("telegram", cap_im_telegram_send_media);
    claw_config_register_on_save(telegram_on_config_saved);

    RTK_LOGI(TAG, "Initialized\n");
    return RTK_SUCCESS;
}

int cap_im_telegram_start(void)
{
    claw_wifi_mgr_register_on_connected(telegram_on_wifi_connected);
    return RTK_SUCCESS;
}

/* Start poll task only when BOTH token AND WiFi are ready. */
static void telegram_try_start(void)
{
    if (s.task) return;
    if (claw_config_get()->telegram.bot_token[0] == '\0') return;
    if (claw_wifi_mgr_get_state() != CLAW_WIFI_STATE_CONNECTED) return;

    s.running = 1;
    if (rtos_task_create(&s.task, "tg_poll", telegram_poll_task, NULL, 8192, 1) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "rtos_task_create failed\n");
        s.running = 0;
        return;
    }
    RTK_LOGI(TAG, "Poll task started (timeout=%lds)\n", s.poll_timeout_sec);
}

static void telegram_on_config_saved(void)
{
    telegram_try_start();
}

static void telegram_on_wifi_connected(void)
{
    telegram_try_start();
}

/* ---- Send media ---------------------------------------------------------- */

int cap_im_telegram_send_media(const char *chat_id,
                                const char *vfs_path,
                                const char *caption,
                                const char *media_kind)
{
    if (!chat_id || !vfs_path) return -1;

    const char *token = claw_config_get()->telegram.bot_token;
    if (token[0] == '\0') {
        RTK_LOGW(TAG, "send_media: bot_token not set\n");
        return -1;
    }

    /* Determine media kind from extension if not provided */
    const char *kind = media_kind;
    if (!kind || !kind[0]) {
        const char *dot = strrchr(vfs_path, '.');
        if (dot) {
            const char *ext = dot + 1;
            if (strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0 ||
                strcmp(ext, "png") == 0 || strcmp(ext, "gif")  == 0 ||
                strcmp(ext, "webp") == 0) {
                kind = "photo";
            } else if (strcmp(ext, "mp3") == 0 || strcmp(ext, "ogg") == 0 ||
                       strcmp(ext, "wav") == 0 || strcmp(ext, "aac") == 0 ||
                       strcmp(ext, "m4a") == 0 || strcmp(ext, "amr") == 0) {
                kind = "voice";
            } else if (strcmp(ext, "mp4") == 0 || strcmp(ext, "avi") == 0 ||
                       strcmp(ext, "mov") == 0) {
                kind = "video";
            } else {
                kind = "document";
            }
        } else {
            kind = "document";
        }
    }

    /* Map kind to Telegram API endpoint */
    const char *api_method;
    const char *form_field;
    if (strcmp(kind, "photo") == 0) {
        api_method = "sendPhoto";    form_field = "photo";
    } else if (strcmp(kind, "voice") == 0) {
        api_method = "sendVoice";    form_field = "voice";
    } else if (strcmp(kind, "video") == 0) {
        api_method = "sendVideo";    form_field = "video";
    } else {
        api_method = "sendDocument"; form_field = "document";
    }

    /* Get file size */
    struct stat st;
    if (stat(vfs_path, &st) != 0) {
        RTK_LOGE(TAG, "send_media: stat failed for %s\n", vfs_path);
        return -1;
    }
    size_t file_size = (size_t)st.st_size;

    /* Extract filename from path */
    const char *fname = strrchr(vfs_path, '/');
    fname = fname ? fname + 1 : vfs_path;

    /* Build API resource */
    char resource[192];
    DiagSnPrintf(resource, sizeof(resource), "/bot%s/%s", token, api_method);

    /* Build multipart preamble:
     *   --ameba_claw_boundary\r\n
     *   Content-Disposition: form-data; name="chat_id"\r\n\r\n
     *   <chat_id>\r\n
     *   [--ameba_claw_boundary\r\n
     *   Content-Disposition: form-data; name="caption"\r\n\r\n
     *   <caption>\r\n]
     *   --ameba_claw_boundary\r\n
     *   Content-Disposition: form-data; name="<field>"; filename="<fname>"\r\n
     *   Content-Type: application/octet-stream\r\n\r\n
     */
    char preamble[768];
    int  pre_len;
    if (caption && caption[0]) {
        pre_len = DiagSnPrintf(preamble, sizeof(preamble),
            "--ameba_claw_boundary\r\n"
            "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n"
            "%s\r\n"
            "--ameba_claw_boundary\r\n"
            "Content-Disposition: form-data; name=\"caption\"\r\n\r\n"
            "%s\r\n"
            "--ameba_claw_boundary\r\n"
            "Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
            "Content-Type: application/octet-stream\r\n\r\n",
            chat_id, caption, form_field, fname);
    } else {
        pre_len = DiagSnPrintf(preamble, sizeof(preamble),
            "--ameba_claw_boundary\r\n"
            "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n"
            "%s\r\n"
            "--ameba_claw_boundary\r\n"
            "Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
            "Content-Type: application/octet-stream\r\n\r\n",
            chat_id, form_field, fname);
    }
    if (pre_len < 0 || pre_len >= (int)sizeof(preamble)) {
        RTK_LOGE(TAG, "send_media: preamble overflow\n");
        return -1;
    }

    /* Closing boundary */
    const char *suffix = "\r\n--ameba_claw_boundary--\r\n";
    size_t suffix_len  = strlen(suffix);

    llm_http_resp_t resp;
    if (llm_http_resp_init(&resp) != 0) return -1;

    int ret = llm_http_post_multipart_file(
        "api.telegram.org", resource,
        preamble, (size_t)pre_len,
        vfs_path, file_size,
        suffix, suffix_len,
        &resp);

    if (ret != 0) {
        RTK_LOGE(TAG, "send_media: upload failed (ret=%d)\n", ret);
    } else {
        RTK_LOGI(TAG, "send_media: %s sent to %s\n", vfs_path, chat_id);
    }
    llm_http_resp_free(&resp);
    return ret;
}


/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_im_attachment.h"
#include "claw_cap.h"
#include "claw_im_dispatch.h"
#include "claw_event_publisher.h"
#include "llm_agent_http.h"
#include "os_wrapper.h"
#include "platform_stdlib.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TAG "cap_im_attach"

/* Queue depth: at most 8 outstanding download jobs across all channels. */
#define ATTACHMENT_QUEUE_DEPTH  8
/* Download task stack — needs TLS stack for llm_http_get_to_file. */
#define ATTACHMENT_TASK_STACK   (8 * 1024)

/* Internal job copy stored in the queue (heap-allocated in enqueue). */
typedef cap_im_attachment_job_t attach_job_t;

static struct {
    rtos_queue_t queue;
    rtos_task_t  task;
    int          running;
} s;

/* ---- MIME / extension helpers ------------------------------------------- */

const char *cap_im_attachment_ext_from_mime(const char *mime)
{
    if (!mime || !mime[0]) return ".bin";
    if (strncmp(mime, "image/jpeg", 10) == 0 ||
        strncmp(mime, "image/jpg",   9) == 0)  return ".jpg";
    if (strncmp(mime, "image/png",   9) == 0)  return ".png";
    if (strncmp(mime, "image/gif",   9) == 0)  return ".gif";
    if (strncmp(mime, "image/webp", 10) == 0)  return ".webp";
    if (strncmp(mime, "audio/mpeg",  10) == 0 ||
        strncmp(mime, "audio/mp3",    9) == 0)  return ".mp3";
    if (strncmp(mime, "audio/wav",    9) == 0 ||
        strncmp(mime, "audio/x-wav", 11) == 0)  return ".wav";
    if (strncmp(mime, "audio/ogg",    9) == 0)  return ".ogg";
    if (strncmp(mime, "audio/aac",    9) == 0)  return ".aac";
    if (strncmp(mime, "audio/amr",    9) == 0)  return ".amr";
    if (strncmp(mime, "audio/silk",  10) == 0)  return ".silk";
    if (strncmp(mime, "audio/mp4",    9) == 0 ||
        strncmp(mime, "audio/m4a",    9) == 0)  return ".m4a";
    if (strncmp(mime, "video/mp4",    9) == 0)  return ".mp4";
    if (strncmp(mime, "video/",       6) == 0)  return ".mp4";
    if (strncmp(mime, "application/pdf",   15) == 0) return ".pdf";
    if (strncmp(mime, "text/plain",        10) == 0) return ".txt";
    return ".bin";
}

const char *cap_im_attachment_guess_ext(const char *url_or_filename,
                                         const char *mime)
{
    /* Try extension from URL/filename first */
    if (url_or_filename && url_or_filename[0]) {
        /* Strip query string */
        const char *q = strchr(url_or_filename, '?');
        size_t len = q ? (size_t)(q - url_or_filename) : strlen(url_or_filename);
        /* Find last dot after last slash */
        const char *dot = NULL;
        for (size_t i = 0; i < len; i++) {
            if (url_or_filename[i] == '.') dot = url_or_filename + i;
            if (url_or_filename[i] == '/') dot = NULL;
        }
        if (dot && dot[1] != '\0') {
            /* Reasonable extension: ≤ 5 chars, all alnum */
            size_t elen = (q ? (size_t)(q - dot) : strlen(dot));
            if (elen >= 2 && elen <= 6) {
                /* Check all chars after dot are alnum */
                int ok = 1;
                for (size_t i = 1; i < elen; i++) {
                    if (!( (dot[i] >= 'a' && dot[i] <= 'z') ||
                           (dot[i] >= 'A' && dot[i] <= 'Z') ||
                           (dot[i] >= '0' && dot[i] <= '9') )) {
                        ok = 0; break;
                    }
                }
                if (ok) return dot; /* points into url_or_filename — valid for duration of job */
            }
        }
    }
    return cap_im_attachment_ext_from_mime(mime);
}

/* ---- Path builder -------------------------------------------------------- */

/* Build: CAP_IM_ATTACHMENT_ROOT/<platform>/<safe_chat_id>/<platform>_<hash>_<kind><ext>
 * safe_chat_id replaces '/' and ':' with '_' to be FS-safe. */
static void build_dest_path(const attach_job_t *job,
                             const char *ext,
                             char *out_dir,  size_t dir_sz,
                             char *out_path, size_t path_sz)
{
    /* Sanitize chat_id for use as directory name */
    char safe_cid[64];
    strlcpy(safe_cid, job->chat_id, sizeof(safe_cid));
    for (char *p = safe_cid; *p; p++) {
        if (*p == '/' || *p == ':' || *p == '\\' || *p == ' ') *p = '_';
    }

    /* FNV-1a 32-bit hash of message_id for unique filename */
    uint32_t hash = 2166136261u;
    for (const char *p = job->message_id; *p; p++) {
        hash ^= (uint8_t)*p;
        hash *= 16777619u;
    }
    /* also mix in media_kind to distinguish multiple attachments in one message */
    for (const char *p = job->media_kind; *p; p++) {
        hash ^= (uint8_t)*p;
        hash *= 16777619u;
    }

    DiagSnPrintf(out_dir, dir_sz, "%s/%s/%s",
                 CAP_IM_ATTACHMENT_ROOT, job->platform, safe_cid);
    DiagSnPrintf(out_path, path_sz, "%s/%s_%08x_%s%s",
                 out_dir, job->platform, (unsigned)hash, job->media_kind, ext);
}

/* ---- Download task ------------------------------------------------------- */

static void attachment_task(void *arg)
{
    (void)arg;
    RTK_LOGI(TAG, "attachment task started\n");

    while (s.running) {
        attach_job_t *job = NULL;
        /* Block indefinitely waiting for next job */
        if (rtos_queue_receive(s.queue, &job, RTOS_MAX_DELAY) != RTK_SUCCESS || !job) {
            continue;
        }

        RTK_LOGI(TAG, "download: platform=%s kind=%s chat=%s\n",
                 job->platform, job->media_kind, job->chat_id);

        /* Determine file extension */
        const char *ext = cap_im_attachment_guess_ext(
            job->original_filename[0] ? job->original_filename : job->download_url,
            job->mime);

        char dir_path[192];
        char dest_path[256];
        build_dest_path(job, ext, dir_path, sizeof(dir_path), dest_path, sizeof(dest_path));

        /* Create directory hierarchy (best-effort, ignore EEXIST) */
        {
            /* mkdir VFS root first, then subdirs */
            char tmp[192];
            strlcpy(tmp, dir_path + 4, sizeof(tmp)); /* skip "vfs:" */
            /* Walk and mkdir each component */
            for (char *p = tmp + 1; *p; p++) {
                if (*p == '/') {
                    *p = '\0';
                    char full[200];
                    DiagSnPrintf(full, sizeof(full), "vfs:%s", tmp);
                    mkdir(full, 0777);
                    *p = '/';
                }
            }
            mkdir(dir_path, 0777);
        }

        /* Download */
        size_t bytes_written = 0;
        int    ret;
        if (job->download_fn) {
            /* Platform-specific download (e.g. WeChat decrypt) */
            ret = job->download_fn(job->download_url, dest_path, job->download_ctx);
            /* download_ctx was heap-allocated by the caller (e.g. wechat_dl_ctx_t);
             * the attachment task owns it from enqueue onward — free after use. */
            free(job->download_ctx);
            job->download_ctx = NULL;
            if (ret == 0) {
                struct stat st;
                if (stat(dest_path, &st) == 0) bytes_written = (size_t)st.st_size;
            }
        } else {
            /* Default: split https://<host><resource> and call get_to_file */
            const char *url = job->download_url;
            const char *host_start = url;
            if (strncmp(url, "https://", 8) == 0)      host_start = url + 8;
            else if (strncmp(url, "http://", 7) == 0)  host_start = url + 7;

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

            ret = llm_http_get_to_file(
                    host_buf,
                    path_start,
                    dest_path,
                    CAP_IM_ATTACHMENT_MAX_BYTES,
                    &bytes_written);
        }

        /* Treat 0-byte result as failure — HTTP 4xx/5xx returns Content-Length:0 */
        if (ret != 0 || bytes_written == 0) {
            RTK_LOGE(TAG, "download failed (ret=%d, bytes=%u): %.80s\n",
                     ret, (unsigned)bytes_written,
                     job->download_url ? job->download_url : "(null)");
            free((void *)job->download_url);
            free(job);
            continue;
        }

        RTK_LOGI(TAG, "saved: %s (%u bytes)\n", dest_path, (unsigned)bytes_written);

        /* Publish as a normal message event so the LLM sees the attachment
         * in context without special routing.  Text summarises the file. */
        char text_buf[320];
        if (job->caption[0]) {
            DiagSnPrintf(text_buf, sizeof(text_buf),
                         "[attachment:%s] %s (%u KB) — %s",
                         job->media_kind, dest_path,
                         (unsigned)((bytes_written + 512) / 1024),
                         job->caption);
        } else {
            DiagSnPrintf(text_buf, sizeof(text_buf),
                         "[attachment:%s] %s (%u KB)",
                         job->media_kind, dest_path,
                         (unsigned)((bytes_written + 512) / 1024));
        }

        claw_event_dispatcher_publish_message(
            job->source_cap,
            job->source_channel,
            job->chat_id,
            text_buf,
            job->sender_id[0] ? job->sender_id : NULL,
            job->message_id[0] ? job->message_id : NULL);

        free((void *)job->download_url);
        free(job);
    }

    rtos_task_delete(NULL);
}

/* ---- LLM tool: im_send_media -------------------------------------------- */

static int cap_im_send_media_execute(const char *input_json,
                                      const claw_cap_call_context_t *ctx,
                                      char **output)
{
    (void)ctx;
    if (!input_json) {
        claw_cap_set_output(output, "{\"error\":\"missing input\"}");
        return RTK_ERR_BADARG;
    }

    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        claw_cap_set_output(output, "{\"error\":\"invalid JSON\"}");
        return RTK_ERR_BADARG;
    }

    cJSON *ch_j   = cJSON_GetObjectItem(root, "channel");
    cJSON *cid_j  = cJSON_GetObjectItem(root, "chat_id");
    cJSON *path_j = cJSON_GetObjectItem(root, "vfs_path");
    cJSON *cap_j  = cJSON_GetObjectItem(root, "caption");   /* optional */
    cJSON *kind_j = cJSON_GetObjectItem(root, "kind");      /* optional */

    if (!cJSON_IsString(ch_j) || !cJSON_IsString(cid_j) || !cJSON_IsString(path_j)) {
        cJSON_Delete(root);
        claw_cap_set_output(output, "{\"error\":\"channel, chat_id and vfs_path required\"}");
        return RTK_ERR_BADARG;
    }

    const char *channel   = ch_j->valuestring;
    const char *chat_id   = cid_j->valuestring;
    const char *vfs_path  = path_j->valuestring;
    const char *caption   = (cap_j && cJSON_IsString(cap_j))  ? cap_j->valuestring  : NULL;
    const char *media_kind = (kind_j && cJSON_IsString(kind_j)) ? kind_j->valuestring : NULL;

    /* Security: only allow sending files from vfs:/inbox/ to prevent the LLM
     * from leaking sensitive vfs:/ files (config, tokens, sessions). */
    if (strncmp(vfs_path, "vfs:/inbox", 10) != 0 &&
        strncmp(vfs_path, "vfs:inbox",   9) != 0) {
        cJSON_Delete(root);
        claw_cap_set_output(output,
            "{\"error\":\"vfs_path must be under vfs:/inbox/\"}");
        return RTK_ERR_BADARG;
    }

    int ret = claw_im_dispatch_send_media(channel, chat_id, vfs_path, caption, media_kind);
    cJSON_Delete(root);

    if (ret == 0) {
        return claw_cap_set_output(output, "{\"ok\":true}");
    }
    claw_cap_set_output(output, "{\"error\":\"send_media failed\"}");
    return RTK_SUCCESS; /* return success to LLM so it sees the error message */
}

/* ---- Cap group ----------------------------------------------------------- */

static const claw_cap_descriptor_t s_caps[] = {
    {
        .id          = "im_send_media",
        .name        = "im_send_media",
        .family      = "im_attachment",
        .description =
            "Send a media file (photo, document, voice, video) to an IM channel. "
            "vfs_path must be under vfs:/inbox/. "
            "kind: \"photo\"|\"document\"|\"voice\"|\"video\" (optional, auto-detected).",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"channel\":{\"type\":\"string\",\"description\":\"IM channel: telegram, wechat, qq, feishu, local\"},"
            "\"chat_id\":{\"type\":\"string\",\"description\":\"Destination chat ID\"},"
            "\"vfs_path\":{\"type\":\"string\",\"description\":\"Local file path under vfs:/inbox/\"},"
            "\"caption\":{\"type\":\"string\",\"description\":\"Optional caption text\"},"
            "\"kind\":{\"type\":\"string\",\"description\":\"Media kind: photo, document, voice, video\"}"
            "},"
            "\"required\":[\"channel\",\"chat_id\",\"vfs_path\"]}",
        .execute     = cap_im_send_media_execute,
    },
};

static const claw_cap_group_t s_group = {
    .group_id         = "im_attachment",
    .plugin_name      = "cap_im_attachment",
    .version          = "1",
    .descriptors      = s_caps,
    .descriptor_count = 1,
};

/* ---- Public API ---------------------------------------------------------- */

int cap_im_attachment_init(void)
{
    _memset(&s, 0, sizeof(s));

    if (rtos_queue_create(&s.queue, sizeof(attach_job_t *),
                          ATTACHMENT_QUEUE_DEPTH) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "queue create failed\n");
        return RTK_ERR_NOMEM;
    }

    /* Create vfs:/inbox root directory (ignore error if exists) */
    mkdir(CAP_IM_ATTACHMENT_ROOT, 0777);

    int err = claw_cap_register_group(&s_group);
    if (err != RTK_SUCCESS) {
        RTK_LOGE(TAG, "cap_register_group failed: %d\n", err);
        return err;
    }

    RTK_LOGI(TAG, "initialized\n");
    return RTK_SUCCESS;
}

int cap_im_attachment_start(void)
{
    s.running = 1;
    if (rtos_task_create(&s.task, "im_attach", attachment_task, NULL,
                         ATTACHMENT_TASK_STACK, 1) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "task create failed\n");
        s.running = 0;
        return RTK_ERR_NOMEM;
    }
    RTK_LOGI(TAG, "task started\n");
    return RTK_SUCCESS;
}

int cap_im_attachment_enqueue(const cap_im_attachment_job_t *job)
{
    if (!job) return RTK_ERR_BADARG;

    /* Deep-copy job to heap — the caller's stack copy may be freed after return */
    attach_job_t *copy = (attach_job_t *)malloc(sizeof(*copy));
    if (!copy) {
        RTK_LOGE(TAG, "enqueue: OOM\n");
        return RTK_ERR_NOMEM;
    }
    _memcpy(copy, job, sizeof(*copy));

    /* download_url can be arbitrarily long (WeChat CDN URLs exceed 700 bytes);
     * strdup into heap so the fixed-size struct field never truncates it. */
    copy->download_url = job->download_url ? strdup(job->download_url) : NULL;
    if (job->download_url && !copy->download_url) {
        RTK_LOGE(TAG, "enqueue: OOM for url\n");
        free(copy);
        return RTK_ERR_NOMEM;
    }

    if (rtos_queue_send(s.queue, &copy, 0) != RTK_SUCCESS) {
        RTK_LOGW(TAG, "queue full, attachment dropped (%s %s)\n",
                 job->platform, job->media_kind);
        free((void *)copy->download_url);
        free(copy);
        return RTK_ERR_NOMEM;
    }
    return RTK_SUCCESS;
}

/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stddef.h>
#include "claw_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * cap_im_attachment — unified attachment receive/send capability.
 *
 * Receive side: a single background task drains a shared queue of download
 * jobs posted by any IM channel.  Files land at:
 *   vfs:/inbox/<platform>/<chat_id>/<platform>_<hash>_<kind>.<ext>
 * After download the task publishes a normal message event so the LLM sees
 * the attachment path in context without any special routing.
 *
 * Send side: the LLM tool "im_send_media" calls
 * claw_im_dispatch_send_media(), which each channel implements by streaming
 * the VFS file via multipart HTTP upload.
 */

/* Storage root for all downloaded attachments (writable VFS). */
#define CAP_IM_ATTACHMENT_ROOT  "vfs:/inbox"

/* Hard safety limit per file — abort download if server reports larger. */
#define CAP_IM_ATTACHMENT_MAX_BYTES  (4 * 1024 * 1024)  /* 4 MB */

/* Download strategy: platform-specific callback, or NULL → plain HTTPS GET. */
typedef int (*cap_im_attachment_download_fn_t)(
    const char *url,
    const char *dest_vfs_path,
    void       *ctx);

/**
 * Attachment download job — filled by an IM channel poll task and enqueued
 * via cap_im_attachment_enqueue().  All string fields are copied internally;
 * the caller may free the original struct after enqueue returns.
 */
typedef struct {
    char platform[16];          /* "telegram", "wechat", "qq", "feishu" */
    char source_cap[32];        /* "cap_im_telegram", …                  */
    char source_channel[16];    /* same as platform for now              */
    char chat_id[96];
    char sender_id[96];
    char message_id[96];
    char media_kind[16];        /* "photo", "document", "voice", "video" */
    char caption[256];
    const char *download_url;   /* heap or literal — enqueue() strdups it */
    char original_filename[64]; /* filename hint from platform            */
    char mime[32];              /* MIME hint — empty if unknown           */

    /* Optional platform-specific download hook (e.g. WeChat decrypt).
     * NULL → use default llm_http_get_to_file(). */
    cap_im_attachment_download_fn_t download_fn;
    void                           *download_ctx;
} cap_im_attachment_job_t;

/**
 * Initialise the attachment subsystem and register the "im_send_media" LLM
 * tool.  Call once before cap_im_attachment_start().
 */
int cap_im_attachment_init(void);

/**
 * Start the background download task.
 * Must be called after claw_event_dispatcher_start().
 */
int cap_im_attachment_start(void);

/**
 * Enqueue an attachment download job from an IM channel poll task.
 * Non-blocking: returns RTK_SUCCESS immediately if the queue has space,
 * RTK_ERR_NOMEM if the queue is full (job is dropped and logged).
 */
int cap_im_attachment_enqueue(const cap_im_attachment_job_t *job);

/**
 * Infer a file extension from a MIME type string.
 * Returns a string literal (no allocation needed).  Falls back to ".bin".
 */
const char *cap_im_attachment_ext_from_mime(const char *mime);

/**
 * Infer a file extension from a URL/filename hint or MIME type.
 * Tries the URL path extension first, then MIME, then ".bin".
 */
const char *cap_im_attachment_guess_ext(const char *url_or_filename,
                                         const char *mime);

#ifdef __cplusplus
}
#endif

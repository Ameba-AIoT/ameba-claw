/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_im_dispatch.h"
#include "ameba_soc.h"
#include "platform_stdlib.h"
#include "claw_cap.h"
#include <cJSON.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define TAG "claw_im_dispatch"

#define IM_DISPATCH_MAX_CHANNELS 8

typedef struct {
    char              channel[16];
    claw_im_send_fn_t fn;
    uint32_t          flags;
    char              send_cap[32];  /* LLM cap name for sending text, e.g. "wechat_send_text" */
} claw_im_binding_t;

static claw_im_binding_t s_bindings[IM_DISPATCH_MAX_CHANNELS];
static int               s_binding_count = 0;

int claw_im_dispatch_has_channel(const char *channel)
{
    if (!channel) return 0;
    for (int i = 0; i < s_binding_count; i++) {
        if (strcmp(s_bindings[i].channel, channel) == 0) return 1;
    }
    return 0;
}

int claw_im_dispatch_register_with_flags(const char *channel,
                                          claw_im_send_fn_t fn,
                                          uint32_t flags,
                                          const char *send_cap_name)
{
    if (!channel || !fn) return -1;
    if (s_binding_count >= IM_DISPATCH_MAX_CHANNELS) {
        RTK_LOGE(TAG, "binding table full, '%s' not registered\n", channel);
        return -1;
    }
    strlcpy(s_bindings[s_binding_count].channel, channel,
            sizeof(s_bindings[s_binding_count].channel));
    s_bindings[s_binding_count].fn    = fn;
    s_bindings[s_binding_count].flags = flags;
    strlcpy(s_bindings[s_binding_count].send_cap,
            send_cap_name ? send_cap_name : "",
            sizeof(s_bindings[s_binding_count].send_cap));
    s_binding_count++;
    return 0;
}

const char *claw_im_dispatch_send_cap(const char *channel)
{
    if (!channel) return NULL;
    for (int i = 0; i < s_binding_count; i++) {
        if (strcmp(s_bindings[i].channel, channel) == 0)
            return s_bindings[i].send_cap[0] ? s_bindings[i].send_cap : NULL;
    }
    return NULL;
}

uint32_t claw_im_dispatch_channel_flags(const char *channel)
{
    if (!channel) return 0;
    for (int i = 0; i < s_binding_count; i++) {
        if (strcmp(s_bindings[i].channel, channel) == 0)
            return s_bindings[i].flags;
    }
    return 0;
}

void claw_im_dispatch_send(const char *channel, const char *chat_id, const char *text)
{
    if (!channel) {
        RTK_LOGE(TAG, "dispatch_send: NULL channel, message dropped\n");
        return;
    }
    for (int i = 0; i < s_binding_count; i++) {
        if (strcmp(s_bindings[i].channel, channel) == 0) {
            s_bindings[i].fn(chat_id, text);
            return;
        }
    }
    RTK_LOGW(TAG, "no handler for channel '%s', message dropped\n", channel);
}

/* ---- Media dispatch ---- */

typedef struct {
    char                   channel[16];
    claw_im_send_media_fn_t fn;
} claw_im_media_binding_t;

static claw_im_media_binding_t s_media_bindings[IM_DISPATCH_MAX_CHANNELS];
static int                     s_media_count = 0;

int claw_im_dispatch_register_media(const char *channel, claw_im_send_media_fn_t fn)
{
    if (!channel || !fn) return -1;
    if (s_media_count >= IM_DISPATCH_MAX_CHANNELS) {
        RTK_LOGE(TAG, "media binding table full, '%s' not registered\n", channel);
        return -1;
    }
    strlcpy(s_media_bindings[s_media_count].channel, channel,
            sizeof(s_media_bindings[s_media_count].channel));
    s_media_bindings[s_media_count].fn = fn;
    s_media_count++;
    return 0;
}

int claw_im_dispatch_send_media(const char *channel, const char *chat_id,
                                 const char *vfs_path, const char *caption,
                                 const char *media_kind)
{
    if (!channel) {
        RTK_LOGE(TAG, "dispatch_send_media: NULL channel\n");
        return -1;
    }
    for (int i = 0; i < s_media_count; i++) {
        if (strcmp(s_media_bindings[i].channel, channel) == 0) {
            return s_media_bindings[i].fn(chat_id, vfs_path, caption, media_kind);
        }
    }
    RTK_LOGW(TAG, "no media handler for channel '%s'\n", channel);
    return -1;
}

/* ---- Per-channel progress send -------------------------------------------- */

typedef struct {
    char                        channel[16];
    claw_im_send_progress_fn_t  fn;
} claw_im_progress_binding_t;

static claw_im_progress_binding_t s_progress_bindings[IM_DISPATCH_MAX_CHANNELS];
static int                        s_progress_count = 0;

int claw_im_dispatch_register_progress(const char *channel,
                                        claw_im_send_progress_fn_t fn)
{
    if (!channel || !fn) return -1;
    if (s_progress_count >= IM_DISPATCH_MAX_CHANNELS) {
        RTK_LOGE(TAG, "progress binding table full, '%s' not registered\n", channel);
        return -1;
    }
    strlcpy(s_progress_bindings[s_progress_count].channel, channel,
            sizeof(s_progress_bindings[s_progress_count].channel));
    s_progress_bindings[s_progress_count].fn = fn;
    s_progress_count++;
    return 0;
}

int claw_im_dispatch_send_progress(const char *channel, const char *chat_id,
                                    const char *text, uint32_t request_id)
{
    if (!channel || !chat_id) return -1;
    for (int i = 0; i < s_progress_count; i++) {
        if (strcmp(s_progress_bindings[i].channel, channel) == 0) {
            s_progress_bindings[i].fn(chat_id, text, request_id);
            return 0;
        }
    }
    return -1;  /* no handler registered — caller uses generic path */
}

/* ---- Generic send_text cap execute ----------------------------------------
 * Shared by all IM channel send_text cap tools — eliminates copy-paste across
 * cap_im_telegram, cap_im_wechat, cap_im_feishu, cap_im_qq, cap_im_local.
 * ----------------------------------------------------------------------- */

int claw_im_cap_execute_send_text(const char *input_json,
                                   char **output,
                                   claw_im_send_fn_t send_fn)
{
    if (!input_json) {
        claw_cap_set_output(output, "{\"error\":\"missing input\"}");
        return RTK_ERR_BADARG;
    }
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        claw_cap_set_output(output, "{\"error\":\"invalid JSON\"}");
        return RTK_ERR_BADARG;
    }
    cJSON *chat_id_j = cJSON_GetObjectItem(root, "chat_id");
    cJSON *text_j    = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(chat_id_j) || !cJSON_IsString(text_j)) {
        cJSON_Delete(root);
        claw_cap_set_output(output, "{\"error\":\"chat_id and text required\"}");
        return RTK_ERR_BADARG;
    }
    send_fn(chat_id_j->valuestring, text_j->valuestring);
    cJSON_Delete(root);
    return claw_cap_set_output(output, "{\"ok\":true}");
}

/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_im_qq.h"
#include "claw_cap.h"
#include "claw_compat.h"
#include "claw_im_dispatch.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "cap_im_qq";

/* ---- Cap execute: qq_send_message (stub) ---- */

static int qq_send_message_execute(const char *input_json,
                                          const claw_cap_call_context_t *ctx,
                                          char **output)
{
    (void)input_json;
    (void)ctx;
    return claw_cap_set_output(output,
             "{\"error\":\"QQ bot requires official enterprise approval from QQ open platform\"}");
}

/* ---- Cap group descriptors ---- */

static const claw_cap_descriptor_t s_qq_caps[] = {
    {
        .id          = "qq_send_message",
        .name        = "qq_send_message",
        .family      = "im_qq",
        .description = "Send a text message to a QQ chat (stub - requires enterprise approval)",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"chat_id\":{\"type\":\"string\",\"description\":\"QQ chat ID\"},"
            "\"text\":{\"type\":\"string\",\"description\":\"Message text to send\"}"
            "},"
            "\"required\":[\"chat_id\",\"text\"]}",
        .execute = qq_send_message_execute,
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
    (void)text;
    RTK_LOGW(TAG, "QQ send stub: chat_id=%s (QQ bot requires official approval)\n", chat_id);
}

int cap_im_qq_init(const cap_im_qq_config_t *cfg)
{
    if (!cfg) return RTK_ERR_BADARG;

    /* Register outbound IM dispatch */
    claw_im_dispatch_register_with_flags("qq", cap_im_qq_send, 0, "qq_send_message");

    /* Register cap group */
    int err = claw_cap_register_group(&s_qq_group);
    if (err != RTK_SUCCESS) {
        RTK_LOGE(TAG, "claw_cap_register_group failed: %d\n", err);
        return err;
    }

    return RTK_SUCCESS;
}

int cap_im_qq_start(void)
{
    return RTK_SUCCESS;
}

/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "claw_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t http_port;     /* default 80 (reuses claw_http_server) */
    uint16_t msg_buf_size;  /* ring buffer depth, default 32 */
} cap_im_local_config_t;

#define CAP_IM_LOCAL_DEFAULT_CONFIG() { .http_port = 80, .msg_buf_size = 32 }

int cap_im_local_init(const cap_im_local_config_t *cfg);
int cap_im_local_start(void);

/* Called by on_response when channel=="local" to push LLM reply.
 * Falls back to cap_session_mgr_get_current() when chat_id=="local". */
void cap_im_local_send(const char *chat_id, const char *text);

/* Variant used when the target alias is already known (avoids get_current race).
 * alias may be NULL or empty to fall back to get_current. */
void cap_im_local_send_for_alias(const char *alias, const char *text);

/* Remove all in-memory ring-buffer entries for the given alias.
 * Must be called after cap_session_mgr_clear_chat so that WS sync
 * responses no longer return stale messages. */
void cap_im_local_clear_alias(const char *alias);

#ifdef __cplusplus
}
#endif

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

/* Called by on_out_message when channel=="local" to push LLM reply */
void cap_im_local_send(const char *chat_id, const char *text);

#ifdef __cplusplus
}
#endif

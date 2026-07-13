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
    char   app_id[64];
    char   app_secret[128];
    int8_t msg_type;          /* 0=text, 2=markdown */
} cap_im_qq_config_t;

#define CAP_IM_QQ_DEFAULT_CONFIG() { \
    .app_id     = "",               \
    .app_secret = "",               \
    .msg_type   = 0,                \
}

int  cap_im_qq_init(const cap_im_qq_config_t *cfg);
int  cap_im_qq_start(void);
void cap_im_qq_send(const char *chat_id, const char *text);

#ifdef __cplusplus
}
#endif

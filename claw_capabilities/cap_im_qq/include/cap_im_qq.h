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
    char app_id[64];
    char client_secret[64];
    char webhook_path[64];  /* "/qq" for future webhook */
} cap_im_qq_config_t;

#define CAP_IM_QQ_DEFAULT_CONFIG() { \
    .app_id = "", \
    .client_secret = "", \
    .webhook_path = "/qq" \
}

int cap_im_qq_init(const cap_im_qq_config_t *cfg);
int cap_im_qq_start(void);
void      cap_im_qq_send(const char *chat_id, const char *text);

#ifdef __cplusplus
}
#endif

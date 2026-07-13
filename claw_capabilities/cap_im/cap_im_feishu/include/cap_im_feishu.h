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
    char     app_id[64];                    /* Feishu App ID (informational; runtime reads claw_config) */
    char     app_secret[64];               /* Feishu App Secret */
    uint32_t token_refresh_interval_s;     /* Token refresh interval, default 3600s */
} cap_im_feishu_config_t;

#define CAP_IM_FEISHU_DEFAULT_CONFIG() { \
    .app_id = "", \
    .app_secret = "", \
    .token_refresh_interval_s = 3600 \
}

int  cap_im_feishu_init(const cap_im_feishu_config_t *cfg);
int  cap_im_feishu_start(void);
void cap_im_feishu_send(const char *chat_id, const char *text);

#ifdef __cplusplus
}
#endif

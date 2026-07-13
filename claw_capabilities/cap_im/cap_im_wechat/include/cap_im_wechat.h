/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "claw_compat.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int poll_retry_delay_ms; /* delay between poll retries on error, default 2000 */
    int wifi_timeout_ms;     /* max wait for WiFi, default 30000 */
    int max_retry_count;     /* max poll errors before relogin, default 10 */
} cap_im_wechat_config_t;

#define CAP_IM_WECHAT_DEFAULT_CONFIG() { \
    .poll_retry_delay_ms = 2000, \
    .wifi_timeout_ms     = 30000, \
    .max_retry_count     = 10, \
}

typedef enum {
    CAP_IM_WECHAT_STATE_IDLE       = 0, /* no token, waiting for QR trigger */
    CAP_IM_WECHAT_STATE_QR_PENDING = 1, /* QR fetched, waiting for user scan */
    CAP_IM_WECHAT_STATE_POLLING    = 2, /* logged in, polling messages */
    CAP_IM_WECHAT_STATE_ERROR      = 3, /* fatal error, needs re-login */
} cap_im_wechat_state_t;

int  cap_im_wechat_init(const cap_im_wechat_config_t *cfg);
int  cap_im_wechat_start(void);
void cap_im_wechat_send(const char *chat_id, const char *text);

/**
 * Fetch a WeChat QR code and begin background polling for login confirmation.
 * If a QR is already pending, returns existing QR URL without re-fetching.
 * @param qr_url   Buffer to receive the QR image URL
 * @param size     Size of qr_url buffer
 * @return  0   success (qr_url filled)
 *         -1   not initialised
 *         -2   already logged in (POLLING state)
 *         -3   QR fetch or task-spawn failed
 *         -4   WiFi not connected
 */
int cap_im_wechat_get_qr(char *qr_url, size_t size);

/**
 * Write current state as JSON into buf.
 * Format: {"state":"idle|qr_pending|polling|error"[,"qr_url":"..."]}
 */
void cap_im_wechat_get_status_json(char *buf, size_t buf_size);

/**
 * Save bot token to VFS file without changing state or starting polling.
 * Pass NULL or empty string to clear the saved token.
 * @return 0 on success, negative on error
 */
int cap_im_wechat_store_token(const char *token);

/**
 * Read the saved bot token from VFS.
 * @return 0 if a token exists and was copied, negative if none saved
 */
int cap_im_wechat_get_token(char *buf, size_t size);

#ifdef __cplusplus
}
#endif

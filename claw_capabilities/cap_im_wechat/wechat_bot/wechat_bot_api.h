/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef WECHAT_BOT_API_H
#define WECHAT_BOT_API_H

#include <stddef.h>
#include <stdint.h>
#include "wechat_bot_http.h"

#define WECHAT_API_TOKEN_SIZE       256
#define WECHAT_API_BASE_URL_SIZE    160
#define WECHAT_API_CHAT_ID_SIZE     72
#define WECHAT_API_CTX_TOKEN_SIZE   160
#define WECHAT_API_TEXT_SIZE        2048

#define WECHAT_DEFAULT_BASE_URL     "https://ilinkai.weixin.qq.com"
#define WECHAT_DEFAULT_APP_ID       "bot"
#define WECHAT_DEFAULT_CLIENT_VER   "131329"

#define WECHAT_CONTEXT_CACHE_SIZE   16

typedef struct {
    char chat_id[WECHAT_API_CHAT_ID_SIZE];
    char context_token[WECHAT_API_CTX_TOKEN_SIZE];
} wechat_context_entry_t;

typedef struct {
    char token[WECHAT_API_TOKEN_SIZE];
    char base_url[WECHAT_API_BASE_URL_SIZE];
    char app_id[64];               /* iLink App ID, default WECHAT_DEFAULT_APP_ID */
    wechat_context_entry_t context_cache[WECHAT_CONTEXT_CACHE_SIZE];
    size_t context_idx;
    char *sync_buf;
    int poll_timeout_ms;
} wechat_bot_state_t;

/**
 * Perform QR login flow.
 * Prints QR URL to serial, blocks until confirmed or timeout.
 * On success, fills state->token and state->base_url.
 * @param state  Bot state to populate
 * @return 0 on success, negative on error
 */
int wechat_api_qr_login(wechat_bot_state_t *state);

/**
 * Fetch one QR code from the server (single HTTP GET, no polling).
 * state->base_url must be set (use wechat_api_state_init first).
 * @param qr_url       Output buffer for the QR image URL
 * @param qr_url_size  Size of qr_url buffer
 * @param qr_id        Output buffer for the QR code ID used in status polling
 * @param qr_id_size   Size of qr_id buffer
 * @return 0 on success, negative on error
 */
int wechat_api_qr_fetch(wechat_bot_state_t *state,
                        char *qr_url, size_t qr_url_size,
                        char *qr_id,  size_t qr_id_size);

/* Return codes for wechat_api_qr_poll_once */
#define WECHAT_QR_WAIT       0  /* still waiting for scan */
#define WECHAT_QR_SCANNED    1  /* scanned, waiting for confirmation */
#define WECHAT_QR_CONFIRMED  2  /* confirmed; state->token and state->base_url set, token saved */
#define WECHAT_QR_REDIRECTED 3  /* redirect; state->base_url updated, reopen session */
#define WECHAT_QR_EXPIRED    4  /* QR expired */

/**
 * Poll QR status once on an open persistent session.
 * On WECHAT_QR_CONFIRMED fills state->token and state->base_url and saves token to VFS.
 * On WECHAT_QR_REDIRECTED updates state->base_url (caller should close/reopen session).
 * @return WECHAT_QR_* constant, or negative on transport error
 */
int wechat_api_qr_poll_once(wechat_bot_state_t *state,
                             wechat_http_session_t *session,
                             const char *qr_id);

/**
 * Open a persistent HTTPS session to the poll host derived from state->base_url.
 * Pass the returned session to wechat_api_poll to reuse the TLS connection.
 * Caller must close it with wechat_http_session_close() when done.
 * Returns NULL on failure (wechat_api_poll will fall back to one-shot mode).
 */
wechat_http_session_t *wechat_api_open_poll_session(wechat_bot_state_t *state);

/**
 * Parsed representation of one item from a WeChat getupdates message.
 * Only fields relevant to the item type are populated; others are NULL/0.
 */
typedef struct {
    int         type;           /* 1=text, 2=image */
    const char *chat_id;        /* conversation ID (group_id or from_user_id) */
    const char *sender_id;      /* from_user_id */
    const char *msg_id;         /* msg_id string from server */
    /* type == 1 */
    const char *text;
    /* type == 2 (image) */
    const char *image_url;      /* media.full_url — HTTPS download URL */
    const char *image_aeskey;   /* image_item.aeskey — 32-char hex AES-128 key */
    int         image_size;     /* media.mid_size — encrypted content bytes */
} wechat_item_t;

/**
 * Poll for new messages via getupdates.
 * Calls item_callback once per item in every received message (all types).
 * Updates state->sync_buf and state->context_cache.
 * @param state          Bot state
 * @param item_callback  Called as: callback(&item, user_data)
 * @param user_data      Passed through to item_callback
 * @param poll_session   Persistent HTTPS session (NULL = one-shot per call)
 * @return 0 on success, negative on error
 */
int wechat_api_poll(wechat_bot_state_t *state,
                    void (*item_callback)(const wechat_item_t *item,
                                         void *user_data),
                    void *user_data,
                    wechat_http_session_t *poll_session);

/**
 * Download and AES-128-ECB decrypt a WeChat image directly to a VFS file.
 * Processes encrypted content in 256-byte blocks; peak heap = one block.
 * @param full_url      HTTPS URL from image_item.media.full_url
 * @param aeskey_hex    32-char hex string of the 16-byte AES-128 key
 * @param dest_path     Destination VFS path (directory must already exist)
 * @param out_bytes     Written plaintext bytes (may be NULL)
 * @return 0 on success, negative on error
 */
int wechat_api_download_decrypt_image(const char *full_url,
                                       const char *aeskey_hex,
                                       const char *dest_path,
                                       size_t     *out_bytes);

/**
 * Send a text message to a chat.
 * @param state    Bot state (uses context_cache for context_token)
 * @param chat_id  Target chat ID
 * @param text     Text to send
 * @return 0 on success, negative on error
 */
int wechat_api_send_text(wechat_bot_state_t *state,
                         const char *chat_id,
                         const char *text);

/** Initialize bot state with defaults. */
void wechat_api_state_init(wechat_bot_state_t *state);

/** Free dynamically allocated state resources (sync_buf). */
void wechat_api_state_cleanup(wechat_bot_state_t *state);

#endif /* WECHAT_BOT_API_H */

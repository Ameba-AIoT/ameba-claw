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
    const char *bot_token;      /* Telegram Bot Token, e.g. "123456:ABCdef..." */
    long        poll_timeout_sec; /* getUpdates long-polling timeout, default 25 */
} cap_im_telegram_config_t;

/**
 * Initialize the Telegram bot module.
 * Registers the cap group and IM dispatch handler.
 * @param config  Module configuration (bot_token may be empty for stub mode)
 * @return RTK_SUCCESS on success
 */
int cap_im_telegram_init(const cap_im_telegram_config_t *config);

/**
 * Start the long-polling task.
 * Must be called after claw_event_dispatcher_start().
 * @return RTK_SUCCESS on success
 */
int cap_im_telegram_start(void);

/**
 * Send a text message to a Telegram chat.
 * Matches claw_im_send_fn_t signature for IM dispatch registration.
 * @param chat_id  Telegram chat ID as decimal string
 * @param text     Message text
 */
void cap_im_telegram_send(const char *chat_id, const char *text);

/**
 * Send a media file to a Telegram chat via multipart upload.
 * Registered as claw_im_send_media_fn_t for the "telegram" channel.
 * @param chat_id    Telegram chat ID as decimal string
 * @param vfs_path   Local VFS file path
 * @param caption    Optional caption (may be NULL)
 * @param media_kind "photo", "document", "voice", "video", or NULL (auto)
 * @return 0 on success, negative on error
 */
int cap_im_telegram_send_media(const char *chat_id,
                                const char *vfs_path,
                                const char *caption,
                                const char *media_kind);

#ifdef __cplusplus
}
#endif

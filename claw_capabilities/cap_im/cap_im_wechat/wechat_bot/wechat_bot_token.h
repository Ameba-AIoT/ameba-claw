/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef WECHAT_BOT_TOKEN_H
#define WECHAT_BOT_TOKEN_H

#include <stddef.h>

#define WECHAT_TOKEN_FILE    "vfs:wechat_token"
#define WECHAT_TOKEN_SIZE    256

/**
 * Load bot token from VFS file.
 * @param token_buf  Output buffer for token string
 * @param buf_size   Size of output buffer
 * @return 0 on success, negative if no token or error
 */
int wechat_token_load(char *token_buf, size_t buf_size);

/**
 * Save bot token to VFS file.
 * @param token  Token string to save
 * @return 0 on success, negative on error
 */
int wechat_token_save(const char *token);

/**
 * Remove saved token file.
 * @return 0 on success, negative on error
 */
int wechat_token_clear(void);

#endif /* WECHAT_BOT_TOKEN_H */

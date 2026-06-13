/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wechat_bot_token.h"
#include "platform_stdlib.h"
#include "diag.h"
#include <stdio.h>
#include <string.h>

int wechat_token_load(char *token_buf, size_t buf_size)
{
    FILE *f;
    size_t nread;

    if (!token_buf || buf_size == 0) {
        return -1;
    }

    token_buf[0] = '\0';

    f = fopen(WECHAT_TOKEN_FILE, "r");
    if (!f) {
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return -1;
    }

    if ((size_t)size >= buf_size) {
        size = (long)(buf_size - 1);
    }

    nread = fread(token_buf, 1, (size_t)size, f);
    token_buf[nread] = '\0';
    fclose(f);

    /* Trim trailing whitespace/newline */
    while (nread > 0 && (token_buf[nread - 1] == '\n' ||
                         token_buf[nread - 1] == '\r' ||
                         token_buf[nread - 1] == ' ')) {
        nread--;
        token_buf[nread] = '\0';
    }

    if (nread == 0) {
        return -1;
    }

    DiagPrintf("[wechat_token] loaded token (%u bytes)\n", (unsigned)nread);
    return 0;
}

int wechat_token_save(const char *token)
{
    FILE *f;
    size_t len;
    size_t written;

    if (!token || !token[0]) {
        return -1;
    }

    len = strlen(token);
    f = fopen(WECHAT_TOKEN_FILE, "w");
    if (!f) {
        DiagPrintf("[wechat_token] failed to open %s for writing\n", WECHAT_TOKEN_FILE);
        return -2;
    }

    written = fwrite(token, 1, len, f);
    fclose(f);

    if (written != len) {
        DiagPrintf("[wechat_token] write incomplete %u/%u\n",
               (unsigned)written, (unsigned)len);
        return -3;
    }

    DiagPrintf("[wechat_token] saved token (%u bytes)\n", (unsigned)len);
    return 0;
}

int wechat_token_clear(void)
{
    if (remove(WECHAT_TOKEN_FILE) == 0) {
        DiagPrintf("[wechat_token] removed token file\n");
        return 0;
    }
    return -1;
}

/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef WECHAT_BOT_HTTP_H
#define WECHAT_BOT_HTTP_H

#include <stddef.h>
#include <stdint.h>

#define WECHAT_HTTP_RESP_INIT_SIZE  2048
#define WECHAT_HTTP_RESP_MAX_SIZE   (16 * 1024)

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} wechat_http_resp_t;

/**
 * Perform HTTPS GET request.
 * @param host     Server hostname (e.g. "ilinkai.weixin.qq.com")
 * @param resource Resource path with query string
 * @param headers  NULL-terminated array of "Name: Value" header strings, or NULL
 * @param response Output response struct (caller must call wechat_http_resp_free)
 * @return 0 on success (HTTP 2xx), negative on error
 */
int wechat_http_get(const char *host, const char *resource,
                    const char *headers[], wechat_http_resp_t *response);

/**
 * Perform HTTPS POST request.
 * @param host         Server hostname
 * @param resource     Resource path
 * @param content_type Content-Type header value
 * @param body         Request body data
 * @param body_len     Request body length
 * @param headers      NULL-terminated array of "Name: Value" header strings, or NULL
 * @param response     Output response struct (caller must call wechat_http_resp_free)
 * @return 0 on success (HTTP 2xx), negative on error
 */
int wechat_http_post(const char *host, const char *resource,
                     const char *content_type,
                     const char *body, size_t body_len,
                     const char *headers[], wechat_http_resp_t *response);

/** Initialize a response struct (allocates initial buffer). */
int wechat_http_resp_init(wechat_http_resp_t *resp);

/** Free response buffer. */
void wechat_http_resp_free(wechat_http_resp_t *resp);

/**
 * Persistent HTTPS session — reuses the TLS connection across multiple GETs
 * to the same host, avoiding a full handshake (~10 s) on every poll.
 */
typedef struct wechat_http_session wechat_http_session_t;

wechat_http_session_t *wechat_http_session_open(const char *host);
void                   wechat_http_session_close(wechat_http_session_t *s);

/**
 * Perform a GET on an open session.  Reconnects transparently on error.
 * Returns 0 on success, negative on unrecoverable failure.
 */
int wechat_http_session_get(wechat_http_session_t *s,
                            const char *resource,
                            const char *headers[],
                            wechat_http_resp_t *response);

/**
 * Perform a POST on an open session.  Uses Connection: keep-alive to avoid
 * a TLS handshake on every call.  Reconnects transparently on error.
 * Returns 0 on success, negative on unrecoverable failure.
 */
int wechat_http_session_post(wechat_http_session_t *s,
                             const char *resource,
                             const char *content_type,
                             const char *body, size_t body_len,
                             const char *headers[],
                             wechat_http_resp_t *response);

#endif /* WECHAT_BOT_HTTP_H */

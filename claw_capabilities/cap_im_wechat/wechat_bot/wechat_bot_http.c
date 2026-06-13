/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wechat_bot_http.h"
#include "httpc/httpc.h"
#include "platform_stdlib.h"
#include <lwip/sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os_wrapper.h"
#include "basic_types.h"

#define TAG "wechat_http"

/* Access the httpc_tls internal struct to access TLS context */
struct httpc_tls_internal {
    mbedtls_ssl_context ctx;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    mbedtls_x509_crt cert;
    mbedtls_pk_context key;
};

/*
 * Custom BIO recv with timeout using lwip_select() + lwip_recv().
 *
 * The default mbedtls_net_recv_timeout() uses POSIX select() and read(),
 * neither of which work correctly with lwIP sockets on this platform:
 *   - POSIX select() doesn't recognize lwIP socket fds
 *   - POSIX read() bypasses lwIP's timeout handling
 *
 * This replacement uses lwip_select() and lwip_recv() directly.
 */
static int wechat_ssl_recv_timeout(void *ctx, unsigned char *buf, size_t len,
                                   uint32_t timeout)
{
    int fd = *((int *)ctx);
    fd_set read_fds;
    struct timeval tv;
    int ret;

    if (fd < 0) {
        return MBEDTLS_ERR_NET_INVALID_CONTEXT;
    }

    if (timeout == 0) {
        /* No timeout - blocking read */
        ret = (int)lwip_recv(fd, buf, len, 0);
        if (ret < 0) {
            return MBEDTLS_ERR_NET_RECV_FAILED;
        }
        return ret;
    }

    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);

    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000) * 1000;

    ret = lwip_select(fd + 1, &read_fds, NULL, NULL, &tv);
    if (ret == 0) {
        return MBEDTLS_ERR_SSL_TIMEOUT;
    }
    if (ret < 0) {
        RTK_LOGE(TAG, "lwip_select error\n");
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }

    ret = (int)lwip_recv(fd, buf, len, 0);
    if (ret < 0) {
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return ret;
}

static void install_custom_tls_timeout(struct httpc_conn *conn, int timeout_ms)
{
    struct httpc_tls_internal *tls = (struct httpc_tls_internal *)conn->tls;
    if (!tls) {
        return;
    }
    mbedtls_ssl_conf_read_timeout(&tls->conf, timeout_ms);
    mbedtls_ssl_set_bio(&tls->ctx, &conn->sock,
                        mbedtls_net_send, NULL, wechat_ssl_recv_timeout);
}

int wechat_http_resp_init(wechat_http_resp_t *resp)
{
    if (!resp) {
        return -1;
    }

    resp->buf = (char *)malloc(WECHAT_HTTP_RESP_INIT_SIZE);
    if (!resp->buf) {
        return -2;
    }
    resp->buf[0] = '\0';
    resp->len = 0;
    resp->cap = WECHAT_HTTP_RESP_INIT_SIZE;
    return 0;
}

void wechat_http_resp_free(wechat_http_resp_t *resp)
{
    if (!resp) {
        return;
    }
    if (resp->buf) {
        free(resp->buf);
        resp->buf = NULL;
    }
    resp->len = 0;
    resp->cap = 0;
}

static int resp_append(wechat_http_resp_t *resp, const char *data, size_t data_len)
{
    if (!resp || !data || data_len == 0) {
        return 0;
    }

    if (resp->len + data_len + 1 > resp->cap) {
        size_t new_cap = resp->cap;
        while (new_cap < resp->len + data_len + 1) {
            new_cap *= 2;
        }
        if (new_cap > WECHAT_HTTP_RESP_MAX_SIZE) {
            new_cap = WECHAT_HTTP_RESP_MAX_SIZE;
        }
        if (resp->len + data_len + 1 > new_cap) {
            RTK_LOGE(TAG, "response too large (%u)\n", (unsigned)(resp->len + data_len));
            return -1;
        }
        char *new_buf = (char *)realloc(resp->buf, new_cap);
        if (!new_buf) {
            return -2;
        }
        resp->buf = new_buf;
        resp->cap = new_cap;
    }

    _memcpy(resp->buf + resp->len, data, data_len);
    resp->len += data_len;
    resp->buf[resp->len] = '\0';
    return 0;
}

static int do_http_request(const char *method, const char *host,
                           const char *resource, const char *content_type,
                           const char *body, size_t body_len,
                           const char *headers[],
                           wechat_http_resp_t *response)
{
    struct httpc_conn *conn = NULL;
    int ret;

    conn = httpc_conn_new(HTTPC_SECURE_TLS, NULL, NULL, NULL);
    if (!conn) {
        RTK_LOGE(TAG, "httpc_conn_new failed\n");
        return -1;
    }

    /* Enable httpc debug to trace protocol issues */
    httpc_setup_debug(HTTPC_DEBUG_ON);

    /* For long-polling, we need to ignore content_len=0 */
    httpc_enable_ignore_content_len(conn);

    if (httpc_conn_connect(conn, (char *)host, 443, 30) != 0) {
        RTK_LOGE(TAG, "connect to %s failed\n", host);
        httpc_conn_free(conn);
        return -2;
    }

    /* Install custom TLS recv timeout using lwip_select + lwip_recv.
     * Default mbedtls uses POSIX select()/read() which don't work with lwIP. */
    install_custom_tls_timeout(conn, 40000);

    /* Build request header */
    ret = httpc_request_write_header_start(conn, (char *)method, (char *)resource,
                                           (char *)content_type,
                                           body ? body_len : 0);
    if (ret != 0) {
        RTK_LOGE(TAG, "write_header_start failed\n");
        goto cleanup;
    }

    /* Add Connection: close */
    httpc_request_write_header(conn, (char *)"Connection", (char *)"close");

    /* Add extra headers */
    if (headers) {
        int i;
        for (i = 0; headers[i] != NULL; i += 2) {
            if (headers[i + 1] == NULL) {
                break;
            }
            httpc_request_write_header(conn, (char *)headers[i], (char *)headers[i + 1]);
        }
    }

    ret = httpc_request_write_header_finish(conn);
    if (ret < 0) {
        RTK_LOGE(TAG, "write_header_finish failed\n");
        goto cleanup;
    }

    /* Write body if POST */
    if (body && body_len > 0) {
        ret = httpc_request_write_data(conn, (uint8_t *)body, body_len);
        if (ret < 0) {
            RTK_LOGE(TAG, "write_data failed\n");
            goto cleanup;
        }
    }

    /* Read response using mbedtls_ssl_read with our custom lwip-based timeout.
     * The custom wechat_ssl_recv_timeout uses lwip_select() which works
     * correctly with lwIP sockets, unlike the POSIX select(). */
    {
        struct httpc_tls_internal *tls = (struct httpc_tls_internal *)conn->tls;
        uint8_t buf[512];
        int total_read = 0;
        int header_end = 0;
        int header_end_steps = 0;

        while (1) {
            int n;
            n = mbedtls_ssl_read(&tls->ctx, buf, sizeof(buf));
            if (n > 0) {
                if (resp_append(response, (const char *)buf, (size_t)n) != 0) {
                    break;
                }
                total_read += n;

                /* Check for end of headers */
                if (!header_end) {
                    size_t i;
                    for (i = 0; i < response->len; i++) {
                        char c = response->buf[i];
                        if (c == '\r' && header_end_steps == 0) header_end_steps = 1;
                        else if (c == '\n' && header_end_steps == 1) header_end_steps = 2;
                        else if (c == '\r' && header_end_steps == 2) header_end_steps = 3;
                        else if (c == '\n' && header_end_steps == 3) { header_end = 1; break; }
                        else header_end_steps = 0;
                    }
                }

                /* If we have headers and body, check content-length */
                if (header_end) {
                    char *cl_str = strstr(response->buf, "Content-Length:");
                    if (!cl_str) cl_str = strstr(response->buf, "content-length:");
                    if (cl_str) {
                        int content_len = atoi(cl_str + 15);
                        char *cl_body = strstr(response->buf, "\r\n\r\n");
                        if (cl_body) {
                            int cl_body_len = total_read - (int)(cl_body + 4 - response->buf);
                            if (cl_body_len >= content_len) break;
                        }
                    }
                    /* No Content-Length or chunked - read until connection close */
                }
            } else if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                /* Connection closed by server */
                break;
            } else if (n == MBEDTLS_ERR_SSL_WANT_READ) {
                /* No data available yet */
                rtos_time_delay_ms(100);
            } else if (n == MBEDTLS_ERR_SSL_TIMEOUT) {
                break;
            } else {
                RTK_LOGE(TAG, "ssl_read error: %d\n", n);
                break;
            }
        }

        RTK_LOGD(TAG, "read total %d bytes\n", total_read);

        /* Strip HTTP headers, keep only body */
        if (header_end) {
            char *body_start = strstr(response->buf, "\r\n\r\n");
            if (body_start) {
                body_start += 4;
                size_t body_len2 = total_read - (size_t)(body_start - response->buf);
                _memmove(response->buf, body_start, body_len2 + 1);
                response->len = body_len2;
            }
        }
    }

    httpc_conn_close(conn);
    httpc_conn_free(conn);
    return 0;

cleanup:
    httpc_conn_close(conn);
    httpc_conn_free(conn);
    return -3;
}

int wechat_http_get(const char *host, const char *resource,
                    const char *headers[], wechat_http_resp_t *response)
{
    if (!host || !resource || !response) {
        return -1;
    }
    return do_http_request("GET", host, resource,
                           "application/json", NULL, 0,
                           headers, response);
}

/* ---------- Persistent session (connection reuse) ---------- */

struct wechat_http_session {
    struct httpc_conn *conn;
    char host[128];
};

static struct httpc_conn *session_connect(wechat_http_session_t *s)
{
    if (s->conn) {
        httpc_conn_close(s->conn);
        httpc_conn_free(s->conn);
        s->conn = NULL;
    }
    s->conn = httpc_conn_new(HTTPC_SECURE_TLS, NULL, NULL, NULL);
    if (!s->conn) {
        return NULL;
    }
    httpc_setup_debug(HTTPC_DEBUG_ON);
    httpc_enable_ignore_content_len(s->conn);
    RTK_LOGD(TAG, "session connecting to %s:443...\n", s->host);
    if (httpc_conn_connect(s->conn, s->host, 443, 30) != 0) {
        RTK_LOGE(TAG, "session connect failed\n");
        httpc_conn_free(s->conn);
        s->conn = NULL;
        return NULL;
    }
    install_custom_tls_timeout(s->conn, 40000);
    return s->conn;
}

wechat_http_session_t *wechat_http_session_open(const char *host)
{
    wechat_http_session_t *s;
    if (!host) {
        return NULL;
    }
    s = (wechat_http_session_t *)malloc(sizeof(*s));
    if (!s) {
        return NULL;
    }
    _memset(s, 0, sizeof(*s));
    strncpy(s->host, host, sizeof(s->host) - 1);
    if (!session_connect(s)) {
        free(s);
        return NULL;
    }
    return s;
}

void wechat_http_session_close(wechat_http_session_t *s)
{
    if (!s) {
        return;
    }
    if (s->conn) {
        httpc_conn_close(s->conn);
        httpc_conn_free(s->conn);
    }
    free(s);
}

int wechat_http_session_get(wechat_http_session_t *s,
                            const char *resource,
                            const char *headers[],
                            wechat_http_resp_t *response)
{
    struct httpc_conn *conn;
    struct httpc_tls_internal *tls;
    int ret;
    int reconnect_attempts = 0;

    if (!s || !resource || !response) {
        return -1;
    }

retry:
    if (reconnect_attempts > 3) {
        RTK_LOGE(TAG, "session: too many reconnect attempts\n");
        return -2;
    }

    if (!s->conn) {
        if (!session_connect(s)) {
            reconnect_attempts++;
            rtos_time_delay_ms(1000);
            goto retry;
        }
    }
    conn = s->conn;
    tls  = (struct httpc_tls_internal *)conn->tls;

    ret = httpc_request_write_header_start(conn, (char *)"GET",
                                           (char *)resource, NULL, 0);
    if (ret != 0) {
        goto do_reconnect;
    }

    httpc_request_write_header(conn, (char *)"Connection", (char *)"keep-alive");

    if (headers) {
        int i;
        for (i = 0; headers[i] != NULL; i += 2) {
            if (headers[i + 1] == NULL) {
                break;
            }
            httpc_request_write_header(conn, (char *)headers[i],
                                       (char *)headers[i + 1]);
        }
    }

    ret = httpc_request_write_header_finish(conn);
    if (ret < 0) {
        goto do_reconnect;
    }

    /* Read response, stop at Content-Length (keep connection alive) */
    {
        uint8_t buf[512];
        int total_read   = 0;
        int header_end   = 0;
        int hdr_steps    = 0;

        while (1) {
            int n = mbedtls_ssl_read(&tls->ctx, buf, sizeof(buf));
            if (n > 0) {
                if (resp_append(response, (const char *)buf, (size_t)n) != 0) {
                    break;
                }
                total_read += n;

                if (!header_end) {
                    size_t i;
                    for (i = 0; i < response->len; i++) {
                        char c = response->buf[i];
                        if      (c == '\r' && hdr_steps == 0) hdr_steps = 1;
                        else if (c == '\n' && hdr_steps == 1) hdr_steps = 2;
                        else if (c == '\r' && hdr_steps == 2) hdr_steps = 3;
                        else if (c == '\n' && hdr_steps == 3) {
                            header_end = 1;
                            break;
                        } else {
                            hdr_steps = 0;
                        }
                    }
                }

                if (header_end) {
                    char *cl_str = strstr(response->buf, "Content-Length:");
                    if (!cl_str) {
                        cl_str = strstr(response->buf, "content-length:");
                    }
                    if (cl_str) {
                        int content_len = atoi(cl_str + 15);
                        char *body_ptr  = strstr(response->buf, "\r\n\r\n");
                        if (body_ptr) {
                            int body_rx = total_read -
                                          (int)(body_ptr + 4 - response->buf);
                            if (body_rx >= content_len) {
                                break;
                            }
                        }
                    }
                }
            } else if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                /* Server closed; mark conn dead so next call reconnects */
                httpc_conn_close(s->conn);
                httpc_conn_free(s->conn);
                s->conn = NULL;
                break;
            } else if (n == MBEDTLS_ERR_SSL_WANT_READ) {
                rtos_time_delay_ms(100);
            } else if (n == MBEDTLS_ERR_SSL_TIMEOUT) {
                break;
            } else {
                goto do_reconnect;
            }
        }

        RTK_LOGD(TAG, "session read total %d bytes\n", total_read);

        if (header_end) {
            char *body_ptr = strstr(response->buf, "\r\n\r\n");
            if (body_ptr) {
                body_ptr += 4;
                size_t blen = (size_t)(total_read -
                              (int)(body_ptr - response->buf));
                _memmove(response->buf, body_ptr, blen + 1);
                response->len = blen;
            }
        }
    }
    return 0;

do_reconnect:
    RTK_LOGW(TAG, "session error, reconnecting...\n");
    reconnect_attempts++;
    if (s->conn) {
        httpc_conn_close(s->conn);
        httpc_conn_free(s->conn);
        s->conn = NULL;
    }
    response->len = 0;
    if (response->buf) {
        response->buf[0] = '\0';
    }
    goto retry;
}

int wechat_http_post(const char *host, const char *resource,
                     const char *content_type,
                     const char *body, size_t body_len,
                     const char *headers[], wechat_http_resp_t *response)
{
    if (!host || !resource || !response) {
        return -1;
    }
    return do_http_request("POST", host, resource,
                           content_type ? content_type : "application/json",
                           body, body_len,
                           headers, response);
}

int wechat_http_session_post(wechat_http_session_t *s,
                              const char *resource,
                              const char *content_type,
                              const char *body, size_t body_len,
                              const char *headers[],
                              wechat_http_resp_t *response)
{
    struct httpc_conn *conn;
    struct httpc_tls_internal *tls;
    int ret;
    int reconnect_attempts = 0;

    if (!s || !resource || !response) {
        return -1;
    }

retry:
    if (reconnect_attempts > 3) {
        RTK_LOGE(TAG, "session_post: too many reconnect attempts\n");
        return -2;
    }

    if (!s->conn) {
        if (!session_connect(s)) {
            reconnect_attempts++;
            rtos_time_delay_ms(1000);
            goto retry;
        }
    }
    conn = s->conn;
    tls  = (struct httpc_tls_internal *)conn->tls;

    ret = httpc_request_write_header_start(conn, (char *)"POST",
                                           (char *)resource,
                                           (char *)(content_type ? content_type : "application/json"),
                                           body ? body_len : 0);
    if (ret != 0) {
        goto do_reconnect;
    }

    httpc_request_write_header(conn, (char *)"Connection", (char *)"keep-alive");

    if (headers) {
        int i;
        for (i = 0; headers[i] != NULL; i += 2) {
            if (headers[i + 1] == NULL) break;
            httpc_request_write_header(conn, (char *)headers[i], (char *)headers[i + 1]);
        }
    }

    ret = httpc_request_write_header_finish(conn);
    if (ret < 0) {
        goto do_reconnect;
    }

    if (body && body_len > 0) {
        ret = httpc_request_write_data(conn, (uint8_t *)body, body_len);
        if (ret < 0) {
            goto do_reconnect;
        }
    }

    /* Read response; stop at Content-Length to keep the connection alive */
    {
        uint8_t buf[512];
        int total_read = 0;
        int header_end = 0;
        int hdr_steps  = 0;

        while (1) {
            int n = mbedtls_ssl_read(&tls->ctx, buf, sizeof(buf));
            if (n > 0) {
                if (resp_append(response, (const char *)buf, (size_t)n) != 0) {
                    break;
                }
                total_read += n;

                if (!header_end) {
                    size_t i;
                    for (i = 0; i < response->len; i++) {
                        char c = response->buf[i];
                        if      (c == '\r' && hdr_steps == 0) hdr_steps = 1;
                        else if (c == '\n' && hdr_steps == 1) hdr_steps = 2;
                        else if (c == '\r' && hdr_steps == 2) hdr_steps = 3;
                        else if (c == '\n' && hdr_steps == 3) { header_end = 1; break; }
                        else hdr_steps = 0;
                    }
                }

                if (header_end) {
                    char *cl_str = strstr(response->buf, "Content-Length:");
                    if (!cl_str) cl_str = strstr(response->buf, "content-length:");
                    if (cl_str) {
                        int content_len = atoi(cl_str + 15);
                        char *body_ptr  = strstr(response->buf, "\r\n\r\n");
                        if (body_ptr) {
                            int body_rx = total_read - (int)(body_ptr + 4 - response->buf);
                            if (body_rx >= content_len) break;
                        }
                    }
                }
            } else if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                httpc_conn_close(s->conn);
                httpc_conn_free(s->conn);
                s->conn = NULL;
                break;
            } else if (n == MBEDTLS_ERR_SSL_WANT_READ) {
                rtos_time_delay_ms(100);
            } else if (n == MBEDTLS_ERR_SSL_TIMEOUT) {
                break;
            } else {
                goto do_reconnect;
            }
        }

        RTK_LOGD(TAG, "session_post read total %d bytes\n", total_read);

        if (header_end) {
            char *body_ptr = strstr(response->buf, "\r\n\r\n");
            if (body_ptr) {
                body_ptr += 4;
                size_t blen = (size_t)(total_read - (int)(body_ptr - response->buf));
                memmove(response->buf, body_ptr, blen + 1);
                response->len = blen;
            }
        }
    }
    return 0;

do_reconnect:
    RTK_LOGW(TAG, "session_post error, reconnecting...\n");
    reconnect_attempts++;
    if (s->conn) {
        httpc_conn_close(s->conn);
        httpc_conn_free(s->conn);
        s->conn = NULL;
    }
    response->len = 0;
    if (response->buf) response->buf[0] = '\0';
    goto retry;
}

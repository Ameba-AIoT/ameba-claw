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
    uint16_t port;           /* default 80 */
    uint8_t  max_connections; /* default 4 */
} claw_http_server_config_t;

#define CLAW_HTTP_SERVER_DEFAULT_CONFIG() { .port = 80, .max_connections = 4 }

typedef enum {
    HTTP_GET    = 0,
    HTTP_POST   = 1,
    HTTP_DELETE = 2,
    HTTP_PUT    = 3,
} http_method_t;

typedef struct {
    http_method_t method;
    char          path[128];
    char          query[256];   /* part after '?' */
    const char   *body;         /* POST body (valid only during handler call) */
    size_t        body_len;
} claw_http_request_t;

typedef void (*claw_http_send_fn_t)(int sock, int status,
                                    const char *content_type,
                                    const char *body, size_t body_len);

typedef void (*claw_http_handler_fn_t)(const claw_http_request_t *req,
                                       claw_http_send_fn_t send_fn,
                                       int sock);

int claw_http_server_init(const claw_http_server_config_t *cfg);
int claw_http_server_start(void);
int claw_http_server_add_route(http_method_t method,
                                     const char *path,
                                     claw_http_handler_fn_t handler);

/* ---- WebSocket (RFC 6455) support, served on the same port as HTTP ---- *
 * A GET request to a registered WS path carrying a "Sec-WebSocket-Key"
 * header is upgraded in place (101 Switching Protocols); the connection
 * then stays open and is driven by the callbacks below. No second port. */

typedef struct claw_ws_conn claw_ws_conn_t;   /* opaque connection handle */

typedef void (*claw_ws_open_fn_t)(claw_ws_conn_t *c);
typedef void (*claw_ws_message_fn_t)(claw_ws_conn_t *c,
                                     const char *data, size_t len, int is_text);
typedef void (*claw_ws_close_fn_t)(claw_ws_conn_t *c);

/* Register a WebSocket endpoint. Any callback may be NULL. */
int claw_http_server_add_ws_route(const char *path,
                                   claw_ws_open_fn_t on_open,
                                   claw_ws_message_fn_t on_message,
                                   claw_ws_close_fn_t on_close);

/* Send a UTF-8 text frame to one connection. Thread-safe. 0 on success. */
int claw_ws_send_text(claw_ws_conn_t *c, const char *data, size_t len);

/* Send a text frame to every open connection on the given route. Thread-safe. */
int claw_ws_broadcast_text(const char *path, const char *data, size_t len);

void  claw_ws_set_userdata(claw_ws_conn_t *c, void *ud);
void *claw_ws_get_userdata(claw_ws_conn_t *c);

#ifdef __cplusplus
}
#endif

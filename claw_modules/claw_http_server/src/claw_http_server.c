/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ameba_soc.h"
#include "claw_http_server.h"
#include "ameba_claw_defs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "os_wrapper.h"
#include "memproc.h"

/* lwIP POSIX socket headers */
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <errno.h>

static const char *TAG = "claw_http_server";

/* ---- Route table ---- */

#define MAX_ROUTES           40
#define HTTP_BODY_BUF_SIZE   8192   /* max request body bytes per connection */
#define HTTP_CONN_TASK_STACK 6144   /* words per connection task; 3072 was too small for /updates + lwIP frames */
#define HTTP_CONN_TASK_PRIO  1

typedef struct {
    http_method_t         method;
    char                  path[128];
    claw_http_handler_fn_t handler;
} route_entry_t;

static route_entry_t  s_routes[MAX_ROUTES];
static int            s_route_count = 0;
static uint16_t       s_port        = 80;
static uint8_t        s_max_conn    = 4;

/* ---- WebSocket support ---- */

#define WS_GUID            "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define MAX_WS_ROUTES      8
#define MAX_WS_CONN        4
#define WS_RX_MAX          4096               /* max inbound frame payload */
#define WS_RX_CAP          (WS_RX_MAX + 16)   /* + room for frame header   */

typedef struct {
    char                 path[64];
    claw_ws_open_fn_t    on_open;
    claw_ws_message_fn_t on_message;
    claw_ws_close_fn_t   on_close;
} ws_route_t;

struct claw_ws_conn {
    int          sock;
    const char  *path;       /* points into s_ws_routes[].path (stable) */
    void        *userdata;
    volatile int in_use;
};

static ws_route_t    s_ws_routes[MAX_WS_ROUTES];
static int           s_ws_route_count = 0;
static struct claw_ws_conn s_ws_conns[MAX_WS_CONN];
static rtos_mutex_t  s_ws_mutex = NULL;   /* guards s_ws_conns + all WS sends */

/* --- tiny SHA-1 (RFC 3174), used only for the handshake accept key --- */
static void ws_sha1(const uint8_t *msg, size_t len, uint8_t out[20])
{
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t  buf[192];
    size_t   total = ((len + 8) / 64 + 1) * 64;
    if (total > sizeof(buf)) { _memset(out, 0, 20); return; }

    _memset(buf, 0, total);
    _memcpy(buf, msg, len);
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[total - 1 - i] = (uint8_t)(bits >> (8 * i));

    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)buf[off + 4 * i] << 24) | ((uint32_t)buf[off + 4 * i + 1] << 16) |
                   ((uint32_t)buf[off + 4 * i + 2] << 8) | (uint32_t)buf[off + 4 * i + 3];
        for (int i = 16; i < 80; i++) {
            uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (v << 1) | (v >> 31);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d);            k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                       k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);     k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                       k = 0xCA62C1D6; }
            uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++) {
        out[4 * i]     = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)(h[i]);
    }
}

static void ws_base64(const uint8_t *in, size_t len, char *out)
{
    static const char B64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, o = 0;
    for (i = 0; i + 3 <= len; i += 3) {
        uint32_t n = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = B64[(n >> 18) & 63]; out[o++] = B64[(n >> 12) & 63];
        out[o++] = B64[(n >> 6) & 63];  out[o++] = B64[n & 63];
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t n = (uint32_t)in[i] << 16;
        out[o++] = B64[(n >> 18) & 63]; out[o++] = B64[(n >> 12) & 63];
        out[o++] = '='; out[o++] = '=';
    } else if (rem == 2) {
        uint32_t n = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = B64[(n >> 18) & 63]; out[o++] = B64[(n >> 12) & 63];
        out[o++] = B64[(n >> 6) & 63];  out[o++] = '=';
    }
    out[o] = '\0';
}

static void ws_accept_key(const char *client_key, char *accept_out)
{
    char    cat[128];
    uint8_t sha[20];
    size_t  kl = strlen(client_key);
    if (kl > 64) kl = 64;
    _memcpy(cat, client_key, kl);
    _memcpy(cat + kl, WS_GUID, sizeof(WS_GUID) - 1);
    size_t total = kl + (sizeof(WS_GUID) - 1);
    ws_sha1((const uint8_t *)cat, total, sha);
    ws_base64(sha, 20, accept_out);
}

/* Case-insensitive lookup of an HTTP header value into out (NUL-terminated). */
static int http_header_get(const char *buf, const char *name,
                           char *out, size_t out_sz)
{
    size_t nl = strlen(name);
    const char *p = buf;
    while (*p) {
        size_t i;
        for (i = 0; i < nl; i++) {
            char a = p[i], b = name[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (!p[i] || a != b) break;
        }
        if (i == nl && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t o = 0;
            while (*v && *v != '\r' && *v != '\n' && o + 1 < out_sz) out[o++] = *v++;
            out[o] = '\0';
            return 1;
        }
        const char *nlp = strchr(p, '\n');
        if (!nlp) break;
        p = nlp + 1;
    }
    return 0;
}

static const ws_route_t *ws_find_route(const char *path)
{
    for (int i = 0; i < s_ws_route_count; i++)
        if (strcmp(s_ws_routes[i].path, path) == 0) return &s_ws_routes[i];
    return NULL;
}

/* Send fully (loop over short writes). Returns 0 on success, -1 on error. */
static int ws_send_all(int sock, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int n = lwip_send(sock, buf + off, len - off, 0);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

/* Frame a server->client message (never masked) and send it. */
static int ws_send_frame(int sock, uint8_t opcode, const uint8_t *payload, size_t len)
{
    uint8_t hdr[10];
    size_t  hl;
    hdr[0] = 0x80 | opcode;
    if (len < 126) {
        hdr[1] = (uint8_t)len; hl = 2;
    } else if (len < 65536) {
        hdr[1] = 126; hdr[2] = (uint8_t)(len >> 8); hdr[3] = (uint8_t)len; hl = 4;
    } else {
        hdr[1] = 127;
        hdr[2] = hdr[3] = hdr[4] = hdr[5] = 0;
        hdr[6] = (uint8_t)(len >> 24); hdr[7] = (uint8_t)(len >> 16);
        hdr[8] = (uint8_t)(len >> 8);  hdr[9] = (uint8_t)len;
        hl = 10;
    }
    if (ws_send_all(sock, hdr, hl) < 0) return -1;
    if (len && ws_send_all(sock, payload, len) < 0) return -1;
    return 0;
}

/* Send a control frame (pong/close) without holding s_ws_mutex during I/O.
 * Same reasoning as claw_ws_broadcast_text: blocking sends must not hold the
 * mutex because broadcast callers snapshot under it and must not be stalled. */
static void ws_send_ctl(struct claw_ws_conn *c, uint8_t op,
                        const uint8_t *p, size_t l)
{
    if (!s_ws_mutex || !c) return;
    rtos_mutex_take(s_ws_mutex, 0xFFFFFFFFUL);
    int sock = c->in_use ? c->sock : -1;
    rtos_mutex_give(s_ws_mutex);
    if (sock >= 0) ws_send_frame(sock, op, p, l);
}

static struct claw_ws_conn *ws_register(int sock, const char *path)
{
    struct claw_ws_conn *c = NULL;
    rtos_mutex_take(s_ws_mutex, 0xFFFFFFFFUL);
    for (int i = 0; i < MAX_WS_CONN; i++) {
        if (!s_ws_conns[i].in_use) {
            c = &s_ws_conns[i];
            c->sock = sock; c->path = path; c->userdata = NULL; c->in_use = 1;
            break;
        }
    }
    rtos_mutex_give(s_ws_mutex);
    return c;
}

static void ws_unregister(struct claw_ws_conn *c)
{
    rtos_mutex_take(s_ws_mutex, 0xFFFFFFFFUL);
    c->in_use = 0; c->sock = -1;
    rtos_mutex_give(s_ws_mutex);
}

/* Handshake (101) then drive the connection until it closes. The caller
 * owns the socket and closes it after this returns. */
static void ws_handshake_and_serve(int sock, const ws_route_t *route,
                                   const char *req_buf, int received,
                                   const char *ws_key)
{
    char accept[32];
    ws_accept_key(ws_key, accept);

    char resp[176];
    int rl = DiagSnPrintf(resp, sizeof(resp),
                          "HTTP/1.1 101 Switching Protocols\r\n"
                          "Upgrade: websocket\r\n"
                          "Connection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    if (ws_send_all(sock, (const uint8_t *)resp, rl) < 0) return;

    int ka = 1;
    lwip_setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));
    struct timeval st = {.tv_sec = 1, .tv_usec = 0};
    lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &st, sizeof(st));

    struct claw_ws_conn *c = ws_register(sock, route->path);
    if (!c) return;   /* table full */

    if (route->on_open) route->on_open(c);

    uint8_t *rx = (uint8_t *)rtos_mem_malloc(WS_RX_CAP);
    if (!rx) { if (route->on_close) route->on_close(c); ws_unregister(c); return; }
    size_t rxn = 0;

    /* Seed any bytes already received after the handshake headers (rare). */
    const char *bs = strstr(req_buf, "\r\n\r\n");
    if (bs) {
        bs += 4;
        size_t lo = (size_t)(req_buf + received - bs);
        if (lo > 0 && lo < WS_RX_CAP) { _memcpy(rx, bs, lo); rxn = lo; }
    }

    int alive = 1;
    while (alive) {
        /* Parse every complete frame currently buffered. */
        while (rxn >= 2) {
            uint8_t  b0 = rx[0], b1 = rx[1];
            int      fin = b0 & 0x80, opcode = b0 & 0x0f, masked = b1 & 0x80;
            uint64_t n = b1 & 0x7f;
            size_t   hl = 2;
            if (n == 126) { if (rxn < 4) break; n = ((uint64_t)rx[2] << 8) | rx[3]; hl = 4; }
            else if (n == 127) {
                if (rxn < 10) break;
                n = 0; for (int i = 0; i < 8; i++) n = (n << 8) | rx[2 + i];
                hl = 10;
            }
            if (masked) hl += 4;
            if (n > WS_RX_MAX) { alive = 0; break; }   /* oversized */
            if (rxn < hl + n) break;                   /* need more bytes */

            uint8_t *pl = rx + hl;
            if (masked) {
                const uint8_t *mk = rx + hl - 4;
                for (uint64_t i = 0; i < n; i++) pl[i] ^= mk[i & 3];
            }

            if (opcode == 0x1 || opcode == 0x2) {            /* text / binary */
                if (fin && route->on_message) {
                    uint8_t save = pl[n];
                    pl[n] = '\0';
                    route->on_message(c, (const char *)pl, (size_t)n, opcode == 0x1);
                    pl[n] = save;
                }
            } else if (opcode == 0x8) {                      /* close */
                ws_send_ctl(c, 0x8, NULL, 0);
                alive = 0;
            } else if (opcode == 0x9) {                      /* ping -> pong */
                ws_send_ctl(c, 0xA, pl, (size_t)n);
            }                                                /* 0xA pong: ignore */

            size_t consumed = hl + (size_t)n;
            rxn -= consumed;
            if (rxn > 0) memmove(rx, rx + consumed, rxn);
        }
        if (!alive) break;

        int r = lwip_recv(sock, rx + rxn, WS_RX_CAP - rxn, 0);
        if (r > 0) {
            rxn += (size_t)r;
            if (rxn >= WS_RX_CAP) break;   /* unframed overflow, drop conn */
        } else if (r == 0) {
            break;                          /* peer closed */
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)
                continue;                   /* recv timeout, keep waiting */
            break;
        }
    }

    rtos_mem_free(rx);
    if (route->on_close) route->on_close(c);
    ws_unregister(c);
}

/* ---- Response helper ---- */

static void http_send(int sock, int status, const char *content_type,
                      const char *body, size_t body_len)
{
    const char *status_str;
    char header[256];
    int  hlen;

    switch (status) {
    case 200: status_str = "OK";                    break;
    case 400: status_str = "Bad Request";           break;
    case 404: status_str = "Not Found";             break;
    case 413: status_str = "Payload Too Large";     break;
    case 415: status_str = "Unsupported Media Type"; break;
    default:  status_str = "Internal Server Error"; break;
    }

    hlen = DiagSnPrintf(header, sizeof(header),
                    "HTTP/1.1 %d %s\r\n"
                    "Content-Type: %s\r\n"
                    "Content-Length: %u\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Connection: close\r\n"
                    "\r\n",
                    status, status_str,
                    content_type ? content_type : "text/plain",
                    (unsigned)body_len);

    lwip_send(sock, header, hlen, 0);
    if (body && body_len > 0) {
        lwip_send(sock, body, body_len, 0);
    }
}

/* ---- Per-connection handler ---- */

#define HTTP_REQ_BUF_SIZE   1024
#define HTTP_MAX_BODY_SIZE  CLAW_HTTP_MAX_BODY_SIZE

static void handle_connection(int sock)
{
    char req_buf[HTTP_REQ_BUF_SIZE];   /* stack — 1 KB, safe for 4 KB task stack */
    char *body_buf = NULL;             /* heap — allocated only when body present */

    int received = lwip_recv(sock, req_buf, sizeof(req_buf) - 1, 0);
    if (received <= 0) {
        lwip_close(sock);
        return;
    }
    req_buf[received] = '\0';

    /* Parse request line: "METHOD /path?query HTTP/1.1" */
    claw_http_request_t req;
    _memset(&req, 0, sizeof(req));

    char method_str[8];
    char raw_path[256];

    if (_sscanf_ss(req_buf, "%7s %255s", method_str, raw_path) != 2) {
        http_send(sock, 400, "text/plain", "Bad Request", 11);
        lwip_close(sock);
        return;
    }

    if (strcmp(method_str, "GET") == 0) {
        req.method = HTTP_GET;
    } else if (strcmp(method_str, "POST") == 0) {
        req.method = HTTP_POST;
    } else if (strcmp(method_str, "DELETE") == 0) {
        req.method = HTTP_DELETE;
    } else if (strcmp(method_str, "PUT") == 0) {
        req.method = HTTP_PUT;
    } else {
        http_send(sock, 400, "text/plain", "Method Not Allowed", 18);
        lwip_close(sock);
        return;
    }

    /* Split path and query */
    char *qmark = strchr(raw_path, '?');
    if (qmark) {
        size_t plen = (size_t)(qmark - raw_path);
        if (plen >= sizeof(req.path)) plen = sizeof(req.path) - 1;
        _memcpy(req.path, raw_path, plen);
        req.path[plen] = '\0';
        strlcpy(req.query, qmark + 1, sizeof(req.query));
    } else {
        strlcpy(req.path, raw_path, sizeof(req.path));
        req.query[0] = '\0';
    }

    /* WebSocket upgrade: GET to a registered WS path with a handshake key.
     * Served in place on this same port; the socket stays open for frames. */
    if (req.method == HTTP_GET) {
        const ws_route_t *wr = ws_find_route(req.path);
        if (wr) {
            char ws_key[64];
            if (http_header_get(req_buf, "Sec-WebSocket-Key", ws_key, sizeof(ws_key))) {
                ws_handshake_and_serve(sock, wr, req_buf, received, ws_key);
                lwip_close(sock);
                return;
            }
        }
    }

    /* Parse Content-Length */
    const char *cl_hdr = strstr(req_buf, "Content-Length:");
    if (!cl_hdr) cl_hdr = strstr(req_buf, "content-length:");
    size_t content_length = 0;
    if (cl_hdr) {
        long cl_val = strtol(cl_hdr + 15, NULL, 10);
        if (cl_val < 0 || cl_val > (long)HTTP_MAX_BODY_SIZE) {
            http_send(sock, 413, "text/plain", "Payload Too Large", 17);
            lwip_close(sock);
            return;
        }
        content_length = (size_t)cl_val;
    }

    /* Body starts after the blank line \r\n\r\n */
    const char *body_start = strstr(req_buf, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
    }

    /* Allocate body buffer only when a body is actually present */
    if (content_length > 0 && body_start) {
        body_buf = (char *)rtos_mem_malloc(content_length + 1);
        if (!body_buf) {
            http_send(sock, 503, "text/plain", "Out of memory", 13);
            lwip_close(sock);
            return;
        }

        size_t already = (size_t)(req_buf + received - body_start);
        if (already > content_length) already = content_length;
        _memcpy(body_buf, body_start, already);

        while (already < content_length) {
            int extra = lwip_recv(sock, body_buf + already, content_length - already, 0);
            if (extra <= 0) break;
            already += (size_t)extra;
        }
        body_buf[already] = '\0';
        /* Reject incomplete body — prevents handlers from processing truncated
         * JSON (e.g. config save mid-write on a dropped connection). */
        if (already < content_length) {
            RTK_LOGW("http_srv", "body truncated (%u/%u), closing\n",
                     (unsigned)already, (unsigned)content_length);
            rtos_mem_free(body_buf);
            lwip_close(sock);
            return;
        }
        req.body     = body_buf;
        req.body_len = already;
    } else {
        req.body     = "";
        req.body_len = 0;
    }

    /* Route lookup */
    for (int i = 0; i < s_route_count; i++) {
        if (s_routes[i].method == req.method &&
            strcmp(s_routes[i].path, req.path) == 0) {
            s_routes[i].handler(&req, http_send, sock);
            rtos_mem_free(body_buf);
            lwip_close(sock);
            return;
        }
    }

    /* No route matched */
    http_send(sock, 404, "text/plain", "Not Found", 9);
    rtos_mem_free(body_buf);
    lwip_close(sock);
}

/* ---- Per-connection task ---- */

static void connection_task(void *arg)
{
    int sock = (int)(intptr_t)arg;
    handle_connection(sock);
    rtos_task_delete(NULL);
}

/* ---- Listener task ---- */

static void listener_task(void *arg)
{
    (void)arg;

    int listen_sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        RTK_LOGE(TAG, "socket() failed\n");
        rtos_task_delete(NULL);
        return;
    }

    int opt = 1;
    lwip_setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    _memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(s_port);

    if (lwip_bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        RTK_LOGE(TAG, "bind() failed on port %u\n", s_port);
        lwip_close(listen_sock);
        rtos_task_delete(NULL);
        return;
    }

    if (lwip_listen(listen_sock, s_max_conn) < 0) {
        RTK_LOGE(TAG, "listen() failed\n");
        lwip_close(listen_sock);
        rtos_task_delete(NULL);
        return;
    }

    RTK_LOGI(TAG, "Listening on port %u\n", s_port);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = lwip_accept(listen_sock,
                                      (struct sockaddr *)&client_addr,
                                      &client_len);
        if (client_sock < 0) {
            RTK_LOGE(TAG, "accept() failed\n");
            rtos_time_delay_ms(100);
            continue;
        }

        /* 5-second receive timeout prevents stalled clients from blocking tasks */
        struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
        lwip_setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        RTK_LOGD(TAG, "accepted connection\n");
        if (rtos_task_create(NULL, "http_conn", connection_task,
                              (void *)(intptr_t)client_sock,
                              HTTP_CONN_TASK_STACK * 4,
                              HTTP_CONN_TASK_PRIO) != RTK_SUCCESS) {
            RTK_LOGE(TAG, "cannot create connection task, closing\n");
            lwip_close(client_sock);
        }
    }

    lwip_close(listen_sock);
    rtos_task_delete(NULL);
}

/* ---- Public API ---- */

int claw_http_server_init(const claw_http_server_config_t *cfg)
{
    if (!cfg) return RTK_ERR_BADARG;
    s_port     = cfg->port;
    s_max_conn = cfg->max_connections;
    s_route_count = 0;

    s_ws_route_count = 0;
    for (int i = 0; i < MAX_WS_CONN; i++) { s_ws_conns[i].in_use = 0; s_ws_conns[i].sock = -1; }
    if (!s_ws_mutex && rtos_mutex_create(&s_ws_mutex) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "ws mutex create failed\n");
        return RTK_ERR_NOMEM;
    }

    RTK_LOGI(TAG, "init port=%u max_conn=%u\n", s_port, s_max_conn);
    return RTK_SUCCESS;
}

int claw_http_server_start(void)
{
    if (rtos_task_create(NULL, "http_server", listener_task,
                         NULL, 4096, 1) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "rtos_task_create listener_task failed\n");
        return RTK_FAIL;
    }
    RTK_LOGI(TAG, "started\n");
    return RTK_SUCCESS;
}

int claw_http_server_add_route(http_method_t method,
                                     const char *path,
                                     claw_http_handler_fn_t handler)
{
    if (!path || !handler) return RTK_ERR_BADARG;
    if (s_route_count >= MAX_ROUTES) {
        RTK_LOGE(TAG, "route table full (%d), cannot add: %s\n", MAX_ROUTES, path);
        return RTK_ERR_NOMEM;
    }
    s_routes[s_route_count].method  = method;
    strlcpy(s_routes[s_route_count].path, path, sizeof(s_routes[0].path));
    s_routes[s_route_count].handler = handler;
    s_route_count++;
    RTK_LOGD(TAG, "route added: %s %s\n",
             method == HTTP_GET    ? "GET"    :
             method == HTTP_POST   ? "POST"   :
             method == HTTP_DELETE ? "DELETE" : "PUT", path);
    return RTK_SUCCESS;
}

/* ---- WebSocket public API ---- */

int claw_http_server_add_ws_route(const char *path,
                                   claw_ws_open_fn_t on_open,
                                   claw_ws_message_fn_t on_message,
                                   claw_ws_close_fn_t on_close)
{
    if (!path) return RTK_ERR_BADARG;
    if (s_ws_route_count >= MAX_WS_ROUTES) return RTK_ERR_NOMEM;
    strlcpy(s_ws_routes[s_ws_route_count].path, path, sizeof(s_ws_routes[0].path));
    s_ws_routes[s_ws_route_count].on_open    = on_open;
    s_ws_routes[s_ws_route_count].on_message = on_message;
    s_ws_routes[s_ws_route_count].on_close   = on_close;
    s_ws_route_count++;
    RTK_LOGD(TAG, "ws route added: %s\n", path);
    return RTK_SUCCESS;
}

int claw_ws_send_text(claw_ws_conn_t *c, const char *data, size_t len)
{
    if (!c || !s_ws_mutex) return RTK_ERR_BADARG;
    rtos_mutex_take(s_ws_mutex, 0xFFFFFFFFUL);
    int r = (c->in_use) ? ws_send_frame(c->sock, 0x1, (const uint8_t *)data, len) : -1;
    rtos_mutex_give(s_ws_mutex);
    return r;
}

int claw_ws_broadcast_text(const char *path, const char *data, size_t len)
{
    if (!path || !s_ws_mutex) return RTK_ERR_BADARG;

    /* Snapshot active socket FDs under the mutex, then release before sending.
     * Holding s_ws_mutex during blocking lwip_send() (SO_SNDTIMEO) causes all
     * concurrent callers to serialise and can block HTTP handlers for seconds.
     * Race risk (fd reuse after snapshot) is negligible in this embedded context;
     * a failed send simply returns -1 and the connection is cleaned up naturally. */
    int socks[MAX_WS_CONN];
    int count = 0;
    rtos_mutex_take(s_ws_mutex, 0xFFFFFFFFUL);
    for (int i = 0; i < MAX_WS_CONN; i++) {
        if (s_ws_conns[i].in_use && strcmp(s_ws_conns[i].path, path) == 0)
            socks[count++] = s_ws_conns[i].sock;
    }
    rtos_mutex_give(s_ws_mutex);

    for (int i = 0; i < count; i++)
        ws_send_frame(socks[i], 0x1, (const uint8_t *)data, len);

    return RTK_SUCCESS;
}

void claw_ws_set_userdata(claw_ws_conn_t *c, void *ud) { if (c) c->userdata = ud; }
void *claw_ws_get_userdata(claw_ws_conn_t *c) { return c ? c->userdata : NULL; }

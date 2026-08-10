/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ameba_soc.h"
#include "llm_agent_http.h"
#include "httpc/httpc.h"
#include "platform_stdlib.h"
#include <lwip/sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "os_wrapper.h"
#include "basic_types.h"
#include "cJSON.h"
#include "ameba_claw_defs.h"

#define TAG "llm_http"

/* TLS handshake needs ~30 KB of stack (mbedTLS certificate parsing, key exchange).
 * Run it in a short-lived task so the LLM agent task stack stays small. */
#define TLS_HANDSHAKE_STACK  (32 * 1024)

struct tls_connect_ctx {
    struct httpc_conn *conn;
    char              *host;
    uint16_t           port;
    uint32_t           timeout_s;
    int                result;
    rtos_sema_t        done;
};

static void tls_connect_task(void *arg)
{
    struct tls_connect_ctx *ctx = (struct tls_connect_ctx *)arg;
    ctx->result = httpc_conn_connect(ctx->conn, ctx->host, ctx->port, ctx->timeout_s);
    rtos_sema_give(ctx->done);
    rtos_task_delete(NULL);
}

/* Parse "host:port" → writes stripped host into out_host, returns port.
 * Sets *use_tls=0 when a non-443 port is found (plain-HTTP debug path). */
static uint16_t parse_host_port(const char *host_port, char *out_host,
                                size_t out_sz, int *use_tls)
{
    const char *colon = strrchr(host_port, ':');
    *use_tls = 1;
    if (colon && colon > host_port) {
        int p = atoi(colon + 1);
        if (p > 0 && p <= 65535) {
            size_t hlen = (size_t)(colon - host_port);
            if (hlen >= out_sz) hlen = out_sz - 1;
            _memcpy(out_host, host_port, hlen);
            out_host[hlen] = '\0';
            if (p != 443) *use_tls = 0;
            return (uint16_t)p;
        }
    }
    strlcpy(out_host, host_port, out_sz);
    return 443;
}

static int plain_write_all(int sock, const void *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = lwip_send(sock, (const char *)buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return (int)len;
}

static int tls_connect_offloaded(struct httpc_conn *conn, char *host,
                                 uint16_t port, uint32_t timeout_s)
{
    struct tls_connect_ctx ctx;
    rtos_task_t task;

    ctx.conn      = conn;
    ctx.host      = host;
    ctx.port      = port;
    ctx.timeout_s = timeout_s;
    ctx.result    = -1;

    if (rtos_sema_create_binary(&ctx.done) != RTK_SUCCESS) {
        return -1;
    }
    if (rtos_task_create(&task, "tls_hs", tls_connect_task, &ctx,
                         TLS_HANDSHAKE_STACK, 2) != RTK_SUCCESS) {
        rtos_sema_delete(ctx.done);
        return -1;
    }
    rtos_sema_take(ctx.done, RTOS_MAX_DELAY);
    rtos_sema_delete(ctx.done);
    return ctx.result;
}

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
 * neither of which work correctly with lwIP sockets on this platform.
 * This replacement uses lwip_select() and lwip_recv() directly.
 */
static int llm_ssl_recv_timeout(void *ctx, unsigned char *buf, size_t len,
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

static int tls_write_all(mbedtls_ssl_context *ctx, const void *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = mbedtls_ssl_write(ctx, (const uint8_t *)buf + sent, len - sent);
        if (n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            rtos_time_delay_ms(10);
            continue;
        }
        if (n <= 0) return n;
        sent += (size_t)n;
    }
    return (int)len;
}

/* Install custom TLS read timeout using lwip_select + lwip_recv.
 * Must be called after httpc_conn_connect (which does TLS handshake). */
static void install_custom_tls_timeout(struct httpc_conn *conn, int timeout_ms)
{
    struct httpc_tls_internal *tls = (struct httpc_tls_internal *)conn->tls;
    if (!tls) {
        return;
    }
    mbedtls_ssl_conf_read_timeout(&tls->conf, timeout_ms);
    mbedtls_ssl_set_bio(&tls->ctx, &conn->sock,
                        mbedtls_net_send, NULL, llm_ssl_recv_timeout);
}

/* ---- Streaming SSE accumulator ------------------------------------------
 * Parses the raw SSE byte stream on-the-fly while the HTTP body is being
 * read, accumulating only the assembled content (~8 KB) instead of the full
 * raw SSE stream (~100-200 KB). This eliminates the LLM_HTTP_RESP_MAX_SIZE
 * bottleneck for streaming responses.
 * ----------------------------------------------------------------------- */

#define SSE_MAX_TC_H 8

typedef struct {
    /* partial-line accumulator (heap-allocated, grows as needed) */
    char  *linebuf;
    size_t linelen;
    size_t linecap;

    /* current SSE event type ("" or "error") */
    char   event_type[32];

    /* assembled content */
    char  *content;    size_t content_len, content_cap;
    char  *reasoning;  size_t reasoning_len, reasoning_cap;  /* GLM delta.reasoning_content */
    char  *finish;
    char  *resp_id;
    char  *resp_model;
    char  *error_payload; /* set on event: error */

    /* tool calls */
    struct { char *id, *name, *args; size_t args_len, args_cap; } tc[SSE_MAX_TC_H];
    int tc_count;

    /* chunked transfer-encoding state */
    long   chunk_remaining;
    int    skip_crlf;
    char   chunk_hdr[20];
    size_t chunk_hdr_len;

    /* flags */
    int is_chunked;
    int is_event_error;
    int done;
} sse_stream_acc_t;

static int sse_h_append(char **dst, size_t *dlen, size_t *dcap,
                        const char *src, size_t slen)
{
    if (*dlen + slen + 1 > *dcap) {
        size_t nc = *dcap ? *dcap * 2 : 4096;
        while (nc < *dlen + slen + 1) nc *= 2;
        char *nb = (char *)realloc(*dst, nc);
        if (!nb) return -1;
        *dst = nb; *dcap = nc;
    }
    memcpy(*dst + *dlen, src, slen);
    *dlen += slen;
    (*dst)[*dlen] = '\0';
    return 0;
}

static void sse_acc_process_line(sse_stream_acc_t *acc, const char *line, size_t llen)
{
    if (llen == 0) { acc->event_type[0] = '\0'; return; }

    if (llen >= 7 && memcmp(line, "event: ", 7) == 0) {
        size_t et = llen - 7;
        if (et >= sizeof(acc->event_type)) et = sizeof(acc->event_type) - 1;
        memcpy(acc->event_type, line + 7, et);
        acc->event_type[et] = '\0';
        return;
    }
    if (llen < 6 || memcmp(line, "data: ", 6) != 0) return;

    const char *pay  = line + 6;
    size_t      plen = llen - 6;

    if (plen == 6 && memcmp(pay, "[DONE]", 6) == 0) { acc->done = 1; return; }

    if (acc->event_type[0]) {
        if (strcmp(acc->event_type, "error") == 0) {
            acc->is_event_error = 1;
            acc->error_payload  = (char *)malloc(plen + 1);
            if (acc->error_payload) { memcpy(acc->error_payload, pay, plen); acc->error_payload[plen] = '\0'; }
            acc->done = 1;
        }
        acc->event_type[0] = '\0';
        return;
    }

    char *tmp = (char *)malloc(plen + 1);
    if (!tmp) return;
    memcpy(tmp, pay, plen); tmp[plen] = '\0';
    cJSON *chunk = cJSON_Parse(tmp); free(tmp);
    if (!chunk) return;

    if (!acc->resp_id)    { cJSON *j = cJSON_GetObjectItem(chunk, "id");    if (j && cJSON_IsString(j)) acc->resp_id    = strdup(j->valuestring); }
    if (!acc->resp_model) { cJSON *j = cJSON_GetObjectItem(chunk, "model"); if (j && cJSON_IsString(j)) acc->resp_model = strdup(j->valuestring); }

    cJSON *choices = cJSON_GetObjectItem(chunk, "choices");
    cJSON *ch0     = choices ? cJSON_GetArrayItem(choices, 0) : NULL;
    if (ch0) {
        cJSON *fr = cJSON_GetObjectItem(ch0, "finish_reason");
        if (fr && cJSON_IsString(fr) && fr->valuestring[0]) { free(acc->finish); acc->finish = strdup(fr->valuestring); }
        cJSON *delta = cJSON_GetObjectItem(ch0, "delta");
        if (delta) {
            cJSON *c = cJSON_GetObjectItem(delta, "content");
            if (c && cJSON_IsString(c) && c->valuestring[0])
                sse_h_append(&acc->content, &acc->content_len, &acc->content_cap, c->valuestring, strlen(c->valuestring));
            cJSON *rc = cJSON_GetObjectItem(delta, "reasoning_content");
            if (rc && cJSON_IsString(rc) && rc->valuestring[0])
                sse_h_append(&acc->reasoning, &acc->reasoning_len, &acc->reasoning_cap, rc->valuestring, strlen(rc->valuestring));
            cJSON *tcs = cJSON_GetObjectItem(delta, "tool_calls");
            if (tcs && cJSON_IsArray(tcs)) {
                cJSON *tci;
                cJSON_ArrayForEach(tci, tcs) {
                    cJSON *idx_j = cJSON_GetObjectItem(tci, "index");
                    int    idx   = idx_j ? (int)cJSON_GetNumberValue(idx_j) : 0;
                    if (idx < 0 || idx >= SSE_MAX_TC_H) continue;
                    if (idx >= acc->tc_count) acc->tc_count = idx + 1;
                    cJSON *id_j = cJSON_GetObjectItem(tci, "id");
                    if (id_j && cJSON_IsString(id_j) && !acc->tc[idx].id) acc->tc[idx].id = strdup(id_j->valuestring);
                    cJSON *func = cJSON_GetObjectItem(tci, "function");
                    if (func) {
                        cJSON *nm = cJSON_GetObjectItem(func, "name");
                        if (nm && cJSON_IsString(nm) && !acc->tc[idx].name) acc->tc[idx].name = strdup(nm->valuestring);
                        cJSON *args = cJSON_GetObjectItem(func, "arguments");
                        if (args && cJSON_IsString(args) && args->valuestring[0])
                            sse_h_append(&acc->tc[idx].args, &acc->tc[idx].args_len, &acc->tc[idx].args_cap, args->valuestring, strlen(args->valuestring));
                    }
                }
            }
        }
    }
    cJSON_Delete(chunk);
}

static void sse_acc_feed_lines(sse_stream_acc_t *acc, const char *data, size_t len)
{
    const char *p = data, *end = data + len;
    while (p < end && !acc->done) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        size_t to_copy = nl ? (size_t)(nl - p) : (size_t)(end - p);
        int    complete = (nl != NULL);
        /* grow linebuf on demand so long SSE lines are never truncated */
        size_t needed = acc->linelen + to_copy + 1;
        if (needed > acc->linecap) {
            size_t nc = acc->linecap ? acc->linecap * 2 : 1024;
            while (nc < needed) nc *= 2;
            char *nb = (char *)realloc(acc->linebuf, nc);
            if (!nb) {
                acc->linelen = 0;
                p = nl ? nl + 1 : end;
                continue;
            }
            acc->linebuf = nb; acc->linecap = nc;
        }
        if (to_copy > 0) { memcpy(acc->linebuf + acc->linelen, p, to_copy); acc->linelen += to_copy; }
        p += to_copy + (complete ? 1 : 0);
        if (complete) {
            if (acc->linelen > 0 && acc->linebuf[acc->linelen - 1] == '\r') acc->linelen--;
            acc->linebuf[acc->linelen] = '\0';
            sse_acc_process_line(acc, acc->linebuf, acc->linelen);
            acc->linelen = 0;
        }
    }
}

static void sse_acc_feed(sse_stream_acc_t *acc, const char *data, size_t len)
{
    const char *p = data, *end = data + len;
    while (p < end && !acc->done) {
        if (!acc->is_chunked) { sse_acc_feed_lines(acc, p, (size_t)(end - p)); break; }
        if (acc->skip_crlf > 0) {
            acc->skip_crlf--; p++; continue;
        }
        if (acc->chunk_remaining > 0) {
            size_t avail = (size_t)(end - p);
            size_t feed  = ((size_t)acc->chunk_remaining < avail) ? (size_t)acc->chunk_remaining : avail;
            sse_acc_feed_lines(acc, p, feed);
            acc->chunk_remaining -= (long)feed;
            p += feed;
            if (acc->chunk_remaining == 0) acc->skip_crlf = 2;
            continue;
        }
        /* Reading chunk header line */
        if (*p != '\n') {
            if (acc->chunk_hdr_len < sizeof(acc->chunk_hdr) - 1) acc->chunk_hdr[acc->chunk_hdr_len++] = *p;
            p++;
        } else {
            acc->chunk_hdr[acc->chunk_hdr_len] = '\0';
            long sz = strtol(acc->chunk_hdr, NULL, 16);
            acc->chunk_hdr_len = 0; p++;
            if (sz <= 0) { acc->done = 1; } else { acc->chunk_remaining = sz; }
        }
    }
}

static int sse_acc_build_response(sse_stream_acc_t *acc, llm_http_resp_t *resp)
{
    if (acc->is_event_error && acc->error_payload) {
        size_t elen = strlen(acc->error_payload);
        char  *nb   = (char *)rtos_mem_malloc(elen + 1);
        if (!nb) return -1;
        memcpy(nb, acc->error_payload, elen + 1);
        rtos_mem_free(resp->buf);
        resp->buf = nb; resp->len = elen; resp->cap = elen + 1;
        return 0;
    }
    cJSON *out    = cJSON_CreateObject();
    cJSON *chlist = cJSON_CreateArray();
    cJSON *ch0out = cJSON_CreateObject();
    cJSON *msg    = cJSON_CreateObject();
    if (!out || !chlist || !ch0out || !msg) {
        cJSON_Delete(out); cJSON_Delete(chlist); cJSON_Delete(ch0out); cJSON_Delete(msg); return -1;
    }
    cJSON_AddStringToObject(out,   "id",     acc->resp_id    ? acc->resp_id    : "chatcmpl-sse");
    cJSON_AddStringToObject(out,   "model",  acc->resp_model ? acc->resp_model : "");
    cJSON_AddStringToObject(out,   "object", "chat.completion");
    cJSON_AddStringToObject(msg,   "role",   "assistant");
    if (acc->content && acc->content[0]) cJSON_AddStringToObject(msg, "content", acc->content);
    else                                 cJSON_AddNullToObject   (msg, "content");
    if (acc->reasoning && acc->reasoning[0]) cJSON_AddStringToObject(msg, "reasoning_content", acc->reasoning);
    if (acc->tc_count > 0) {
        cJSON *tc_arr = cJSON_CreateArray();
        if (!tc_arr) { cJSON_Delete(chlist); cJSON_Delete(ch0out); cJSON_Delete(msg); cJSON_Delete(out); return -1; }
        int i;
        for (i = 0; i < acc->tc_count; i++) {
            cJSON *t    = cJSON_CreateObject();
            cJSON *func = cJSON_CreateObject();
            if (!t || !func) { cJSON_Delete(t); cJSON_Delete(func); cJSON_Delete(tc_arr); cJSON_Delete(chlist); cJSON_Delete(ch0out); cJSON_Delete(msg); cJSON_Delete(out); return -1; }
            cJSON_AddStringToObject(t,    "id",        acc->tc[i].id   ? acc->tc[i].id   : "call_sse");
            cJSON_AddStringToObject(t,    "type",      "function");
            cJSON_AddStringToObject(func, "name",      acc->tc[i].name ? acc->tc[i].name : "");
            cJSON_AddStringToObject(func, "arguments", acc->tc[i].args ? acc->tc[i].args : "{}");
            cJSON_AddItemToObject(t, "function", func);
            cJSON_AddItemToArray(tc_arr, t);
        }
        cJSON_AddItemToObject(msg, "tool_calls", tc_arr);
    }
    cJSON_AddItemToObject   (ch0out, "message",       msg);
    cJSON_AddStringToObject (ch0out, "finish_reason", acc->finish ? acc->finish : "stop");
    cJSON_AddItemToArray    (chlist, ch0out);
    cJSON_AddItemToObject   (out,    "choices",       chlist);

    char  *assembled = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (!assembled) return -1;
    size_t alen = strlen(assembled);
    char  *nb   = (char *)rtos_mem_malloc(alen + 1);
    if (!nb) { free(assembled); return -1; }
    memcpy(nb, assembled, alen + 1); free(assembled);
    rtos_mem_free(resp->buf);
    resp->buf = nb; resp->len = alen; resp->cap = alen + 1;
    RTK_LOGD(TAG, "sse_build: assembled=%u bytes tc_count=%d\n", (unsigned)alen, acc->tc_count);
    if (acc->tc_count > 0)
        RTK_LOGD(TAG, "tc[0] name=%s args_len=%u\n",
                 acc->tc[0].name ? acc->tc[0].name : "(null)", (unsigned)acc->tc[0].args_len);
    return 0;
}

static void sse_acc_free(sse_stream_acc_t *acc)
{
    free(acc->linebuf);
    free(acc->content); free(acc->reasoning); free(acc->finish); free(acc->resp_id);
    free(acc->resp_model); free(acc->error_payload);
    { int i; for (i = 0; i < SSE_MAX_TC_H; i++) { free(acc->tc[i].id); free(acc->tc[i].name); free(acc->tc[i].args); } }
}

/* ---- End streaming SSE accumulator -------------------------------------- */

int llm_http_resp_init(llm_http_resp_t *resp)
{
    if (!resp) {
        return -1;
    }

    resp->buf = (char *)rtos_mem_malloc(LLM_HTTP_RESP_INIT_SIZE);
    if (!resp->buf) {
        return -2;
    }
    resp->buf[0] = '\0';
    resp->len = 0;
    resp->cap = LLM_HTTP_RESP_INIT_SIZE;
    resp->ttfb_ms = 0;
    resp->cap_hdr = NULL;
    resp->cap_hdr_val[0] = '\0';
    return 0;
}

void llm_http_resp_free(llm_http_resp_t *resp)
{
    if (!resp) {
        return;
    }
    if (resp->buf) {
        rtos_mem_free(resp->buf);
        resp->buf = NULL;
    }
    resp->len = 0;
    resp->cap = 0;
}

static int resp_append(llm_http_resp_t *resp, const char *data, size_t data_len)
{
    if (!resp || !data || data_len == 0) {
        return 0;
    }

    if (resp->len + data_len + 1 > resp->cap) {
        size_t new_cap = resp->cap;
        while (new_cap < resp->len + data_len + 1) {
            new_cap *= 2;
        }
        if (new_cap > LLM_HTTP_RESP_MAX_SIZE) {
            new_cap = LLM_HTTP_RESP_MAX_SIZE;
        }
        if (resp->len + data_len + 1 > new_cap) {
            RTK_LOGE(TAG, "response too large (%u)\n", (unsigned)(resp->len + data_len));
            return -1;
        }
        /* Use rtos_mem_* allocator consistently — resp->buf was allocated
         * with rtos_mem_malloc in llm_http_resp_init, so realloc (which uses
         * the standard heap) would cause undefined behavior on this platform. */
        char *new_buf = (char *)rtos_mem_malloc(new_cap);
        if (!new_buf) {
            return -2;
        }
        if (resp->buf && resp->len > 0) {
            memcpy(new_buf, resp->buf, resp->len);
        }
        rtos_mem_free(resp->buf);
        resp->buf = new_buf;
        resp->cap = new_cap;
    }

    _memcpy(resp->buf + resp->len, data, data_len);
    resp->len += data_len;
    resp->buf[resp->len] = '\0';
    return 0;
}

/* auth_type: 0=Anthropic(x-api-key+anthropic-version), 1=Bearer, 2=no-auth, 3=x-api-key-only
 * One connect→send→recv attempt. The request body is NOT freed here — the retry
 * wrapper (llm_http_post_internal) owns the body so it survives across retries. */
static int llm_http_post_once(const char *host, const char *resource,
                              const char *body, size_t body_len,
                              const char *api_key,
                              int auth_type,
                              llm_http_resp_t *response)
{
    struct httpc_conn *conn = NULL;
    struct httpc_tls_internal *tls;
    char *hdr = NULL;
    uint8_t *read_buf = NULL;
    int hdr_len;
    int ret;

    if (!host || !resource || !response) {
        return -1;
    }

    char clean_host[128];
    int  use_tls;
    uint16_t port = parse_host_port(host, clean_host, sizeof(clean_host), &use_tls);

    hdr = (char *)malloc(800);
    if (!hdr) {
        return -1;
    }
    read_buf = (uint8_t *)malloc(512);
    if (!read_buf) {
        free(hdr);
        return -1;
    }

    if (use_tls) {
        /* TLS handshake retry: server-side throttling can return WANT_READ
         * (-0x6900) on the first attempt — let one transient failure recover. */
        const int   delays_ms[] = { 0, 1000, 3000 };
        const int   attempts    = (int)(sizeof(delays_ms) / sizeof(delays_ms[0]));
        int         attempt;
        int         connected   = 0;

        for (attempt = 0; attempt < attempts; attempt++) {
            if (delays_ms[attempt]) {
                rtos_time_delay_ms(delays_ms[attempt]);
            }
            conn = httpc_conn_new(HTTPC_SECURE_TLS, NULL, NULL, NULL);
            if (!conn) {
                RTK_LOGE(TAG, "httpc_conn_new failed\n");
                free(hdr);
                free(read_buf);
                return -1;
            }
            if (tls_connect_offloaded(conn, clean_host, port, 30) == 0) {
                connected = 1;
                break;
            }
            RTK_LOGW(TAG, "connect to %s failed (attempt %d/%d)\n",
                     host, attempt + 1, attempts);
            httpc_conn_free(conn);
            conn = NULL;
        }
        if (!connected) {
            free(hdr);
            free(read_buf);
            return -2;
        }
        /* Install custom TLS recv timeout. In streaming mode this is the
         * inter-chunk gap (tokens arrive continuously, so it never trips); in
         * non-stream mode it must cover the entire generation, which for a
         * 16K-token output can exceed two minutes — hence 300 s. */
        install_custom_tls_timeout(conn, CLAW_AGENT_LLM_RECV_TIMEOUT_MS);
    } else {
        /* Plain HTTP — used for local debug proxies (e.g. ccglass). */
        const int attempts = 3;
        int connected = 0;
        for (int attempt = 0; attempt < attempts; attempt++) {
            if (attempt) rtos_time_delay_ms(1000);
            conn = httpc_conn_new(HTTPC_SECURE_NONE, NULL, NULL, NULL);
            if (!conn) {
                RTK_LOGE(TAG, "httpc_conn_new (plain) failed\n");
                free(hdr);
                free(read_buf);
                return -1;
            }
            if (httpc_conn_connect(conn, clean_host, port, 10) == 0) {
                connected = 1;
                break;
            }
            RTK_LOGW(TAG, "connect to %s:%u failed (attempt %d/%d)\n",
                     clean_host, port, attempt + 1, attempts);
            httpc_conn_free(conn);
            conn = NULL;
        }
        if (!connected) {
            free(hdr);
            free(read_buf);
            return -2;
        }
        struct timeval rcv_tv = { .tv_sec = CLAW_AGENT_LLM_PLAIN_RECV_TIMEOUT_MS / 1000, .tv_usec = 0 };
        lwip_setsockopt(conn->sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));
    }

    tls = use_tls ? (struct httpc_tls_internal *)conn->tls : NULL;

    /* Build and send HTTP request directly via mbedtls — the httpc header-write
     * APIs do not reliably emit Content-Length, causing the server to wait for
     * the body indefinitely and close after its idle timeout. */
    if (auth_type == 1) {
        hdr_len = DiagSnPrintf(hdr, 800,
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %u\r\n"
            "Connection: close\r\n"
            "Authorization: Bearer %s\r\n"
            "\r\n",
            resource, host, (unsigned)body_len, api_key ? api_key : "");
    } else if (auth_type == 2) {
        hdr_len = DiagSnPrintf(hdr, 800,
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %u\r\n"
            "Connection: close\r\n"
            "\r\n",
            resource, host, (unsigned)body_len);
    } else if (auth_type == 3) {
        /* Verbatim Authorization header value (e.g. "QQBot {token}") */
        hdr_len = DiagSnPrintf(hdr, 800,
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %u\r\n"
            "Connection: close\r\n"
            "Authorization: %s\r\n"
            "\r\n",
            resource, host, (unsigned)body_len, api_key ? api_key : "");
    } else {
        hdr_len = DiagSnPrintf(hdr, 800,
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %u\r\n"
            "Connection: close\r\n"
            "x-api-key: %s\r\n"
            "anthropic-version: 2023-06-01\r\n"
            "\r\n",
            resource, host, (unsigned)body_len, api_key ? api_key : "");
    }

    if (hdr_len < 0 || hdr_len >= 800) {
        RTK_LOGE(TAG, "header buffer overflow (%d)\n", hdr_len);
        ret = -3;
        goto cleanup;
    }

    if (use_tls) {
        /* Always try to merge header + body into one contiguous buffer so both
         * land in the same TLS write burst.  Some servers (e.g. open.bigmodel.cn
         * behind a load balancer) reset the TCP connection if the body is not
         * received in the same burst as the headers — even when Content-Length
         * is set correctly and the body arrives milliseconds later.
         *
         * If the merged malloc fails (e.g. body is hundreds of KB and heap is
         * tight), fall back to writing header then body separately, accepting
         * that some servers may reject the request. */
        size_t total_len = (size_t)hdr_len + (body ? body_len : 0);
        char *req_buf = total_len <= 256 * 1024 ? (char *)malloc(total_len) : NULL;
        if (req_buf) {
            memcpy(req_buf, hdr, (size_t)hdr_len);
            if (body && body_len > 0) memcpy(req_buf + hdr_len, body, body_len);
            int wr = tls_write_all(&tls->ctx, req_buf, total_len);
            free(req_buf);
            if (wr < 0) {
                RTK_LOGE(TAG, "ssl_write request failed (ret=%d)\n", wr);
                ret = -3;
                goto cleanup;
            }
        } else {
            /* Fallback: header then body separately */
            if (tls_write_all(&tls->ctx, hdr, (size_t)hdr_len) < 0) {
                RTK_LOGE(TAG, "ssl_write header failed\n");
                ret = -3;
                goto cleanup;
            }
            if (body && body_len > 0) {
                int bwr = tls_write_all(&tls->ctx, body, body_len);
                if (bwr < 0) {
                    RTK_LOGE(TAG, "ssl_write body failed (err=-0x%04x body_len=%u)\n",
                             (unsigned)(-bwr), (unsigned)body_len);
                    ret = -3;
                    goto cleanup;
                }
            }
        }
    } else {
        if (plain_write_all(conn->sock, hdr, (size_t)hdr_len) < 0) {
            RTK_LOGE(TAG, "plain_write headers failed\n");
            ret = -3;
            goto cleanup;
        }
        if (body && body_len > 0) {
            if (plain_write_all(conn->sock, body, body_len) < 0) {
                RTK_LOGE(TAG, "plain_write body failed\n");
                ret = -3;
                goto cleanup;
            }
        }
    }

    {
        int total_read = 0;
        int header_end = 0;
        int header_end_steps = 0;
        int is_sse = 0;
        sse_stream_acc_t *acc = NULL;
        int timed_out = 0;   /* loop ended on a recv timeout (not a fast close) */

        /* Latency instrumentation. DTimestamp_Get() is a 1 µs counter.
         * t_sent is stamped here, right after the full request was written. */
        u32 t_sent     = DTimestamp_Get();
        u32 t_first    = 0;
        int got_first  = 0;

        while (1) {
            int n;
            if (use_tls) {
                n = mbedtls_ssl_read(&tls->ctx, read_buf, 512);
            } else {
                n = lwip_recv(conn->sock, read_buf, 512, 0);
                /* n == 0: peer closed cleanly. n < 0: timeout or reset —
                 * log the reason so an empty response is not silently
                 * mistaken for a clean end-of-stream downstream. */
                if (n < 0) {
                    int err = errno;
                    /* SO_RCVTIMEO fired. On this SDK's lwIP 2.1.2 a recv timeout
                     * surfaces as EWOULDBLOCK (ERR_TIMEOUT -> err_to_errno ==
                     * EWOULDBLOCK; see component/lwip/.../api/err.c), never
                     * ETIMEDOUT — that code is not even in lwIP's errno table.
                     * EAGAIN/ETIMEDOUT are accepted defensively so a different
                     * stack or future errno mapping still classifies a timeout
                     * as -5 (no retry) instead of -4 (retryable fast close). */
                    if (err == EAGAIN || err == EWOULDBLOCK || err == ETIMEDOUT) {
                        RTK_LOGE(TAG, "plain HTTP read timeout\n");
                        timed_out = 1;
                    } else {
                        RTK_LOGE(TAG, "plain HTTP recv error: errno=%d\n", err);
                    }
                }
            }
            if (n > 0) {
                total_read += n;

                /* Time-to-first-byte: interval between request sent and the
                 * first byte of the response (u32 µs subtraction wraps cleanly
                 * for a single wrap, well within our <=300 s window). */
                if (!got_first) {
                    got_first = 1;
                    t_first = DTimestamp_Get();
                    u32 ttfb = t_first - t_sent;
                    response->ttfb_ms = (uint32_t)(ttfb / 1000);
                    RTK_LOGI(NOTAG, "timing: TTFB %u ms\n", (unsigned)response->ttfb_ms);
                }

                /* SSE streaming mode: process body bytes without buffering */
                if (is_sse && acc) {
                    sse_acc_feed(acc, (const char *)read_buf, (size_t)n);
                    if (acc->done) break;
                    continue;
                }

                /* Pre-header or non-SSE mode: buffer everything */
                if (resp_append(response, (const char *)read_buf, (size_t)n) != 0) {
                    break;
                }

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

                    if (header_end) {
                        /* Check if SSE response; if so, switch to streaming mode */
                        if (strstr(response->buf, "text/event-stream")) {
                            acc = (sse_stream_acc_t *)calloc(1, sizeof(sse_stream_acc_t));
                            if (acc) {
                                char *te = strstr(response->buf, "Transfer-Encoding:");
                                if (!te) te = strstr(response->buf, "transfer-encoding:");
                                if (te && strstr(te, "chunked")) acc->is_chunked = 1;
                                /* Feed partial body already in buffer */
                                char *body = strstr(response->buf, "\r\n\r\n");
                                if (body) {
                                    body += 4;
                                    size_t partial = (size_t)(response->buf + response->len - body);
                                    if (partial > 0) sse_acc_feed(acc, body, partial);
                                }
                                /* Clear response buf — assembled JSON written later */
                                response->len = 0;
                                response->buf[0] = '\0';
                                is_sse = 1;
                                if (acc->done) break;
                            }
                        }
                    }
                }

                if (header_end && !is_sse) {
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
                }
            } else if (n == 0 || (!use_tls && n < 0) ||
                       (use_tls && n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)) {
                break;
            } else if (use_tls && n == MBEDTLS_ERR_SSL_WANT_READ) {
                rtos_time_delay_ms(100);
            } else if (use_tls && n == MBEDTLS_ERR_SSL_TIMEOUT) {
                RTK_LOGE(TAG, "TLS read timeout\n");
                timed_out = 1;
                break;
            } else if (use_tls) {
                RTK_LOGE(TAG, "ssl_read error: %d\n", n);
                break;
            } else {
                break;
            }
        }

        RTK_LOGD(TAG, "read total %d bytes\n", total_read);

        /* Streaming only: total wall time from request sent to last byte.
         * (Non-stream's "total" ~= TTFB, since the server buffers the whole
         * body and the first byte already arrives at the end of generation.) */
        if (is_sse && got_first) {
            u32 total_us = DTimestamp_Get() - t_sent;
            RTK_LOGI(NOTAG, "timing: total %u ms\n", (unsigned)(total_us / 1000));
        }

        if (is_sse && acc) {
            /* Build assembled JSON into response->buf */
            int build_rc = sse_acc_build_response(acc, response);
            int sse_done = acc->done;
            sse_acc_free(acc);
            free(acc);
            if (build_rc != 0) {
                /* OOM / JSON assembly failed: sse_acc_build_response left
                 * response->buf empty. Surface a real error instead of
                 * returning 0 and letting the caller feed an empty string into
                 * cJSON_Parse — which would report a misleading "JSON parse
                 * failed" and hide the actual out-of-memory cause. */
                ret = -1;
                RTK_LOGE(TAG, "SSE build failed (OOM) -> -1\n");
                goto cleanup;
            }
            if (timed_out && !sse_done) {
                /* The stream was cut short by a recv timeout before the
                 * terminating [DONE]/finish_reason arrived. The assembled JSON
                 * is a truncated answer; do NOT pass it off as a complete
                 * success (the header was already received, so the generic
                 * total_read/header_end guard below would otherwise let it
                 * return 0). -5 = timeout, not retried. */
                ret = -5;
                RTK_LOGE(TAG, "SSE stream truncated by timeout (done=%d) -> -5\n", sse_done);
                goto cleanup;
            }
        } else if (header_end) {
            int is_chunked = 0;
            char *te = strstr(response->buf, "Transfer-Encoding:");
            if (!te) {
                te = strstr(response->buf, "transfer-encoding:");
            }
            if (te && strstr(te, "chunked")) {
                is_chunked = 1;
            }

            char *body_start = strstr(response->buf, "\r\n\r\n");
            if (body_start) {
                body_start += 4;
                size_t body_len2 = total_read - (size_t)(body_start - response->buf);
                _memmove(response->buf, body_start, body_len2 + 1);
                response->len = body_len2;

                if (is_chunked) {
                    /* Decode chunked transfer encoding in-place */
                    char *src = response->buf;
                    char *dst = response->buf;
                    char *end = response->buf + response->len;
                    while (src < end) {
                        char *nl = memchr(src, '\n', (size_t)(end - src));
                        if (!nl) {
                            break;
                        }
                        long chunk_sz = strtol(src, NULL, 16);
                        src = nl + 1;
                        if (chunk_sz <= 0) {
                            break;
                        }
                        if (src + chunk_sz > end) {
                            chunk_sz = end - src;
                        }
                        _memmove(dst, src, (size_t)chunk_sz);
                        dst += chunk_sz;
                        src += chunk_sz;
                        if (src + 1 < end && src[0] == '\r' && src[1] == '\n') {
                            src += 2;
                        } else if (src < end && src[0] == '\n') {
                            src += 1;
                        }
                    }
                    *dst = '\0';
                    response->len = (size_t)(dst - response->buf);
                }
            }
        }

        /* The connection delivered no complete HTTP response: either nothing
         * arrived, or the header terminator (CRLFCRLF) was never seen. Report a
         * distinct transport error instead of leaving an empty buffer for the
         * caller to feed into cJSON_Parse — which would surface the misleading
         * "JSON parse failed".
         *
         * -4 (fast close: clean FIN / RST / TLS close-notify) is retryable: the
         *     peer dropped us promptly, a fresh connection usually works.
         * -5 (recv timeout) is NOT retried: the peer accepted the connection but
         *     hung, so retrying would only multiply the (long) timeout wait. */
        if (total_read == 0 || !header_end) {
            ret = timed_out ? -5 : -4;
            RTK_LOGE(TAG, "empty/incomplete HTTP response (read=%d, header_end=%d, timed_out=%d) -> %d\n",
                     total_read, header_end, timed_out, ret);
            goto cleanup;
        }
    }

    httpc_conn_close(conn);
    httpc_conn_free(conn);
    free(hdr);
    free(read_buf);
    return 0;

cleanup:
    httpc_conn_close(conn);
    httpc_conn_free(conn);
    free(hdr);
    free(read_buf);
    return ret;
}

/* Number of full connect→send→recv attempts before giving up on an empty
 * (closed/reset) response. The proxy/upstream occasionally accepts the request
 * but drops the connection before delivering the body (-4); a fresh connection
 * — new socket, new ephemeral port, after a short back-off that lets a TIME_WAIT
 * PCB recycle — usually succeeds. Only -4 is retried; hard errors and successes
 * return immediately. */
#define LLM_HTTP_EMPTY_RETRIES 3

/* early_free: if non-NULL, *early_free is freed (and set NULL) once all attempts
 * are done. Unlike a free-right-after-write, this keeps the body available for
 * retries; the heap is reclaimed a little later (negligible on this target).
 * Honors the _ef contract: *early_free is NULL after this returns. */
static int llm_http_post_internal(const char *host, const char *resource,
                                  const char *body, size_t body_len,
                                  const char *api_key,
                                  int auth_type,
                                  char **early_free,
                                  llm_http_resp_t *response)
{
    int ret = -4;
    int attempt;

    for (attempt = 0; attempt < LLM_HTTP_EMPTY_RETRIES; attempt++) {
        if (attempt > 0) {
            RTK_LOGW(TAG, "empty response, retrying (%d/%d)\n",
                     attempt + 1, LLM_HTTP_EMPTY_RETRIES);
            /* Reuse the caller's buffer; only the content from the dropped
             * attempt needs clearing. */
            if (response) {
                response->len = 0;
                if (response->buf) {
                    response->buf[0] = '\0';
                }
            }
            rtos_time_delay_ms(500);
        }
        ret = llm_http_post_once(host, resource, body, body_len,
                                 api_key, auth_type, response);
        if (ret != -4) {
            break;  /* success or a hard error — do not retry */
        }
    }

    if (early_free && *early_free) {
        free(*early_free);
        *early_free = NULL;
    }
    return ret;
}

int llm_http_post(const char *host, const char *resource,
                  const char *body, size_t body_len,
                  const char *api_key,
                  llm_http_resp_t *response)
{
    return llm_http_post_internal(host, resource, body, body_len, api_key, 0, NULL, response);
}

int llm_http_post_bearer(const char *host, const char *resource,
                         const char *body, size_t body_len,
                         const char *api_key,
                         llm_http_resp_t *response)
{
    return llm_http_post_internal(host, resource, body, body_len, api_key, 1, NULL, response);
}

int llm_http_post_no_auth(const char *host, const char *resource,
                           const char *body, size_t body_len,
                           llm_http_resp_t *response)
{
    return llm_http_post_internal(host, resource, body, body_len, NULL, 2, NULL, response);
}

int llm_http_post_bearer_ef(const char *host, const char *resource,
                             char **body_pp, size_t body_len,
                             const char *api_key,
                             llm_http_resp_t *response)
{
    return llm_http_post_internal(host, resource, *body_pp, body_len, api_key, 1, body_pp, response);
}

int llm_http_post_auth(const char *host, const char *resource,
                       const char *body, size_t body_len,
                       const char *auth_value,
                       llm_http_resp_t *response)
{
    return llm_http_post_internal(host, resource, body, body_len, auth_value, 3, NULL, response);
}

int llm_http_get_auth(const char *host, const char *resource,
                      const char *auth_value,
                      llm_http_resp_t *response)
{
    if (!host || !resource || !auth_value || !response) return -1;

    char clean_host[128];
    int  use_tls;
    uint16_t port = parse_host_port(host, clean_host, sizeof(clean_host), &use_tls);

    struct httpc_conn *conn = NULL;
    if (use_tls) {
        const int delays_ms[] = { 0, 1000, 3000 };
        int attempts = (int)(sizeof(delays_ms) / sizeof(delays_ms[0]));
        for (int i = 0; i < attempts; i++) {
            if (delays_ms[i]) rtos_time_delay_ms(delays_ms[i]);
            conn = httpc_conn_new(HTTPC_SECURE_TLS, NULL, NULL, NULL);
            if (!conn) return -1;
            if (tls_connect_offloaded(conn, clean_host, port, 30) == 0) break;
            RTK_LOGW(TAG, "get_auth: connect attempt %d/%d failed\n", i + 1, attempts);
            httpc_conn_free(conn);
            conn = NULL;
        }
        if (!conn) return -2;
        install_custom_tls_timeout(conn, 30000);
    } else {
        conn = httpc_conn_new(HTTPC_SECURE_NONE, NULL, NULL, NULL);
        if (!conn) return -1;
        if (httpc_conn_connect(conn, clean_host, port, 10) != 0) {
            httpc_conn_free(conn); return -2;
        }
    }

    struct httpc_tls_internal *tls = use_tls ? (struct httpc_tls_internal *)conn->tls : NULL;

    /* Build GET request with Authorization header */
    char *hdr = (char *)malloc(800);
    if (!hdr) { httpc_conn_close(conn); httpc_conn_free(conn); return -1; }
    int hdr_len = DiagSnPrintf(hdr, 800,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "Authorization: %s\r\n"
        "\r\n",
        resource, host, auth_value);
    if (hdr_len < 0 || hdr_len >= 800) {
        free(hdr); httpc_conn_close(conn); httpc_conn_free(conn); return -3;
    }

    int wr;
    if (use_tls)
        wr = tls_write_all(&tls->ctx, hdr, (size_t)hdr_len);
    else
        wr = plain_write_all(conn->sock, hdr, (size_t)hdr_len);
    free(hdr);
    if (wr < 0) { httpc_conn_close(conn); httpc_conn_free(conn); return -3; }

    /* Read full response into response buffer, then strip HTTP header */
    uint8_t *read_buf = (uint8_t *)malloc(512);
    if (!read_buf) { httpc_conn_close(conn); httpc_conn_free(conn); return -1; }

    int ret = -4;
    int total_read = 0;
    int header_end = 0;

    while (1) {
        int n;
        if (use_tls) {
            n = mbedtls_ssl_read(&tls->ctx, read_buf, 512);
        } else {
            n = lwip_recv(conn->sock, read_buf, 512, 0);
        }
        if (n > 0) {
            total_read += n;
            if (resp_append(response, (const char *)read_buf, (size_t)n) != 0) break;

            if (!header_end && strstr(response->buf, "\r\n\r\n"))
                header_end = 1;

            if (header_end) {
                char *cl_str = strstr(response->buf, "Content-Length:");
                if (!cl_str) cl_str = strstr(response->buf, "content-length:");
                if (cl_str) {
                    int content_len = atoi(cl_str + 15);
                    char *body_ptr = strstr(response->buf, "\r\n\r\n");
                    if (body_ptr) {
                        int body_got = total_read - (int)(body_ptr + 4 - response->buf);
                        if (body_got >= content_len) break;
                    }
                }
            }
        } else if (n == 0 ||
                   (!use_tls && n < 0) ||
                   (use_tls && n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)) {
            break;
        } else if (use_tls && n == MBEDTLS_ERR_SSL_WANT_READ) {
            rtos_time_delay_ms(100);
        } else {
            break;
        }
    }

    free(read_buf);

    if (total_read > 0 && header_end) {
        char *body_start = strstr(response->buf, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            size_t body_len2 = (size_t)(total_read - (int)(body_start - response->buf));
            _memmove(response->buf, body_start, body_len2 + 1);
            response->len = body_len2;
        }
        ret = 0;
    }

    httpc_conn_close(conn);
    httpc_conn_free(conn);
    return ret;
}

/* ---- Persistent session ---- */

struct llm_http_session {
    struct httpc_conn *conn;
    char host[128];
};

static struct httpc_conn *llm_session_connect(llm_http_session_t *s)
{
    if (s->conn) {
        httpc_conn_close(s->conn);
        httpc_conn_free(s->conn);
        s->conn = NULL;
    }

    /* Mirror the retry logic from llm_http_post_internal: WANT_READ on the
     * first handshake is a transient server-side condition; back off and retry
     * rather than failing immediately. */
    const int delays_ms[] = { 0, 1000, 3000 };
    const int attempts    = (int)(sizeof(delays_ms) / sizeof(delays_ms[0]));
    for (int i = 0; i < attempts; i++) {
        if (delays_ms[i]) {
            rtos_time_delay_ms(delays_ms[i]);
        }
        s->conn = httpc_conn_new(HTTPC_SECURE_TLS, NULL, NULL, NULL);
        if (!s->conn) {
            return NULL;
        }
        if (tls_connect_offloaded(s->conn, s->host, 443, 30) == 0) {
            install_custom_tls_timeout(s->conn, 40000);
            return s->conn;
        }
        RTK_LOGW(TAG, "session connect to %s failed (attempt %d/%d)\n",
                 s->host, i + 1, attempts);
        httpc_conn_free(s->conn);
        s->conn = NULL;
    }
    RTK_LOGE(TAG, "session connect to %s failed after %d attempts\n", s->host, attempts);
    return NULL;
}

llm_http_session_t *llm_http_session_open(const char *host)
{
    llm_http_session_t *s;
    if (!host) {
        return NULL;
    }
    s = (llm_http_session_t *)malloc(sizeof(*s));
    if (!s) {
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    strncpy(s->host, host, sizeof(s->host) - 1);
    if (!llm_session_connect(s)) {
        free(s);
        return NULL;
    }
    return s;
}

void llm_http_session_close(llm_http_session_t *s)
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

int llm_http_session_post_no_auth(llm_http_session_t *s,
                                  const char *resource,
                                  const char *body, size_t body_len,
                                  llm_http_resp_t *response)
{
    struct httpc_tls_internal *tls;
    char *hdr = NULL;
    uint8_t *read_buf = NULL;
    int hdr_len;
    int reconnect_attempts = 0;

    if (!s || !resource || !response) {
        return -1;
    }

    hdr = (char *)malloc(512);
    if (!hdr) {
        return -1;
    }
    read_buf = (uint8_t *)malloc(512);
    if (!read_buf) {
        free(hdr);
        return -1;
    }

retry:
    if (reconnect_attempts > 3) {
        RTK_LOGE(TAG, "session_post: too many reconnect attempts\n");
        free(hdr);
        free(read_buf);
        return -2;
    }

    if (!s->conn) {
        if (!llm_session_connect(s)) {
            reconnect_attempts++;
            rtos_time_delay_ms(1000);
            goto retry;
        }
    }
    tls = (struct httpc_tls_internal *)s->conn->tls;

    hdr_len = DiagSnPrintf(hdr, 512,
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %u\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        resource, s->host, (unsigned)body_len);

    if (hdr_len < 0 || hdr_len >= 512) {
        RTK_LOGE(TAG, "session header overflow\n");
        free(hdr);
        free(read_buf);
        return -3;
    }

    /* Merge header + body — same reason as in llm_http_post_once. */
    {
        size_t total_len = (size_t)hdr_len + (body ? body_len : 0);
        char *req_buf = (char *)malloc(total_len);
        if (!req_buf) { goto do_reconnect; }
        memcpy(req_buf, hdr, (size_t)hdr_len);
        if (body && body_len > 0) memcpy(req_buf + hdr_len, body, body_len);
        int wr = tls_write_all(&tls->ctx, req_buf, total_len);
        free(req_buf);
        if (wr < 0) goto do_reconnect;
    }

    {
        int total_read = 0;
        int header_end = 0;
        int hdr_steps  = 0;

        while (1) {
            int n = mbedtls_ssl_read(&tls->ctx, read_buf, 512);
            if (n > 0) {
                if (resp_append(response, (const char *)read_buf, (size_t)n) != 0) break;
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

        RTK_LOGD(TAG, "session_post read %d bytes\n", total_read);

        if (header_end) {
            char *body_ptr = strstr(response->buf, "\r\n\r\n");
            if (body_ptr) {
                body_ptr += 4;
                size_t blen = response->len - (size_t)(body_ptr - response->buf);
                memmove(response->buf, body_ptr, blen + 1);
                response->len = blen;
            }
        }
    }
    free(hdr);
    free(read_buf);
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

/* ---- HTTPS GET → VFS file ------------------------------------------------
 * Opens a TLS connection, issues a GET, streams the response body directly to
 * a VFS file in 512-byte chunks.  Never buffers the whole body in RAM.
 * ----------------------------------------------------------------------- */

int llm_http_get_to_file(const char *host, const char *resource,
                          const char *dest_path,
                          size_t max_bytes, size_t *out_bytes)
{
    if (!host || !resource || !dest_path) return -1;

    char clean_host[128];
    int  use_tls;
    uint16_t port = parse_host_port(host, clean_host, sizeof(clean_host), &use_tls);

    /* Connect */
    struct httpc_conn *conn = NULL;
    if (use_tls) {
        const int delays_ms[] = { 0, 1000, 3000 };
        int attempts = (int)(sizeof(delays_ms) / sizeof(delays_ms[0]));
        for (int i = 0; i < attempts; i++) {
            if (delays_ms[i]) rtos_time_delay_ms(delays_ms[i]);
            conn = httpc_conn_new(HTTPC_SECURE_TLS, NULL, NULL, NULL);
            if (!conn) return -1;
            if (tls_connect_offloaded(conn, clean_host, port, 30) == 0) break;
            RTK_LOGW(TAG, "get_to_file: connect attempt %d/%d failed\n", i + 1, attempts);
            httpc_conn_free(conn);
            conn = NULL;
        }
        if (!conn) return -2;
        install_custom_tls_timeout(conn, 60000); /* 60 s — file downloads can be large */
    } else {
        conn = httpc_conn_new(HTTPC_SECURE_NONE, NULL, NULL, NULL);
        if (!conn) return -1;
        if (httpc_conn_connect(conn, clean_host, port, 10) != 0) {
            httpc_conn_free(conn); return -2;
        }
    }

    struct httpc_tls_internal *tls = use_tls ? (struct httpc_tls_internal *)conn->tls : NULL;

    /* Send GET request */
    /* Header buffer sized dynamically to handle long resource paths
     * (e.g. WeChat CDN URLs with 800+ byte encrypted_query_param). */
    size_t hdr_need = strlen(resource) + strlen(host) + 64;
    char *hdr = (char *)malloc(hdr_need);
    if (!hdr) { httpc_conn_close(conn); httpc_conn_free(conn); return -1; }
    int hdr_len = DiagSnPrintf(hdr, hdr_need,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        resource, host);
    if (hdr_len < 0 || (size_t)hdr_len >= hdr_need) {
        free(hdr); httpc_conn_close(conn); httpc_conn_free(conn); return -3;
    }

    int wr;
    if (use_tls) {
        wr = tls_write_all(&tls->ctx, hdr, (size_t)hdr_len);
    } else {
        wr = plain_write_all(conn->sock, hdr, (size_t)hdr_len);
    }
    free(hdr);
    if (wr < 0) { httpc_conn_close(conn); httpc_conn_free(conn); return -3; }

    /* Read response — header first, then stream body to VFS */
    uint8_t *chunk = (uint8_t *)malloc(512);
    if (!chunk) { httpc_conn_close(conn); httpc_conn_free(conn); return -1; }

    /* Accumulate HTTP header (until \r\n\r\n) — max 4 KB */
    char  *hdr_buf  = (char *)malloc(4096);
    size_t hdr_got  = 0;
    int    hdr_done = 0;
    if (!hdr_buf) { free(chunk); httpc_conn_close(conn); httpc_conn_free(conn); return -1; }

    FILE  *fp         = NULL;
    size_t bytes_out  = 0;
    int    ret        = -4;
    long   content_len = -1; /* from Content-Length header */

    while (!hdr_done) {
        int n;
        if (use_tls) n = mbedtls_ssl_read(&tls->ctx, chunk, 512);
        else         n = lwip_recv(conn->sock, chunk, 512, 0);
        if (n <= 0) goto done;

        /* Append to header buffer until we see \r\n\r\n */
        size_t copy = (size_t)n;
        if (hdr_got + copy > 4095) copy = 4095 - hdr_got;
        memcpy(hdr_buf + hdr_got, chunk, copy);
        hdr_got += copy;
        hdr_buf[hdr_got] = '\0';

        char *sep = strstr(hdr_buf, "\r\n\r\n");
        if (!sep) continue;

        hdr_done = 1;
        /* Parse Content-Length */
        char *cl = strstr(hdr_buf, "Content-Length:");
        if (!cl) cl = strstr(hdr_buf, "content-length:");
        if (cl) content_len = atol(cl + 15);

        /* Safety cap */
        if (max_bytes > 0 && content_len > 0 && (size_t)content_len > max_bytes) {
            RTK_LOGE(TAG, "get_to_file: content-length %ld > max %u\n",
                     content_len, (unsigned)max_bytes);
            ret = -6;
            goto done;
        }

        /* Create destination VFS directory (best-effort) */
        {
            char dir_tmp[256];
            strlcpy(dir_tmp, dest_path, sizeof(dir_tmp));
            char *slash = strrchr(dir_tmp, '/');
            if (slash && slash != dir_tmp) {
                *slash = '\0';
                mkdir(dir_tmp, 0777);
            }
        }

        fp = fopen(dest_path, "wb");
        if (!fp) {
            RTK_LOGE(TAG, "get_to_file: cannot create %s\n", dest_path);
            ret = -7;
            goto done;
        }

        /* Write the partial body already in chunk (bytes after the \r\n\r\n) */
        char   *body_start = sep + 4;
        size_t  remaining  = (size_t)(hdr_buf + hdr_got - body_start);
        /* also any bytes past `copy` in the original chunk */
        size_t  extra = (size_t)n - copy;

        if (remaining > 0) {
            fwrite(body_start, 1, remaining, fp);
            bytes_out += remaining;
        }
        if (extra > 0) {
            /* bytes from chunk that didn't fit into hdr_buf */
            fwrite(chunk + copy, 1, extra, fp);
            bytes_out += extra;
        }
    }

    /* Stream remainder of body */
    while (1) {
        if (content_len >= 0 && (long)bytes_out >= content_len) break;
        int n;
        if (use_tls) n = mbedtls_ssl_read(&tls->ctx, chunk, 512);
        else         n = lwip_recv(conn->sock, chunk, 512, 0);
        if (n <= 0) break;
        fwrite(chunk, 1, (size_t)n, fp);
        bytes_out += (size_t)n;
        if (max_bytes > 0 && bytes_out > max_bytes) {
            RTK_LOGE(TAG, "get_to_file: exceeded max_bytes %u\n", (unsigned)max_bytes);
            ret = -6;
            goto done;
        }
    }
    ret = 0;

done:
    if (fp) fclose(fp);
    free(hdr_buf);
    free(chunk);
    httpc_conn_close(conn);
    httpc_conn_free(conn);
    if (out_bytes) *out_bytes = bytes_out;
    if (ret != 0 && dest_path) remove(dest_path); /* clean partial file on error */
    RTK_LOGI(TAG, "get_to_file: %s -> %s (%u bytes, ret=%d)\n",
             resource, dest_path, (unsigned)bytes_out, ret);
    return ret;
}

/* ---- General-purpose HTTPS request (cap_http_request) ---------------------- */

int llm_http_request(const char *method,
                     const char *host, const char *resource,
                     const char *extra_headers,
                     const char *body, size_t body_len,
                     int *out_status,
                     llm_http_resp_t *response)
{
    if (!method || !host || !resource || !response) return -1;

    /* Heap-allocate clean_host: host can be up to 255 chars (from cap_http_request's
     * 256-byte buffer); clean_host[128] would silently truncate long hostnames,
     * causing the TCP connection to go to a different server than the Host: header. */
    char *clean_host = (char *)malloc(256);
    if (!clean_host) return -1;
    int  use_tls;
    uint16_t port = parse_host_port(host, clean_host, 256, &use_tls);

    struct httpc_conn *conn = NULL;
    if (use_tls) {
        const int delays_ms[] = { 0, 1000, 3000 };
        int attempts = (int)(sizeof(delays_ms) / sizeof(delays_ms[0]));
        for (int i = 0; i < attempts; i++) {
            if (delays_ms[i]) rtos_time_delay_ms(delays_ms[i]);
            conn = httpc_conn_new(HTTPC_SECURE_TLS, NULL, NULL, NULL);
            if (!conn) { free(clean_host); return -1; }
            if (tls_connect_offloaded(conn, clean_host, port, 30) == 0) break;
            RTK_LOGW(TAG, "http_request: connect %s attempt %d/%d failed\n", host, i + 1, attempts);
            httpc_conn_free(conn);
            conn = NULL;
        }
        if (!conn) { free(clean_host); return -2; }
        install_custom_tls_timeout(conn, 30000);
    } else {
        conn = httpc_conn_new(HTTPC_SECURE_NONE, NULL, NULL, NULL);
        if (!conn) { free(clean_host); return -1; }
        if (httpc_conn_connect(conn, clean_host, port, 10) != 0) {
            free(clean_host); httpc_conn_free(conn); return -2;
        }
        struct timeval rcv_tv = { .tv_sec = 30, .tv_usec = 0 };
        lwip_setsockopt(conn->sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));
    }
    free(clean_host); /* no longer needed — connection already established */

    struct httpc_tls_internal *tls = use_tls ? (struct httpc_tls_internal *)conn->tls : NULL;

    /* Build request header — fixed base + extra_headers.
     * Size from actual inputs: method + resource + host can each be up to
     * hundreds of bytes (cap_http_request allocates 512 for resource, 256 for
     * host); a fixed 512-byte base would overflow for long URLs. */
    size_t extra_len = extra_headers ? strlen(extra_headers) : 0;
    size_t hdr_cap = strlen(method) + strlen(resource) + strlen(host)
                     + extra_len + 128;
    char *hdr = (char *)malloc(hdr_cap);
    if (!hdr) { httpc_conn_close(conn); httpc_conn_free(conn); return -1; }

    int hdr_len;
    if (body && body_len > 0) {
        hdr_len = DiagSnPrintf(hdr, (int)hdr_cap,
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Length: %u\r\n"
            "Connection: close\r\n"
            "%s"
            "\r\n",
            method, resource, host, (unsigned)body_len,
            extra_headers ? extra_headers : "");
    } else {
        hdr_len = DiagSnPrintf(hdr, (int)hdr_cap,
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Connection: close\r\n"
            "%s"
            "\r\n",
            method, resource, host,
            extra_headers ? extra_headers : "");
    }

    if (hdr_len < 0 || (size_t)hdr_len >= hdr_cap) {
        free(hdr); httpc_conn_close(conn); httpc_conn_free(conn); return -3;
    }

    /* Send header + body */
    int wr;
    if (use_tls) {
        wr = tls_write_all(&tls->ctx, hdr, (size_t)hdr_len);
        if (wr >= 0 && body && body_len > 0)
            wr = tls_write_all(&tls->ctx, body, body_len);
    } else {
        wr = plain_write_all(conn->sock, hdr, (size_t)hdr_len);
        if (wr >= 0 && body && body_len > 0)
            wr = plain_write_all(conn->sock, body, body_len);
    }
    free(hdr);
    if (wr < 0) { httpc_conn_close(conn); httpc_conn_free(conn); return -3; }

    /* Read response */
    uint8_t *read_buf = (uint8_t *)malloc(512);
    if (!read_buf) { httpc_conn_close(conn); httpc_conn_free(conn); return -1; }

    int ret = -4;
    int total_read = 0;
    int header_end = 0;
    int is_head = (strcmp(method, "HEAD") == 0);

    while (1) {
        int n;
        if (use_tls) n = mbedtls_ssl_read(&tls->ctx, read_buf, 512);
        else         n = lwip_recv(conn->sock, read_buf, 512, 0);

        if (n > 0) {
            total_read += n;
            if (resp_append(response, (const char *)read_buf, (size_t)n) != 0) break;

            if (!header_end && strstr(response->buf, "\r\n\r\n")) {
                header_end = 1;
                if (is_head) break;
            }

            if (header_end && !is_head) {
                char *cl_str = strstr(response->buf, "Content-Length:");
                if (!cl_str) cl_str = strstr(response->buf, "content-length:");
                if (cl_str) {
                    int content_len = atoi(cl_str + 15);
                    char *body_ptr = strstr(response->buf, "\r\n\r\n");
                    if (body_ptr) {
                        int body_got = total_read - (int)(body_ptr + 4 - response->buf);
                        if (body_got >= content_len) break;
                    }
                }
            }
        } else if (n == 0 || (!use_tls && n < 0) ||
                   (use_tls && n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)) {
            break;
        } else if (use_tls && n == MBEDTLS_ERR_SSL_WANT_READ) {
            rtos_time_delay_ms(100);
        } else if (use_tls && n == MBEDTLS_ERR_SSL_TIMEOUT) {
            break;
        } else {
            break;
        }
    }

    free(read_buf);

    if (total_read > 0 && header_end) {
        /* Extract HTTP status code */
        if (out_status) {
            *out_status = 0;
            if (response->len >= 12 && memcmp(response->buf, "HTTP/", 5) == 0) {
                const char *sp = (const char *)memchr(response->buf, ' ', 16);
                if (sp) *out_status = atoi(sp + 1);
            }
        }
        /* Detect chunked transfer encoding before stripping headers.
         * Bound the "chunked" search to the header line to avoid a false
         * positive if the response body happens to contain the word "chunked"
         * (e.g. Transfer-Encoding: identity  +  body with "chunked" text). */
        int is_chunked = 0;
        {
            char *te = strstr(response->buf, "Transfer-Encoding:");
            if (!te) te = strstr(response->buf, "transfer-encoding:");
            if (te) {
                /* Cap search to min(128, bytes remaining in buffer) to avoid an
                 * out-of-bounds write when the TE header sits within the last 127
                 * bytes of the allocation (which has only 1 NUL byte of slack). */
                size_t te_remaining = (size_t)(response->buf + response->len - te);
                size_t search_len = te_remaining < 128 ? te_remaining : 128;
                char *eol = (char *)memchr(te, '\n', search_len);
                if (eol) {
                    char saved = *eol;
                    *eol = '\0';
                    if (strstr(te, "chunked")) is_chunked = 1;
                    *eol = saved;
                }
                /* If no '\n' within search_len bytes, the header line is truncated
                 * or malformed — leave is_chunked = 0 (safe: decoder won't run). */
            }
        }
        /* Strip HTTP header, keep body only */
        char *body_start = strstr(response->buf, "\r\n\r\n");
        if (body_start) {
            /* Capture one optional response header before stripping */
            if (response->cap_hdr && response->cap_hdr[0]) {
                response->cap_hdr_val[0] = '\0';
                size_t nlen = strlen(response->cap_hdr);
                const char *scan = response->buf;
                const char *crlf = strstr(scan, "\r\n");
                if (crlf) {
                    scan = crlf + 2; /* skip status line */
                    while (scan < body_start) {
                        crlf = strstr(scan, "\r\n");
                        if (!crlf || crlf > body_start) break;
                        size_t line_len = (size_t)(crlf - scan);
                        if (line_len == 0) break;
                        if (line_len >= nlen + 1 &&
                            strncmp(scan, response->cap_hdr, nlen) == 0 &&
                            scan[nlen] == ':') {
                            const char *v = scan + nlen + 1;
                            while (*v == ' ') v++;
                            size_t vlen = 0;
                            while (v[vlen] && (v + vlen) < crlf &&
                                   vlen < sizeof(response->cap_hdr_val) - 1)
                                vlen++;
                            memcpy(response->cap_hdr_val, v, vlen);
                            response->cap_hdr_val[vlen] = '\0';
                            break;
                        }
                        scan = crlf + 2;
                    }
                }
            }
            body_start += 4;
            size_t blen = response->len - (size_t)(body_start - response->buf);
            _memmove(response->buf, body_start, blen + 1);
            response->len = blen;

            if (is_chunked) {
                /* Decode chunked transfer encoding in-place */
                char *src = response->buf;
                char *dst = response->buf;
                char *end = response->buf + response->len;
                while (src < end) {
                    char *nl = memchr(src, '\n', (size_t)(end - src));
                    if (!nl) break;
                    /* Use endptr to reject chunk-size lines with no valid hex
                     * digits (e.g. a stray newline or a malformed size token). */
                    char *endptr = src;
                    long chunk_sz = strtol(src, &endptr, 16);
                    if (endptr == src || chunk_sz < 0) break;
                    src = nl + 1;
                    if (chunk_sz == 0) break;  /* terminal chunk */
                    /* Guard using ptrdiff_t arithmetic to avoid pointer overflow UB
                     * when chunk_sz is LONG_MAX (strtol ERANGE on 32-bit long). */
                    if (chunk_sz > (long)(end - src)) {
                        /* Truncated final chunk: copy what we have and warn. */
                        RTK_LOGW(TAG, "chunked: truncated chunk\n");
                        _memmove(dst, src, (size_t)(end - src));
                        dst += end - src;
                        break;
                    }
                    _memmove(dst, src, (size_t)chunk_sz);
                    dst += chunk_sz;
                    src += chunk_sz;
                    if (src + 1 < end && src[0] == '\r' && src[1] == '\n') {
                        src += 2;
                    } else if (src < end && src[0] == '\n') {
                        src += 1;
                    }
                }
                *dst = '\0';
                response->len = (size_t)(dst - response->buf);
            }
        }
        ret = 0;
    }

    httpc_conn_close(conn);
    httpc_conn_free(conn);
    RTK_LOGI(TAG, "http_request: %s %s -> status=%d body=%u bytes ret=%d\n",
             method, host, out_status ? *out_status : 0, (unsigned)(response ? response->len : 0), ret);
    return ret;
}

/* ---- HTTPS multipart/form-data POST with VFS file streaming ---------------
 * Sends:  preamble (multipart headers for all form fields + file part header)
 *       + file content (512-byte chunks from VFS)
 *       + suffix (closing boundary: "--<boundary>--\r\n")
 * Content-Length = preamble_len + file_size + suffix_len, set up front.
 * Peak RAM: one 512-byte chunk, not the full file.
 * ----------------------------------------------------------------------- */

int llm_http_post_multipart_file(const char *host, const char *resource,
                                  const char *preamble, size_t preamble_len,
                                  const char *vfs_path, size_t file_size,
                                  const char *suffix, size_t suffix_len,
                                  llm_http_resp_t *resp)
{
    if (!host || !resource || !preamble || !vfs_path || !suffix || !resp) return -1;

    FILE *fp = fopen(vfs_path, "rb");
    if (!fp) {
        RTK_LOGE(TAG, "multipart: cannot open %s\n", vfs_path);
        return -7;
    }

    char clean_host[128];
    int  use_tls;
    uint16_t port = parse_host_port(host, clean_host, sizeof(clean_host), &use_tls);

    struct httpc_conn *conn = NULL;
    if (use_tls) {
        const int delays_ms[] = { 0, 1000, 3000 };
        int attempts = (int)(sizeof(delays_ms) / sizeof(delays_ms[0]));
        for (int i = 0; i < attempts; i++) {
            if (delays_ms[i]) rtos_time_delay_ms(delays_ms[i]);
            conn = httpc_conn_new(HTTPC_SECURE_TLS, NULL, NULL, NULL);
            if (!conn) { fclose(fp); return -1; }
            if (tls_connect_offloaded(conn, clean_host, port, 30) == 0) break;
            httpc_conn_free(conn); conn = NULL;
        }
        if (!conn) { fclose(fp); return -2; }
        install_custom_tls_timeout(conn, 60000);
    } else {
        conn = httpc_conn_new(HTTPC_SECURE_NONE, NULL, NULL, NULL);
        if (!conn) { fclose(fp); return -1; }
        if (httpc_conn_connect(conn, clean_host, port, 10) != 0) {
            httpc_conn_free(conn); fclose(fp); return -2;
        }
    }

    struct httpc_tls_internal *tls = use_tls ? (struct httpc_tls_internal *)conn->tls : NULL;

    size_t total_body = preamble_len + file_size + suffix_len;
    /* Build HTTP header — multipart boundary string is in preamble */
    char *hdr = (char *)malloc(512);
    if (!hdr) { fclose(fp); httpc_conn_close(conn); httpc_conn_free(conn); return -1; }
    int hdr_len = DiagSnPrintf(hdr, 512,
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: multipart/form-data; boundary=ameba_claw_boundary\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "\r\n",
        resource, host, (unsigned)total_body);
    if (hdr_len < 0 || hdr_len >= 512) {
        free(hdr); fclose(fp); httpc_conn_close(conn); httpc_conn_free(conn); return -3;
    }

    /* Write header */
    int wr;
    if (use_tls) wr = tls_write_all(&tls->ctx, hdr, (size_t)hdr_len);
    else         wr = plain_write_all(conn->sock, hdr, (size_t)hdr_len);
    free(hdr);
    if (wr < 0) { fclose(fp); httpc_conn_close(conn); httpc_conn_free(conn); return -3; }

    /* Write preamble (form fields + file part header) */
    if (use_tls) wr = tls_write_all(&tls->ctx, preamble, preamble_len);
    else         wr = plain_write_all(conn->sock, preamble, preamble_len);
    if (wr < 0) { fclose(fp); httpc_conn_close(conn); httpc_conn_free(conn); return -3; }

    /* Stream file in 512-byte chunks */
    uint8_t *chunk = (uint8_t *)malloc(512);
    if (!chunk) { fclose(fp); httpc_conn_close(conn); httpc_conn_free(conn); return -1; }
    size_t n;
    while ((n = fread(chunk, 1, 512, fp)) > 0) {
        if (use_tls) wr = tls_write_all(&tls->ctx, chunk, n);
        else         wr = plain_write_all(conn->sock, chunk, n);
        if (wr < 0) {
            free(chunk); fclose(fp);
            httpc_conn_close(conn); httpc_conn_free(conn);
            return -3;
        }
    }
    free(chunk);
    fclose(fp);

    /* Write closing boundary */
    if (use_tls) wr = tls_write_all(&tls->ctx, suffix, suffix_len);
    else         wr = plain_write_all(conn->sock, suffix, suffix_len);
    if (wr < 0) { httpc_conn_close(conn); httpc_conn_free(conn); return -3; }

    /* Read response into resp */
    uint8_t *rbuf = (uint8_t *)malloc(512);
    if (!rbuf) { httpc_conn_close(conn); httpc_conn_free(conn); return -1; }
    int total_read = 0, header_end = 0, hdr_steps = 0;
    while (1) {
        int rn;
        if (use_tls) rn = mbedtls_ssl_read(&tls->ctx, rbuf, 512);
        else         rn = lwip_recv(conn->sock, rbuf, 512, 0);
        if (rn <= 0) break;
        if (resp_append(resp, (const char *)rbuf, (size_t)rn) != 0) break;
        total_read += rn;
        if (!header_end) {
            for (size_t i = 0; i < resp->len; i++) {
                char c = resp->buf[i];
                if      (c == '\r' && hdr_steps == 0) hdr_steps = 1;
                else if (c == '\n' && hdr_steps == 1) hdr_steps = 2;
                else if (c == '\r' && hdr_steps == 2) hdr_steps = 3;
                else if (c == '\n' && hdr_steps == 3) { header_end = 1; break; }
                else hdr_steps = 0;
            }
        }
        if (header_end) {
            char *cl = strstr(resp->buf, "Content-Length:");
            if (!cl) cl = strstr(resp->buf, "content-length:");
            if (cl) {
                int clen = atoi(cl + 15);
                char *bp = strstr(resp->buf, "\r\n\r\n");
                if (bp && total_read - (int)(bp + 4 - resp->buf) >= clen) break;
            }
        }
    }
    free(rbuf);

    /* Strip HTTP header from resp, leave only body */
    if (header_end) {
        char *bp = strstr(resp->buf, "\r\n\r\n");
        if (bp) {
            bp += 4;
            size_t blen = resp->len - (size_t)(bp - resp->buf);
            memmove(resp->buf, bp, blen + 1);
            resp->len = blen;
        }
    }

    httpc_conn_close(conn);
    httpc_conn_free(conn);
    RTK_LOGI(TAG, "multipart: %s upload done, resp %u bytes\n",
             vfs_path, (unsigned)resp->len);
    return (total_read > 0 && header_end) ? 0 : -4;
}

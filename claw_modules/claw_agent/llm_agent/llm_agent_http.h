/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LLM_AGENT_HTTP_H
#define LLM_AGENT_HTTP_H

#include <stddef.h>
#include <stdint.h>

/* Sized to fit a single LLM response body. With max_tokens=16384:
 *   - English-heavy reply ≈ 64 KB raw + 1–2 KB JSON envelope
 *   - GLM in non-stream mode also carries reasoning_content in the body
 *   - Tool-call response with arguments ≈ +5 KB
 * 128 KB cap leaves headroom; PSRAM heap can absorb it (~4 MB free at boot).
 * Coupled to llm.max_tokens — if that default grows, grow this too or long
 * non-stream replies hit "response too large" (-1) and get truncated.
 * INIT 8 KB cuts the realloc chain for the common case (avg reply ~6–12 KB). */
#define LLM_HTTP_RESP_INIT_SIZE  (8 * 1024)
#define LLM_HTTP_RESP_MAX_SIZE   (128 * 1024)

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    uint32_t ttfb_ms;  /* time-to-first-byte in ms; 0 if not measured */
} llm_http_resp_t;

/**
 * Perform HTTPS POST request to LLM API.
 * @param host         Server hostname
 * @param resource     Resource path
 * @param body         Request body (JSON string)
 * @param body_len     Request body length
 * @param api_key      x-api-key header value
 * @param response     Output response struct (caller must free)
 * @return 0 on success, negative on error
 */
int llm_http_post(const char *host, const char *resource,
                  const char *body, size_t body_len,
                  const char *api_key,
                  llm_http_resp_t *response);

/**
 * Perform HTTPS POST with OpenAI-compatible Bearer auth.
 * Uses "Authorization: Bearer {api_key}" instead of "x-api-key".
 * Same interface as llm_http_post().
 */
int llm_http_post_bearer(const char *host, const char *resource,
                         const char *body, size_t body_len,
                         const char *api_key,
                         llm_http_resp_t *response);

/**
 * Perform HTTPS POST without any Authorization header.
 * Used for APIs where auth is embedded in the URL (e.g., Telegram Bot API).
 * Same interface as llm_http_post() but without api_key.
 */
int llm_http_post_no_auth(const char *host, const char *resource,
                           const char *body, size_t body_len,
                           llm_http_resp_t *response);

/**
 * Early-free variant: same as llm_http_post_bearer() but accepts char** body_pp.
 * The body buffer (*body_pp) is freed and set to NULL immediately after it has
 * been sent over TLS, reclaiming heap before the (large) SSE response is read.
 * After the call returns, *body_pp is NULL regardless of success/failure.
 */
int llm_http_post_bearer_ef(const char *host, const char *resource,
                             char **body_pp, size_t body_len,
                             const char *api_key,
                             llm_http_resp_t *response);

/** Initialize a response struct (allocates initial buffer). */
int llm_http_resp_init(llm_http_resp_t *resp);

/** Free response buffer. */
void llm_http_resp_free(llm_http_resp_t *resp);

/**
 * Persistent HTTPS session — reuses the TLS connection across multiple POSTs
 * to the same host, avoiding a full handshake on every poll cycle.
 */
typedef struct llm_http_session llm_http_session_t;

llm_http_session_t *llm_http_session_open(const char *host);
void                llm_http_session_close(llm_http_session_t *s);

/**
 * POST on an open session without Authorization header (e.g., Telegram Bot API).
 * Reconnects transparently on error.  Uses Connection: keep-alive.
 * Returns 0 on success, negative on unrecoverable failure.
 */
int llm_http_session_post_no_auth(llm_http_session_t *s,
                                  const char *resource,
                                  const char *body, size_t body_len,
                                  llm_http_resp_t *response);

/**
 * HTTPS GET — stream response body directly to a VFS file.
 * Used for downloading IM attachments without buffering the whole file in RAM.
 * @param host         Server hostname (port via "host:port", default 443)
 * @param resource     Request path (e.g. "/file/bot<token>/<path>")
 * @param dest_path    VFS destination path (e.g. "vfs:/inbox/telegram/...")
 * @param max_bytes    Safety cap — fail if Content-Length exceeds this (0 = no cap)
 * @param out_bytes    Written file size in bytes (may be NULL)
 * @return 0 on success, negative on error
 */
int llm_http_get_to_file(const char *host, const char *resource,
                          const char *dest_path,
                          size_t max_bytes, size_t *out_bytes);

/**
 * HTTPS multipart/form-data POST — stream a VFS file as one part.
 * Sends: preamble (headers + form fields) + file content (chunked 512 B at a
 * time) + suffix (closing boundary).  Peak RAM usage is O(512 B), not O(file).
 * @param host         Server hostname
 * @param resource     Request path
 * @param preamble     Raw bytes before the file data (multipart headers)
 * @param preamble_len Byte length of preamble
 * @param vfs_path     VFS source file path
 * @param file_size    Known file size in bytes (used for Content-Length header)
 * @param suffix       Raw bytes after file data (closing boundary)
 * @param suffix_len   Byte length of suffix
 * @param resp         Response accumulator (caller must init and free)
 * @return 0 on success, negative on error
 */
int llm_http_post_multipart_file(const char *host, const char *resource,
                                  const char *preamble, size_t preamble_len,
                                  const char *vfs_path, size_t file_size,
                                  const char *suffix, size_t suffix_len,
                                  llm_http_resp_t *resp);

#endif /* LLM_AGENT_HTTP_H */

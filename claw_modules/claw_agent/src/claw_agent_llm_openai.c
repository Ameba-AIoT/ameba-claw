/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * OpenAI-compatible chat backend.
 *
 * Owns everything specific to the OpenAI wire format: the SSE stream
 * reassembler (OpenAI delta chunks → a single non-streaming completion JSON),
 * tool-call argument sanitization/parsing, and the request/response exchange in
 * claw_agent_llm_chat_openai(). Anthropic lives in its own TU; both share the
 * frontend's config resolution and URL parsing via claw_agent_llm_internal.h.
 */

#include "ameba_soc.h"
#include "claw_agent_llm_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "llm_agent_http.h"
#include "claw_config.h"
#include "os_wrapper.h"
#include "ameba_claw_defs.h"

#define TAG "claw_agent_llm"

/* ---- SSE stream reassembly ---- */

/* Reassemble an OpenAI-compatible SSE stream (data: {...}\n\n events) into
 * a standard non-streaming chat-completion JSON so the existing parser works
 * unchanged.  Returns 0 on success, -1 on OOM. */

#define SSE_MAX_TC 8

static int sse_str_append(char **dst, size_t *dlen, size_t *dcap,
                          const char *src, size_t slen)
{
    if (*dlen + slen + 1 > *dcap) {
        size_t new_cap = *dcap + slen + 256;
        char  *nb = realloc(*dst, new_cap);
        if (!nb) return -1;
        *dst  = nb;
        *dcap = new_cap;
    }
    memcpy(*dst + *dlen, src, slen);
    *dlen += slen;
    (*dst)[*dlen] = '\0';
    return 0;
}

int claw_agent_llm_sse_reassemble(llm_http_resp_t *resp)
{
    if (resp->len < 6)
        return 0; /* not SSE */

    /* Handle "event: xxx\ndata: {...}" — backend error events or named SSE events.
     * Extract the data: line's JSON payload and replace the buffer with it so that
     * the downstream cJSON_Parse / error-field extraction works normally. */
    if (strncmp(resp->buf, "event: ", 7) == 0) {
        char *data_line = strstr(resp->buf, "\ndata: ");
        if (!data_line)
            return 0; /* malformed, let caller fail */
        data_line += 7; /* skip "\ndata: " */
        char *nl = memchr(data_line, '\n', (size_t)(resp->buf + resp->len - data_line));
        size_t jlen = nl ? (size_t)(nl - data_line) : (size_t)(resp->buf + resp->len - data_line);
        if (jlen == 0)
            return 0;
        char *new_buf = (char *)rtos_mem_malloc(jlen + 1);
        if (!new_buf) return -1;
        memcpy(new_buf, data_line, jlen);
        new_buf[jlen] = '\0';
        rtos_mem_free(resp->buf);
        resp->buf = new_buf;
        resp->len = jlen;
        resp->cap = jlen + 1;
        return 0; /* buffer is now plain JSON — let caller parse it */
    }

    if (strncmp(resp->buf, "data: ", 6) != 0)
        return 0; /* not SSE */

    /* Accumulated content / tool-call state */
    char  *content   = NULL; size_t content_len = 0, content_cap = 0;
    char  *finish    = NULL;
    char  *resp_id   = NULL;
    char  *resp_model = NULL;
    cJSON *usage      = NULL;   /* final chunk's usage (stream_options.include_usage) */

    struct {
        char  *id;
        char  *name;
        char  *args;
        size_t args_len;
        size_t args_cap;
    } tc[SSE_MAX_TC];
    int tc_count = 0;
    memset(tc, 0, sizeof(tc));

    char *p   = resp->buf;
    char *end = resp->buf + resp->len;

    while (p < end) {
        char  *nl   = memchr(p, '\n', (size_t)(end - p));
        size_t llen = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (llen > 0 && p[llen - 1] == '\r') llen--;

        if (llen >= 6 && strncmp(p, "data: ", 6) == 0) {
            char  *pay  = p + 6;
            size_t plen = llen - 6;

            if (plen == 6 && memcmp(pay, "[DONE]", 6) == 0)
                goto sse_build;

            /* Temporarily null-terminate for cJSON */
            char saved = pay[plen];
            pay[plen] = '\0';
            cJSON *chunk = cJSON_Parse(pay);
            pay[plen] = saved;

            if (!chunk) { p = nl ? nl + 1 : end; continue; }

            if (!resp_id) {
                cJSON *j = cJSON_GetObjectItem(chunk, "id");
                if (j && cJSON_IsString(j)) resp_id = strdup(j->valuestring);
            }
            if (!resp_model) {
                cJSON *j = cJSON_GetObjectItem(chunk, "model");
                if (j && cJSON_IsString(j)) resp_model = strdup(j->valuestring);
            }
            if (!usage) {
                cJSON *u = cJSON_GetObjectItem(chunk, "usage");
                if (u && cJSON_IsObject(u)) usage = cJSON_Duplicate(u, true);
            }

            cJSON *choices = cJSON_GetObjectItem(chunk, "choices");
            cJSON *ch0     = choices ? cJSON_GetArrayItem(choices, 0) : NULL;
            if (ch0) {
                cJSON *fr = cJSON_GetObjectItem(ch0, "finish_reason");
                if (fr && cJSON_IsString(fr) && fr->valuestring[0]) {
                    free(finish); finish = strdup(fr->valuestring);
                }
                cJSON *delta = cJSON_GetObjectItem(ch0, "delta");
                if (delta) {
                    cJSON *c = cJSON_GetObjectItem(delta, "content");
                    if (c && cJSON_IsString(c) && c->valuestring[0]) {
                        size_t clen = strlen(c->valuestring);
                        if (sse_str_append(&content, &content_len, &content_cap,
                                           c->valuestring, clen) != 0) {
                            cJSON_Delete(chunk); goto sse_fail;
                        }
                    }
                    cJSON *tcs = cJSON_GetObjectItem(delta, "tool_calls");
                    if (tcs && cJSON_IsArray(tcs)) {
                        cJSON *tci;
                        cJSON_ArrayForEach(tci, tcs) {
                            cJSON *idx_j = cJSON_GetObjectItem(tci, "index");
                            int idx = idx_j ? (int)cJSON_GetNumberValue(idx_j) : 0;
                            if (idx < 0 || idx >= SSE_MAX_TC) continue;
                            if (idx >= tc_count) tc_count = idx + 1;

                            cJSON *id_j = cJSON_GetObjectItem(tci, "id");
                            if (id_j && cJSON_IsString(id_j) && id_j->valuestring[0] && !tc[idx].id)
                                tc[idx].id = strdup(id_j->valuestring);

                            cJSON *func = cJSON_GetObjectItem(tci, "function");
                            if (func) {
                                cJSON *nm = cJSON_GetObjectItem(func, "name");
                                /* Accept name only when non-empty: GLM-Z1 sends an initial
                                 * announcement chunk with "name":"" before the real name,
                                 * which would lock the slot to an empty string. */
                                if (nm && cJSON_IsString(nm) && nm->valuestring[0] && !tc[idx].name)
                                    tc[idx].name = strdup(nm->valuestring);
                                cJSON *args = cJSON_GetObjectItem(func, "arguments");
                                if (args && cJSON_IsString(args) && args->valuestring[0]) {
                                    size_t alen = strlen(args->valuestring);
                                    if (sse_str_append(&tc[idx].args,
                                                       &tc[idx].args_len,
                                                       &tc[idx].args_cap,
                                                       args->valuestring, alen) != 0) {
                                        cJSON_Delete(chunk); goto sse_fail;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            cJSON_Delete(chunk);
        }
        p = nl ? nl + 1 : end;
    }

sse_build: {
    cJSON *out    = cJSON_CreateObject();
    cJSON *chlist = cJSON_CreateArray();
    cJSON *ch0out = cJSON_CreateObject();
    cJSON *msg    = cJSON_CreateObject();
    if (!out || !chlist || !ch0out || !msg) {
        cJSON_Delete(out); cJSON_Delete(chlist);
        cJSON_Delete(ch0out); cJSON_Delete(msg);
        goto sse_fail;
    }

    cJSON_AddStringToObject(out, "id",     resp_id    ? resp_id    : "chatcmpl-sse");
    cJSON_AddStringToObject(out, "model",  resp_model ? resp_model : "");
    cJSON_AddStringToObject(out, "object", "chat.completion");
    cJSON_AddStringToObject(msg, "role", "assistant");

    if (content && content[0])
        cJSON_AddStringToObject(msg, "content", content);
    else
        cJSON_AddNullToObject(msg, "content");

    if (tc_count > 0) {
        cJSON *tc_arr = cJSON_CreateArray();
        if (!tc_arr) {
            cJSON_Delete(out); cJSON_Delete(chlist);
            cJSON_Delete(ch0out); cJSON_Delete(msg);
            goto sse_fail;
        }
        int i;
        for (i = 0; i < tc_count; i++) {
            cJSON *t    = cJSON_CreateObject();
            cJSON *func = cJSON_CreateObject();
            if (!t || !func) {
                cJSON_Delete(t); cJSON_Delete(func); cJSON_Delete(tc_arr);
                cJSON_Delete(out); cJSON_Delete(chlist);
                cJSON_Delete(ch0out); cJSON_Delete(msg);
                goto sse_fail;
            }
            cJSON_AddStringToObject(t,    "id",   tc[i].id   ? tc[i].id   : "call_sse");
            cJSON_AddStringToObject(t,    "type", "function");
            cJSON_AddStringToObject(func, "name",      tc[i].name ? tc[i].name : "");
            cJSON_AddStringToObject(func, "arguments", tc[i].args ? tc[i].args : "{}");
            cJSON_AddItemToObject(t, "function", func);
            cJSON_AddItemToArray(tc_arr, t);
        }
        cJSON_AddItemToObject(msg, "tool_calls", tc_arr);
    }

    cJSON_AddItemToObject(ch0out, "message",       msg);
    cJSON_AddStringToObject(ch0out, "finish_reason", finish ? finish : "stop");
    cJSON_AddItemToArray(chlist, ch0out);
    cJSON_AddItemToObject(out, "choices", chlist);

    /* Fold the streamed usage back in so the non-stream parser path picks up
     * real prompt_tokens. Ownership moves to `out` (freed below). */
    if (usage) { cJSON_AddItemToObject(out, "usage", usage); usage = NULL; }

    char *assembled = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (!assembled) goto sse_fail;

    size_t alen = strlen(assembled);
    char  *new_buf = (char *)rtos_mem_malloc(alen + 1);
    if (!new_buf) { free(assembled); goto sse_fail; }
    memcpy(new_buf, assembled, alen + 1);
    free(assembled);

    rtos_mem_free(resp->buf);
    resp->buf = new_buf;
    resp->len = alen;
    resp->cap = alen + 1;
    }

    free(content); free(finish); free(resp_id); free(resp_model);
    if (usage) cJSON_Delete(usage);
    { int i; for (i = 0; i < SSE_MAX_TC; i++) { free(tc[i].id); free(tc[i].name); free(tc[i].args); } }
    return 0;

sse_fail:
    free(content); free(finish); free(resp_id); free(resp_model);
    if (usage) cJSON_Delete(usage);
    { int i; for (i = 0; i < SSE_MAX_TC; i++) { free(tc[i].id); free(tc[i].name); free(tc[i].args); } }
    return -1;
}

/* ---- Tool call parsing (OpenAI format) ---- */

/* Replace literal control chars (0x00-0x1F) inside a JSON value string with
 * their JSON-escape equivalents.  cJSON is lenient about raw control chars,
 * but Python's json module (used by the LiteLLM proxy) treats a literal LF
 * as ending the string, causing "Unterminated string" when the proxy tries to
 * re-parse "arguments" from conversation history. */
static char *json_str_sanitize(const char *src)
{
    size_t i, slen, extra;
    char  *out, *q;
    unsigned char c;

    if (!src) return strdup("{}");
    slen  = strlen(src);
    extra = 0;
    for (i = 0; i < slen; i++) {
        c = (unsigned char)src[i];
        if (c == '\n' || c == '\r' || c == '\t' || c == '\b' || c == '\f') extra++;
        else if (c < 0x20) extra += 5; /* \uXXXX: 6 output chars - 1 input = 5 extra */
    }
    if (extra == 0) return strdup(src);
    out = (char *)malloc(slen + extra + 1);
    if (!out) return strdup(src);
    q = out;
    for (i = 0; i < slen; i++) {
        c = (unsigned char)src[i];
        if      (c == '\n') { *q++ = '\\'; *q++ = 'n'; }
        else if (c == '\r') { *q++ = '\\'; *q++ = 'r'; }
        else if (c == '\t') { *q++ = '\\'; *q++ = 't'; }
        else if (c == '\b') { *q++ = '\\'; *q++ = 'b'; }
        else if (c == '\f') { *q++ = '\\'; *q++ = 'f'; }
        else if (c < 0x20)  { q += sprintf(q, "\\u%04X", (unsigned)c); }
        else                { *q++ = (char)c; }
    }
    *q = '\0';
    return out;
}

static int parse_tool_calls_openai(cJSON *tool_calls_json,
                                   llm_resp_t *out_response,
                                   char **out_error_message)
{
    int count = cJSON_GetArraySize(tool_calls_json);
    int i;

    if (count <= 0) return RTK_SUCCESS;
    if (count > CLAW_AGENT_LLM_MAX_TOOL_CALLS) count = CLAW_AGENT_LLM_MAX_TOOL_CALLS;

    out_response->calls = calloc((size_t)count, sizeof(llm_call_t));
    if (!out_response->calls) {
        if (out_error_message) *out_error_message = dup_printf("OOM allocating tool_calls");
        return RTK_ERR_NOMEM;
    }

    for (i = 0; i < count; i++) {
        cJSON *tc       = cJSON_GetArrayItem(tool_calls_json, i);
        cJSON *id_item  = cJSON_GetObjectItem(tc, "id");
        cJSON *func     = cJSON_GetObjectItem(tc, "function");
        cJSON *name_item = func ? cJSON_GetObjectItem(func, "name") : NULL;
        cJSON *args_item = func ? cJSON_GetObjectItem(func, "arguments") : NULL;

        out_response->calls[i].call_id =
            (id_item && cJSON_IsString(id_item)) ? strdup(id_item->valuestring) : strdup("");
        out_response->calls[i].fn_name =
            (name_item && cJSON_IsString(name_item)) ? strdup(name_item->valuestring) : strdup("");
        {
            const char *raw = (args_item && cJSON_IsString(args_item)) ? args_item->valuestring : "{}";
            size_t raw_len = strlen(raw);
            RTK_LOGD(TAG, "[DBG2] tc[%d] fn=%s args_item=%s raw_len=%u last='%c'(0x%02X)\n",
                     i, out_response->calls[i].fn_name ? out_response->calls[i].fn_name : "?",
                     args_item ? "OK" : "NULL", (unsigned)raw_len,
                     raw_len > 0 ? raw[raw_len-1] : '?',
                     raw_len > 0 ? (unsigned char)raw[raw_len-1] : 0);
            out_response->calls[i].args_json = json_str_sanitize(raw);
        }

        if (!out_response->calls[i].call_id ||
                !out_response->calls[i].fn_name ||
                !out_response->calls[i].args_json) {
            out_response->call_cnt = (size_t)i + 1;
            if (out_error_message) *out_error_message = dup_printf("OOM copying tool call fields");
            return RTK_ERR_NOMEM;
        }
    }
    out_response->call_cnt = (size_t)count;
    return RTK_SUCCESS;
}

/* ---- OpenAI-compatible backend ---- */

int claw_agent_llm_chat_openai(const char *system_prompt,
                               cJSON *messages,
                               const char *tools_json,
                               llm_resp_t *out_response,
                               char **out_error_message)
{
    const claw_config_t *cfg = claw_config_get();
    const char *api_key  = resolve_api_key(cfg);
    const char *model    = resolve_model(cfg, "glm-5.1");
    claw_llm_backend_t backend = resolve_backend(cfg);
    uint32_t max_tokens  = cfg->llm.max_tokens ? (uint32_t)cfg->llm.max_tokens
                                               : CLAW_AGENT_LLM_DEFAULT_TOKENS;
    char host[128], api_path[256];
    llm_parse_url(cfg->llm.api_url, backend, host, sizeof(host), api_path, sizeof(api_path));

    cJSON *req = NULL, *sys_msg = NULL, *messages_copy = NULL, *tools_arr = NULL;
    char *req_json = NULL;
    llm_http_resp_t http_resp;
    cJSON *resp_root = NULL;
    int err = RTK_FAIL;
    char parse_preview[257] = {0};

    req = cJSON_CreateObject();
    if (!req) { *out_error_message = dup_printf("OOM req"); return RTK_ERR_NOMEM; }

    cJSON_AddStringToObject(req, "model", model);
    cJSON_AddNumberToObject(req, "max_completion_tokens", (double)max_tokens);

    /* Streaming: when enabled, the server returns an SSE (text/event-stream)
     * response and the HTTP layer assembles the deltas on-the-fly. This keeps
     * the first byte arriving within seconds (TTFT) instead of after the whole
     * generation completes, so the recv timeout is never starved on large
     * outputs. The HTTP layer auto-falls-back to buffered parsing if the server
     * ignores the flag and returns plain JSON. Only OpenAI-compatible backends
     * take this path — the SSE accumulator parses OpenAI delta chunks, not
     * Anthropic's content_block_delta events (see chat_anthropic). */
    if (cfg->llm.stream_enabled) {
        cJSON_AddTrueToObject(req, "stream");
        /* Ask the server to emit a final usage chunk; without this, streamed
         * responses carry no token counts and token-budget compaction would
         * fall back to char estimation every turn. */
        cJSON *so = cJSON_CreateObject();
        if (so) {
            cJSON_AddTrueToObject(so, "include_usage");
            cJSON_AddItemToObject(req, "stream_options", so);
        }
    } else {
        cJSON_AddFalseToObject(req, "stream");
    }

    /* GLM-5.1/5/4.7 enable reasoning by default, which can burn the whole
     * max_tokens budget on reasoning_content before any answer/tool_call
     * (finish_reason="length"). When reasoning is disabled in config, tell the
     * server to skip it. When enabled, send nothing — the model's own default
     * applies, and non-GLM OpenAI endpoints stay unaffected. */
    if (!cfg->llm.thinking_enabled) {
        cJSON *think = cJSON_CreateObject();
        if (think) {
            cJSON_AddStringToObject(think, "type", "disabled");
            cJSON_AddItemToObject(req, "thinking", think);
        }
    }

    messages_copy = cJSON_CreateArray();
    sys_msg = cJSON_CreateObject();
    if (!messages_copy || !sys_msg) {
        *out_error_message = dup_printf("OOM messages");
        err = RTK_ERR_NOMEM;
        goto cleanup;
    }
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", system_prompt);
    cJSON_AddItemToArray(messages_copy, sys_msg);
    sys_msg = NULL;

    {
        cJSON *item;
        cJSON_ArrayForEach(item, messages) {
            cJSON *dup = cJSON_Duplicate(item, true);
            if (!dup) { *out_error_message = dup_printf("OOM dup msg"); err = RTK_ERR_NOMEM; goto cleanup; }
            cJSON_AddItemToArray(messages_copy, dup);
        }
    }
    cJSON_AddItemToObject(req, "messages", messages_copy);
    messages_copy = NULL;

    if (tools_json && tools_json[0]) {
        tools_arr = cJSON_Parse(tools_json);
        if (tools_arr && cJSON_IsArray(tools_arr) && cJSON_GetArraySize(tools_arr) > 0) {
            cJSON_AddItemToObject(req, "tools", tools_arr);
            tools_arr = NULL;
        } else {
            cJSON_Delete(tools_arr);
            tools_arr = NULL;
        }
    }

    req_json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req); req = NULL;
    if (!req_json) { *out_error_message = dup_printf("OOM serial"); err = RTK_ERR_NOMEM; goto cleanup; }

    RTK_LOGD(TAG, "POST %s (%u bytes) backend=%d\n", api_path, (unsigned)strlen(req_json), (int)backend);

    if (llm_http_resp_init(&http_resp) != 0) {
        *out_error_message = dup_printf("OOM http resp");
        err = RTK_ERR_NOMEM;
        goto cleanup;
    }

    {
        int http_ret;
        http_ret = llm_http_post_bearer_ef(host, api_path, &req_json, strlen(req_json), api_key, &http_resp);
        free(req_json); req_json = NULL; /* safe: already NULL if early-freed */
        if (http_ret != 0) {
            /* -4 = fast close (FIN/RST), retried internally; -5 = recv timeout. */
            if (http_ret == -4) {
                *out_error_message = dup_printf("Empty HTTP response (connection closed/reset)");
            } else if (http_ret == -5) {
                *out_error_message = dup_printf("HTTP response timeout (no data)");
            } else {
                *out_error_message = dup_printf("HTTP POST failed (%d)", http_ret);
            }
            llm_http_resp_free(&http_resp);
            err = RTK_FAIL;
            goto cleanup;
        }
    }

    if (sse_reassemble(&http_resp) != 0) {
        *out_error_message = dup_printf("OOM sse");
        llm_http_resp_free(&http_resp);
        err = RTK_ERR_NOMEM;
        goto cleanup;
    }

    /* Capture preview before freeing — used for logging on parse failure */
    {
        size_t plen = http_resp.len;
        size_t plen2 = plen < 256 ? plen : 256;
        _memcpy(parse_preview, http_resp.buf, plen2);
        parse_preview[plen2] = '\0';
        /* Compact log: head 100 + tail 80, strip \r to avoid terminal overwrites */
        {
            char head_buf[101];
            size_t hlen = plen < 100 ? plen : 100;
            _memcpy(head_buf, http_resp.buf, hlen);
            head_buf[hlen] = '\0';
            for (size_t _i = 0; _i < hlen; _i++) { if (head_buf[_i] == '\r') head_buf[_i] = ' '; }
            if (plen <= 100) {
                RTK_LOGD(TAG, "response %u bytes: %s\n", (unsigned)plen, head_buf);
            } else {
                char tail_buf[81];
                _memcpy(tail_buf, http_resp.buf + (plen - 80), 80);
                tail_buf[80] = '\0';
                for (size_t _i = 0; _i < 80; _i++) { if (tail_buf[_i] == '\r') tail_buf[_i] = ' '; }
                RTK_LOGD(TAG, "response %u bytes: %s...%s\n", (unsigned)plen, head_buf, tail_buf);
            }
        }
    }
    out_response->ttfb_ms = http_resp.ttfb_ms;
    resp_root = cJSON_Parse(http_resp.buf);
    llm_http_resp_free(&http_resp);   /* buf freed here */

    if (!resp_root) {
        RTK_LOGE(TAG, "JSON parse failed, raw (first 256): %s\n", parse_preview);
        *out_error_message = dup_printf("JSON parse failed");
        err = RTK_FAIL;
        goto cleanup;
    }

    {
        cJSON *error = cJSON_GetObjectItem(resp_root, "error");
        if (error) {
            cJSON *msg = cJSON_GetObjectItem(error, "message");
            char *dump = cJSON_PrintUnformatted(resp_root);
            RTK_LOGE(TAG, "API error body: %.256s\n", dump ? dump : "(null)");
            free(dump);
            *out_error_message = dup_printf("LLM API error: %s",
                                            (msg && cJSON_IsString(msg)) ? msg->valuestring : "unknown");
            err = RTK_FAIL; goto cleanup;
        }
        cJSON *code = cJSON_GetObjectItem(resp_root, "code");
        cJSON *msg  = cJSON_GetObjectItem(resp_root, "msg");
        if (!msg) msg = cJSON_GetObjectItem(resp_root, "message");
        if (code && !cJSON_GetObjectItem(resp_root, "choices")) {
            *out_error_message = dup_printf("LLM API error (code=%d): %s",
                                            (int)cJSON_GetNumberValue(code),
                                            (msg && cJSON_IsString(msg)) ? msg->valuestring : "unknown");
            err = RTK_FAIL; goto cleanup;
        }
    }

    {
        cJSON *choices = cJSON_GetObjectItem(resp_root, "choices");
        if (!choices || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
            char *dump = cJSON_PrintUnformatted(resp_root);
            RTK_LOGE(TAG, "No choices: %.256s\n", dump ? dump : "(null)");
            free(dump);
            *out_error_message = dup_printf("No choices in LLM response");
            err = RTK_FAIL; goto cleanup;
        }
        cJSON *choice0  = cJSON_GetArrayItem(choices, 0);
        cJSON *message  = choice0 ? cJSON_GetObjectItem(choice0, "message") : NULL;
        if (!message) { *out_error_message = dup_printf("No message in choice"); err = RTK_FAIL; goto cleanup; }

        cJSON *content = cJSON_GetObjectItem(message, "content");
        if (content && cJSON_IsString(content) && content->valuestring[0]) {
            out_response->reply = strdup(content->valuestring);
            if (!out_response->reply) { *out_error_message = dup_printf("OOM text"); err = RTK_ERR_NOMEM; goto cleanup; }
        }
        {
            cJSON *rc = cJSON_GetObjectItem(message, "reasoning_content");
            if (rc && cJSON_IsString(rc) && rc->valuestring[0]) {
                out_response->thinking = strdup(rc->valuestring);
                if (!out_response->thinking) {
                    *out_error_message = dup_printf("OOM reasoning_content");
                    err = RTK_ERR_NOMEM;
                    goto cleanup;
                }
            }
        }
        cJSON *tool_calls_json = cJSON_GetObjectItem(message, "tool_calls");
        if (tool_calls_json && cJSON_IsArray(tool_calls_json) && cJSON_GetArraySize(tool_calls_json) > 0) {
            err = parse_tool_calls_openai(tool_calls_json, out_response, out_error_message);
            if (err != RTK_SUCCESS) goto cleanup;
        }
    }

    /* Real token usage (drives token-budget compaction). OpenAI/GLM shape:
     * usage:{prompt_tokens,completion_tokens,prompt_tokens_details:{cached_tokens}}.
     * For streaming this is folded back in by sse_reassemble (include_usage). */
    {
        cJSON *usage = cJSON_GetObjectItem(resp_root, "usage");
        if (usage) {
            cJSON *pt  = cJSON_GetObjectItem(usage, "prompt_tokens");
            cJSON *ct  = cJSON_GetObjectItem(usage, "completion_tokens");
            cJSON *ptd = cJSON_GetObjectItem(usage, "prompt_tokens_details");
            if (pt && cJSON_IsNumber(pt)) out_response->prompt_tokens     = (uint32_t)pt->valuedouble;
            if (ct && cJSON_IsNumber(ct)) out_response->completion_tokens = (uint32_t)ct->valuedouble;
            if (ptd) {
                cJSON *cc = cJSON_GetObjectItem(ptd, "cached_tokens");
                if (cc && cJSON_IsNumber(cc)) out_response->cached_tokens = (uint32_t)cc->valuedouble;
            }
        }
    }

    RTK_LOGD(TAG, "done text_len=%u tool_calls=%u prompt_tokens=%u cached=%u\n",
             (unsigned)(out_response->reply ? strlen(out_response->reply) : 0),
             (unsigned)out_response->call_cnt,
             (unsigned)out_response->prompt_tokens,
             (unsigned)out_response->cached_tokens);
    err = RTK_SUCCESS;

cleanup:
    cJSON_Delete(resp_root); cJSON_Delete(req); cJSON_Delete(sys_msg);
    cJSON_Delete(messages_copy); cJSON_Delete(tools_arr); free(req_json);
    if (err != RTK_SUCCESS) claw_agent_llm_response_free(out_response);
    return err;
}

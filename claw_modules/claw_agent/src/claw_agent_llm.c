/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * LLM wire-layer frontend.
 *
 * Owns the parts that are backend-agnostic: one-time init, compile-time default
 * resolution (claw_config field → registered default → built-in), request URL
 * parsing, and the public claw_agent_llm_chat_messages() dispatch + retry loop
 * that routes to the OpenAI or Anthropic backend. The per-backend wire format
 * lives in claw_agent_llm_openai.c / claw_agent_llm_anthropic.c.
 */
#include "ameba_soc.h"
#include "claw_agent_llm.h"
#include "claw_agent_llm_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "llm_agent_http.h"
#include "claw_config.h"
#include "os_wrapper.h"
#include "ameba_claw_defs.h"

#define TAG "claw_agent_llm"

#define CLAW_AGENT_LLM_DEFAULT_HOST         "open.bigmodel.cn"
#define CLAW_AGENT_LLM_DEFAULT_PATH         "/api/paas/v4/chat/completions"
#define CLAW_AGENT_LLM_ANTHROPIC_DEFAULT_PATH "/v1/messages"

static bool s_initialized;

/* Compile-time fallbacks populated by claw_agent_llm_set_defaults().
 * Used when claw_config_get() returns empty fields — i.e. nobody has yet
 * written vfs:claw_config.json. Keeps the "flash a fresh binary and it
 * works" UX intact while letting the runtime config override at any
 * time without rebooting. The strings are owned by this module (copied
 * from main.c's static string literals via strlcpy). */
static char               s_default_api_key[256];
static char               s_default_model[64];
static char               s_default_host[128];
static char               s_default_path[256];
static claw_llm_backend_t s_default_backend = CLAW_LLM_BACKEND_OPENAI_BEARER;
static bool               s_defaults_set;

void claw_agent_llm_set_defaults(const char *api_key,
                                  const char *model,
                                  const char *base_url,
                                  const char *api_path,
                                  claw_llm_backend_t backend)
{
    if (api_key && api_key[0])  strlcpy(s_default_api_key, api_key,  sizeof(s_default_api_key));
    if (model   && model[0])    strlcpy(s_default_model,   model,    sizeof(s_default_model));
    if (base_url && base_url[0]) {
        const char *u = base_url;
        if (strncmp(u, "https://", 8) == 0) u += 8;
        else if (strncmp(u, "http://", 7) == 0) u += 7;
        strlcpy(s_default_host, u, sizeof(s_default_host));
    }
    if (api_path && api_path[0]) strlcpy(s_default_path, api_path, sizeof(s_default_path));
    s_default_backend = backend;
    s_defaults_set = true;
}

/* Pick api_key: claw_config wins if non-empty, else compile-time default,
 * else NULL. NULL eventually causes a 401 with an empty Authorization
 * header — that is the right "tell me to set a key" failure mode. */
const char *claw_agent_llm_resolve_api_key(const claw_config_t *cfg)
{
    if (cfg->llm.api_key[0]) return cfg->llm.api_key;
    if (s_default_api_key[0]) return s_default_api_key;
    return "";
}

const char *claw_agent_llm_resolve_model(const claw_config_t *cfg, const char *builtin)
{
    if (cfg->llm.model[0]) return cfg->llm.model;
    if (s_default_model[0]) return s_default_model;
    return builtin;
}

claw_llm_backend_t claw_agent_llm_resolve_backend(const claw_config_t *cfg)
{
    /* The backend field is a uint8 — there's no "unset" value distinct
     * from 0 (OPENAI_BEARER). Treat the presence of an api_url override
     * as "user took control of routing"; without it, prefer the
     * compile-time default which knows what the bundled key expects. */
    if (cfg->llm.api_url[0] || !s_defaults_set) {
        return (claw_llm_backend_t)cfg->llm.backend;
    }
    return s_default_backend;
}

void claw_agent_llm_parse_url(const char *api_url, claw_llm_backend_t backend,
                              char *host, size_t host_sz, char *path, size_t path_sz)
{
    /* Three-tier resolution for the request URL:
     *   1. claw_config api_url (highest priority — user explicitly set it)
     *   2. compile-time defaults registered via claw_agent_llm_set_defaults
     *      (the values main.c bakes into s_core_cfg)
     *   3. hardcoded built-in (open.bigmodel.cn / anthropic /v1/messages)
     * Built-in is always the seed so the buffers are populated; defaults
     * then overlay; api_url overlays last. */
    strlcpy(host, s_default_host[0] ? s_default_host : CLAW_AGENT_LLM_DEFAULT_HOST,
            host_sz);
    if (s_default_path[0]) {
        strlcpy(path, s_default_path, path_sz);
    } else {
        strlcpy(path,
                backend == CLAW_LLM_BACKEND_ANTHROPIC
                    ? CLAW_AGENT_LLM_ANTHROPIC_DEFAULT_PATH
                    : CLAW_AGENT_LLM_DEFAULT_PATH,
                path_sz);
    }
    if (!api_url || !api_url[0]) return;

    const char *u = api_url;
    if (strncmp(u, "https://", 8) == 0) u += 8;
    else if (strncmp(u, "http://", 7) == 0) u += 7;

    const char *slash = strchr(u, '/');
    if (slash && slash > u) {
        size_t hlen = (size_t)(slash - u);
        if (hlen < host_sz) { _memcpy(host, u, hlen); host[hlen] = '\0'; }
        /* Strip trailing slashes so a base URL like "/v1/" doesn't produce "//chat/completions" */
        char path_base[256];
        strlcpy(path_base, slash, sizeof(path_base));
        size_t plen = strlen(path_base);
        while (plen > 1 && path_base[plen - 1] == '/') path_base[--plen] = '\0';

        if (backend != CLAW_LLM_BACKEND_ANTHROPIC
                && !strstr(path_base, "chat/completions")
                && !strstr(path_base, "messages")) {
            DiagSnPrintf(path, path_sz, "%s/chat/completions", path_base);
        } else if (backend == CLAW_LLM_BACKEND_ANTHROPIC
                && !strstr(path_base, "messages")) {
            DiagSnPrintf(path, path_sz, "%s/messages", path_base);
        } else {
            strlcpy(path, path_base, path_sz);
        }
    } else if (!slash && u[0]) {
        strlcpy(host, u, host_sz);
    }
}

char *claw_agent_llm_dup_printf(const char *fmt, ...)
{
    va_list args, copy;
    int needed;
    char *buf;

    va_start(args, fmt);
    va_copy(copy, args);
    needed = DiagVSNprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return NULL;
    }
    buf = calloc(1, (size_t)needed + 1);
    if (!buf) {
        va_end(args);
        return NULL;
    }
    DiagVSNprintf(buf, (size_t)needed + 1, fmt, args);
    va_end(args);
    return buf;
}

int claw_agent_llm_init(char **out_error_message)
{
    if (out_error_message) *out_error_message = NULL;
    s_initialized = true;
    RTK_LOGD(TAG, "init\n");
    return RTK_SUCCESS;
}

/* ---- Public API ---- */

int claw_agent_llm_chat_messages(const char *system_prompt,
                                cJSON *messages,
                                const char *tools_json,
                                llm_resp_t *out_response,
                                char **out_error_message)
{
    if (out_error_message) *out_error_message = NULL;
    if (!s_initialized) {
        if (out_error_message) *out_error_message = dup_printf("LLM not initialized");
        return RTK_FAIL;
    }
    if (!system_prompt || !messages || !out_response || !out_error_message ||
            !cJSON_IsArray(messages)) {
        return RTK_ERR_BADARG;
    }

    bool is_anthropic =
        (resolve_backend(claw_config_get()) == CLAW_LLM_BACKEND_ANTHROPIC);

    /* Application-level retry loop. The transport layer already recovers from
     * connect / TLS / empty-response glitches; this loop additionally re-tries
     * the whole exchange when the server returns an error BODY — most importantly
     * rate-limit / overload ("该模型当前访问量过大，请您稍后再试", HTTP 429 or a
     * 200 carrying an `error` field). A short wait (CLAW_AGENT_LLM_RETRY_DELAY_MS)
     * sits between attempts so we don't hammer an overloaded endpoint.
     *
     * RTK_ERR_BADARG / RTK_ERR_NOMEM are local, deterministic failures — no point
     * retrying those, so we break out immediately. Everything else (API error
     * body, timeout, transport failure that exhausted the low-level retries) is
     * treated as transient and re-attempted up to the cap. */
    int err = RTK_FAIL;
    for (int attempt = 1; attempt <= CLAW_AGENT_LLM_RETRY_MAX_ATTEMPTS; attempt++) {
        _memset(out_response, 0, sizeof(*out_response));
        if (*out_error_message) { free(*out_error_message); *out_error_message = NULL; }

        err = is_anthropic
            ? claw_agent_llm_chat_anthropic(system_prompt, messages, tools_json, out_response, out_error_message)
            : claw_agent_llm_chat_openai(system_prompt, messages, tools_json, out_response, out_error_message);

        if (err == RTK_SUCCESS) return RTK_SUCCESS;
        if (err == RTK_ERR_BADARG || err == RTK_ERR_NOMEM) break;
        if (attempt >= CLAW_AGENT_LLM_RETRY_MAX_ATTEMPTS) break;

        RTK_LOGW(TAG, "LLM call failed (attempt %d/%d): %s — retry in %u ms\n",
                 attempt, CLAW_AGENT_LLM_RETRY_MAX_ATTEMPTS,
                 *out_error_message ? *out_error_message : "(no msg)",
                 (unsigned)CLAW_AGENT_LLM_RETRY_DELAY_MS);
        rtos_time_delay_ms(CLAW_AGENT_LLM_RETRY_DELAY_MS);
    }
    return err;
}

void claw_agent_llm_response_free(llm_resp_t *response)
{
    size_t i;
    if (!response) return;
    free(response->reply);
    response->reply = NULL;
    free(response->thinking);
    response->thinking = NULL;
    if (response->calls) {
        for (i = 0; i < response->call_cnt; i++) {
            free(response->calls[i].call_id);
            free(response->calls[i].fn_name);
            free(response->calls[i].args_json);
        }
        free(response->calls);
        response->calls = NULL;
    }
    response->call_cnt = 0;
}

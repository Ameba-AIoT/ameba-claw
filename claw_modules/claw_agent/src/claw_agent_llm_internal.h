/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/*
 * Private surface shared across the LLM wire-format translation units:
 *   claw_agent_llm.c            — frontend: init, config resolution, URL parsing,
 *                                 the chat_messages dispatch + retry loop
 *   claw_agent_llm_openai.c     — OpenAI-compatible backend (+ SSE reassembly)
 *   claw_agent_llm_anthropic.c  — Anthropic backend (+ prompt-cache anchors)
 *
 * The backends are deliberately self-contained per wire format so a new backend
 * — or a per-backend feature such as mid-request abort — can be added in
 * isolation. They reach back into the frontend only for config resolution and
 * a couple of small shared helpers, declared below.
 */

#include "claw_agent_llm.h"
#include "claw_config.h"
#include "cJSON.h"
#include "llm_agent_http.h"

/* ---- Shared wire-format constants ---- */
#define CLAW_AGENT_LLM_DEFAULT_TOKENS       16384
#define CLAW_AGENT_LLM_MAX_TOOL_CALLS       16

/* ---- Frontend-owned helpers (claw_agent_llm.c) ----
 * Aliased to the short names the backends were written against so the wire
 * code reads exactly as before; the aliases are scoped to these TUs only. */

char *claw_agent_llm_dup_printf(const char *fmt, ...);

/* Resolve config fields with the compile-time defaults registered via
 * claw_agent_llm_set_defaults() as fallback. */
const char        *claw_agent_llm_resolve_api_key(const claw_config_t *cfg);
const char        *claw_agent_llm_resolve_model(const claw_config_t *cfg, const char *builtin);
claw_llm_backend_t claw_agent_llm_resolve_backend(const claw_config_t *cfg);

/* Three-tier (api_url → compile-time default → built-in) request URL builder. */
void claw_agent_llm_parse_url(const char *api_url, claw_llm_backend_t backend,
                              char *host, size_t host_sz,
                              char *path, size_t path_sz);

#define dup_printf       claw_agent_llm_dup_printf
#define resolve_api_key  claw_agent_llm_resolve_api_key
#define resolve_model    claw_agent_llm_resolve_model
#define resolve_backend  claw_agent_llm_resolve_backend
#define llm_parse_url    claw_agent_llm_parse_url

/* ---- OpenAI SSE reassembler (owner: claw_agent_llm_openai.c) ----
 * Shared because the Anthropic path also runs it to unwrap "event:/data:" SSE
 * error events into plain JSON; on a normal (non-SSE) body it is a no-op. */
int claw_agent_llm_sse_reassemble(llm_http_resp_t *resp);
#define sse_reassemble   claw_agent_llm_sse_reassemble

/* ---- Per-backend entry points ---- */
int claw_agent_llm_chat_openai(const char *system_prompt,
                               cJSON *messages,
                               const char *tools_json,
                               llm_resp_t *out_response,
                               char **out_error_message);

int claw_agent_llm_chat_anthropic(const char *system_prompt,
                                  cJSON *messages,
                                  const char *tools_json,
                                  llm_resp_t *out_response,
                                  char **out_error_message);

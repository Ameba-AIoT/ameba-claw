/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "claw_compat.h"
#include "claw_agent.h"
#include "cJSON.h"

typedef struct {
    char *call_id;
    char *fn_name;
    char *args_json;
} llm_call_t;

typedef struct {
    char *reply;
    char *thinking;
    llm_call_t *calls;
    size_t call_cnt;
    /* Token usage parsed from the LLM response (0 when the endpoint does not
     * report it — caller falls back to a char-based estimate). prompt_tokens
     * is the real size of the context we just sent, used to drive token-budget
     * compaction. */
    uint32_t prompt_tokens;
    uint32_t completion_tokens;
    uint32_t cached_tokens;
    uint32_t ttfb_ms;  /* time-to-first-byte of the HTTP call, ms; 0 if not available */
} llm_resp_t;

int claw_agent_llm_init(char **out_error_message);

/* Register compile-time fallbacks. Whenever vfs:claw_config.json leaves a
 * field empty (api_key, model, host/path), the LLM HTTP layer falls back
 * to the value supplied here. NULL or empty arguments are ignored — pass
 * exactly the fields you want to override. base_url should be a bare
 * hostname (no scheme); api_path should start with '/'. */
void claw_agent_llm_set_defaults(const char *api_key,
                                  const char *model,
                                  const char *base_url,
                                  const char *api_path,
                                  claw_llm_backend_t backend);

int claw_agent_llm_chat_messages(const char *system_prompt,
                                      cJSON *messages,
                                      const char *tools_json,
                                      llm_resp_t *out_response,
                                      char **out_error_message);
void claw_agent_llm_response_free(llm_resp_t *response);

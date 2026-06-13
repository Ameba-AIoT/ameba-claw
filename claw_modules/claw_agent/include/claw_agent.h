/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "claw_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLAW_AGENT_RESPONSE_STATUS_OK = 0,
    CLAW_AGENT_RESPONSE_STATUS_ERROR = 1,
} claw_agent_response_status_t;

typedef enum {
    CLAW_AGENT_COMPLETION_DONE = 0,
} claw_agent_completion_type_t;

/* claw_agent_request_t.flags bits */
/* Caller will use claw_agent_receive_for() to collect the response.
 * engine pushes to response queue instead of invoking on_response callback. */
#define CLAW_AGENT_REQUEST_FLAG_SYNC_RECEIVE  (1u << 1)

typedef struct {
    uint32_t request_id;
    uint32_t flags;
    const char *session_id;
    const char *user_text;
    const char *source_channel;   /* informational: where the request came from */
    const char *source_chat_id;
    const char *source_sender_id;
    const char *source_message_id;
    const char *source_cap;
} claw_agent_request_t;

/* Persist one completed turn. `tool_msgs_json`, when non-NULL, is the verbatim
 * serialization of this turn's tool round-trips (the assistant tool_call /
 * tool_use messages and their results, in the wire format identified by
 * `backend`). The session layer stores it so the next request can replay the
 * tool history byte-identically — restoring cross-turn tool visibility and
 * keeping the LLM prompt-cache prefix continuous. NULL when no tools ran. */
typedef int (*claw_agent_append_session_turn_fn)(const char *session_id,
                                                      const char *user_text,
                                                      const char *assistant_text,
                                                      const char *tool_msgs_json,
                                                      int backend,
                                                      uint32_t prompt_tokens,
                                                      void *user_ctx);

typedef int (*claw_agent_request_start_fn)(const claw_agent_request_t *request,
                                                void *user_ctx);

typedef int (*claw_agent_stage_note_fn)(const claw_agent_request_t *request,
                                             char **out_note,
                                             void *user_ctx);

typedef struct claw_agent_response claw_agent_response_t;

typedef enum {
    CLAW_AGENT_CONTEXT_KIND_SYSTEM_PROMPT = 0,
    CLAW_AGENT_CONTEXT_KIND_MESSAGES = 1,
    CLAW_AGENT_CONTEXT_KIND_TOOLS = 2,
} claw_agent_context_kind_t;

typedef struct {
    claw_agent_context_kind_t kind;
    /* IMPORTANT: providers MUST allocate `content` with libc `malloc()` (or
     * compatible allocators that share the libc heap, e.g. `strdup`,
     * `cJSON_PrintUnformatted`). The agent frees this buffer with libc
     * `free()`. Using `rtos_mem_malloc` here silently corrupts heap
     * metadata — the bug is non-deterministic and surfaces hours/days
     * later in unrelated allocations. */
    char *content;
} claw_agent_context_t;

typedef int (*claw_agent_context_provider_collect_fn)(
    const claw_agent_request_t *request,
    claw_agent_context_t *out_context,
    void *user_ctx);

typedef struct {
    const char *name;
    claw_agent_context_provider_collect_fn collect;
    void *user_ctx;
} claw_agent_context_provider_t;

typedef int (*claw_agent_call_cap_fn)(const char *cap_name,
                                           const char *input_json,
                                           const claw_agent_request_t *request,
                                           char **out_output,
                                           void *user_ctx);

/* Callback invoked when a request completes without SYNC_RECEIVE flag set.
 * The caller receives the full response and is responsible for routing it
 * (e.g. dispatching to an IM channel). */
typedef void (*claw_agent_response_fn)(const claw_agent_response_t *response,
                                      void *user_ctx);

/* Callback fired just before each tool call executes.
 * Callers may use this to send per-step progress messages to the originating
 * IM channel.  source_channel / source_chat_id may be NULL for non-IM requests. */
typedef void (*claw_agent_tool_progress_fn)(uint32_t    request_id,
                                            const char *tool_name,
                                            const char *tool_args,
                                            const char *source_channel,
                                            const char *source_chat_id,
                                            void       *user_ctx);

typedef enum {
    CLAW_LLM_BACKEND_OPENAI_BEARER = 0, /* OpenAI-compatible, Authorization: Bearer <key> */
    CLAW_LLM_BACKEND_ANTHROPIC     = 1, /* Anthropic Claude API (different request/response format) */
} claw_llm_backend_t;

typedef struct {
    const char *api_key;
    const char *model;
    const char *base_url;   /* optional: override API host, e.g. "myhost.com" */
    const char *api_path;   /* optional: override API path, e.g. "/v1/chat/completions" */
    claw_llm_backend_t backend; /* default 0 = OPENAI_BEARER */
    uint32_t timeout_ms;
    uint32_t max_tokens;
    bool supports_tools;
    const char *system_prompt;
    claw_agent_append_session_turn_fn append_session_turn;
    void *append_session_turn_user_ctx;
    claw_agent_request_start_fn on_request_start;
    void *on_request_start_user_ctx;
    claw_agent_call_cap_fn call_cap;
    void *cap_user_ctx;
    claw_agent_response_fn on_response;
    void *on_response_user_ctx;
    claw_agent_tool_progress_fn on_tool_progress;
    void *on_tool_progress_user_ctx;
    uint32_t task_stack_size;
    uint16_t task_priority;
    uint32_t max_tool_iterations;
    uint32_t request_queue_len;
    uint32_t response_queue_len;
    size_t max_context_providers;
} claw_agent_config_t;

struct claw_agent_response {
    uint32_t request_id;
    claw_agent_response_status_t status;
    claw_agent_completion_type_t completion_type;
    /* Routing metadata copied from the request — callers use these to
     * dispatch the reply to the correct channel/chat. */
    char *source_channel;
    char *source_chat_id;
    char *text;
    char *error_message;
    char *tool_trace;   /* human-readable log of tool calls, NULL if none */
};

typedef struct {
    uint32_t request_id;
    const char *session_id;
    const char *user_text;
    const char *final_text;
    const char *context_providers_csv;
    const char *tool_calls_csv;
} claw_agent_completion_summary_t;

typedef void (*claw_agent_completion_observer_fn)(const claw_agent_completion_summary_t *summary,
                                                 void *user_ctx);

int claw_agent_init(const claw_agent_config_t *config);
int claw_agent_start(void);
int claw_agent_add_context_provider(const claw_agent_context_provider_t *provider);
int claw_agent_add_completion_observer(claw_agent_completion_observer_fn observer,
                                            void *user_ctx);
int claw_agent_call_cap(const char *cap_name,
                             const char *input_json,
                             const claw_agent_request_t *request,
                             char **out_output);
int claw_agent_submit(const claw_agent_request_t *request, uint32_t timeout_ms);
int claw_agent_cancel_request(uint32_t request_id);
/* Cancel the in-flight request if it belongs to the given session.
 * Returns RTK_SUCCESS if a cancellation was armed, RTK_FAIL if no match. */
int claw_agent_cancel_for_session(const char *session_id);
/* Wait for any completed response (FIFO from response queue).
 * Caller must call claw_agent_response_free() on RTK_SUCCESS. */
int claw_agent_receive(claw_agent_response_t *out_resp, uint32_t timeout_ms);
/* Wait for the response to a specific request_id.
 * Use after claw_agent_submit() with CLAW_AGENT_REQUEST_FLAG_SYNC_RECEIVE set.
 * Responses for other requests are buffered in a pending list. */
int claw_agent_receive_for(uint32_t request_id,
                           claw_agent_response_t *out_resp,
                           uint32_t timeout_ms);
void claw_agent_response_free(claw_agent_response_t *response);

#ifdef __cplusplus
}
#endif

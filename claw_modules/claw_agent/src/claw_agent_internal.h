/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/*
 * Private surface shared across the claw_agent core translation units:
 *   claw_agent.c          — engine shell / lifecycle / public API / tasks
 *   claw_agent_context.c  — request materialization (prompt/messages/tools + tool rounds)
 *   claw_agent_loop.c     — the agentic tool-call loop (process_request)
 *
 * Nothing here is part of the public API (claw_agent.h); it only exists so the
 * three TUs above can share the engine struct and a handful of helpers without
 * leaking internals to the rest of the firmware.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "claw_agent.h"
#include "claw_agent_llm.h"
#include "cJSON.h"
#include "os_wrapper.h"

/* ---- Build-time tunables shared between TUs ----
 * (Engine-only constants such as stack/priority/queue depth and the watchdog
 *  thresholds live in claw_agent.c; these are the ones referenced from more
 *  than one TU.) */
#define TOOL_LOG_BUFSIZE        768     /* tool-call trace buffer (loop owns the storage, context appends) */
#define TAGBUF_SIZE             384     /* comma-separated provider/tool tag buffers */
#define MAX_OBSERVERS           4       /* completion observers */

/* ---- Internal queue/node types ---- */

typedef struct {
    claw_agent_request_t pub;
    char *sid;
    char *utext;
    char *src_ch;
    char *src_cid;
    char *src_uid;
    char *src_mid;
    char *src_cap;
} rtk_req_node_t;

typedef struct {
    claw_agent_response_t pub;
} rtk_resp_node_t;

/* ---- Engine state (single instance, owned by claw_agent.c) ---- */

typedef struct {
    bool ready;
    bool running;
    char *sys_prompt;
    claw_agent_append_session_turn_fn save_turn;
    void *save_turn_ctx;
    claw_agent_request_start_fn       on_start;
    void *on_start_ctx;
    claw_agent_call_cap_fn            dispatch_cap;
    void *dispatch_cap_ctx;
    claw_agent_response_fn            on_response;
    void *on_response_ctx;
    claw_agent_tool_progress_fn       on_tool_progress;
    void *on_tool_progress_ctx;
    claw_agent_context_provider_t    *providers;
    size_t provider_cnt;
    size_t provider_cap;
    uint32_t stack_size;
    uint16_t priority;
    rtos_queue_t       req_q;
    rtos_queue_t       rsp_q;
    rtos_task_t        worker;
    rtos_mutex_t       flight_lock;
    uint32_t           flight_id;
    char               flight_session[128]; /* session_id of the current in-flight request */
    volatile bool      abort_flag;
    /* Heartbeat: updated at the start of every request; read by watchdog task. */
    volatile uint32_t  last_heartbeat_ms;
    rtos_task_t        watchdog_worker;
    struct {
        claw_agent_completion_observer_fn fn;
        void *ctx;
    } observers[MAX_OBSERVERS];
    size_t observer_cnt;

    /* Per-request scratch buffers: kept in the struct (heap) rather than on
     * the task stack to reduce engine_task peak stack depth by ~1.5 KB. */
    char prov_tags[TAGBUF_SIZE];
    char tool_tags[TAGBUF_SIZE];
    char tlog[TOOL_LOG_BUFSIZE];
} rtk_core_engine_t;

/* Single engine instance, defined in claw_agent.c. */
extern rtk_core_engine_t *g_engine;

/* ---- Assembled LLM request context (built by context.c, consumed by loop.c) ---- */

typedef struct {
    char  *sys_prompt;
    cJSON *messages;
    char  *tools_json;
} llm_ctx_t;

/* ---- Shared memory helper (owner: claw_agent.c) ---- */
char *claw_agent_str_clone(const char *s);

/* ---- Tag-buffer helper (owner: claw_agent_context.c) ----
 * Append `tag` to a comma-separated buffer; nodup skips duplicates. */
void claw_agent_tagbuf_add(char *buf, size_t bufsz, const char *tag, bool nodup);

/* ---- Request materialization (owner: claw_agent_context.c) ---- */

void claw_agent_llm_ctx_free(llm_ctx_t *c);

/* Fuse all context providers + the user turn + replayed tool history into a
 * complete {sys_prompt, messages, tools_json}. prov_tags collects the names of
 * the providers that contributed (for observer reporting). */
int  claw_agent_assemble_context(const rtk_req_node_t *rn,
                                 const cJSON *rt_msgs,
                                 llm_ctx_t *out,
                                 char *prov_tags,
                                 size_t prov_tags_sz);

/* Serialize one assistant tool-call round (the assistant message that carries
 * tool_calls / tool_use) into the wire format of the active backend. */
int  claw_agent_build_tool_call_round(cJSON *arr, const llm_resp_t *rsp);

/* Execute every tool the LLM requested and append the tool-result messages to
 * `arr` in the active backend's wire format. tlog accumulates a human-readable
 * trace of the calls. */
int  claw_agent_build_tool_result_round(cJSON *arr,
                                        const llm_resp_t *rsp,
                                        const claw_agent_request_t *req,
                                        char *tlog, size_t tlog_sz);

/* ---- Agentic loop (owner: claw_agent_loop.c) ---- */

/* Run the full tool-call loop for one request: assemble → LLM → tool rounds →
 * repeat until a final answer (or limit/abort/error). Fills `resp`, persists
 * the turn, and fires completion observers. */
void claw_agent_process_request(rtk_req_node_t *rn, rtk_resp_node_t *resp,
                                char *prov_tags, size_t prov_tags_sz,
                                char *tool_tags, size_t tool_tags_sz);

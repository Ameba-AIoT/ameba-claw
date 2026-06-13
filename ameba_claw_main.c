/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ameba_soc.h"
#include "ameba_claw_defs.h"
#include "claw_cap.h"
#include "claw_agent.h"
#include "claw_memory.h"
#include "claw_event_dispatcher.h"
#include "claw_event_publisher.h"
#include "cap_session_mgr.h"
#include <cJSON.h>
#include "platform_stdlib.h"
#include "basic_types.h"
#include "os_wrapper.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cap_scheduler.h"
#include "cap_web_search.h"
#include "cap_vision.h"
#include "cap_skill_mgr.h"
#include "cap_lua.h"
#include "cap_router_mgr.h"
#include "cap_mcp_client.h"
#include "cap_mcp_server.h"
#include "cap_im_attachment.h"
#include "cap_im_telegram.h"
#include "claw_im_dispatch.h"
#include "claw_http_server.h"
#include "cap_im_local.h"
#include "cap_im_feishu.h"
#include "cap_im_qq.h"
#include "cap_im_wechat.h"
#include "cap_time.h"
#include "cap_files.h"
#include "cap_system.h"
#include "cap_net_discover.h"
#include "cap_audio_stream.h"
#include "claw_config.h"
#include "claw_wifi_mgr.h"
#include "cap_webui.h"
#include "claw_memory_extract.h"
#include "claw_memory_compact.h"
#include "cap_honesty.h"
#include "cap_board_mgr.h"

#define TAG "ameba_claw"

extern void lua_task(void *param);

/* ---- System prompt loaded from rolfs:/SYSTEM.md at startup ---- */

static char *s_system_prompt_ptr = NULL;

static void load_system_prompt(void)
{
    FILE *f = fopen("rolfs:/SYSTEM.md", "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
        buf[--n] = '\0';
    s_system_prompt_ptr = buf;
}


/* ---- Tools context provider ---- */

static int collect_tools_context(const claw_agent_request_t *request,
                                        claw_agent_context_t *out_context,
                                        void *user_ctx)
{
    claw_cap_call_context_t cap_ctx = {0};
    char *tools_json;

    (void)user_ctx;

    /* Pass the session id so per-session cap_groups visibility (improvement
     * #12 Inc 6) narrows the injected tools to the groups this session has
     * activated via skills, in addition to the always-visible base set. */
    if (request) {
        cap_ctx.session_id = request->session_id;
    }

    /* Returns bare JSON array [{type:"function", function:{...}}, ...] */
    tools_json = claw_cap_build_llm_tools_json(&cap_ctx, false);
    if (!tools_json) {
        return RTK_FAIL;
    }

    out_context->kind    = CLAW_AGENT_CONTEXT_KIND_TOOLS;
    out_context->content = tools_json;
    return RTK_SUCCESS;
}

/* ---- Composition-root wiring callbacks ----
 *
 * These thin callbacks live here (the composition root / main.c) so that
 * cap_scheduler and claw_wifi_mgr have zero compile-time dependency on each
 * other.  main.c already includes both headers, so it is the only right place
 * to own this cross-cutting wire.
 */
static void on_wifi_connected_for_scheduler(void)
{
    cap_scheduler_fire_event("wifi_connected");
}

/* ---- IM conversation context provider ----
 *
 * Injects the current channel and chat_id into the system prompt so the
 * LLM always knows who it is talking to.  This lets it write scheduler
 * jobs or Lua scripts that reference the correct chat_id without the user
 * having to supply it explicitly.
 *
 * Only injected when source_channel is an IM channel (not serial/local) so
 * the information is actionable.  Returns RTK_FAIL (skipped) for serial and
 * other non-IM sessions where the chat_id is not meaningful for IM tools.
 */
static int collect_im_context(const claw_agent_request_t *request,
                               claw_agent_context_t *out_context,
                               void *user_ctx)
{
    (void)user_ctx;
    if (!request || !request->source_channel || !request->source_chat_id) return RTK_FAIL;

    /* Skip channels where scheduler reminders make no sense:
     * - SILENT_PROGRESS: serial/AT console (no persistent chat_id)
     * - EPHEMERAL_SESSION: local WebUI (chat_id is a browser session, gone
     *   when the tab closes — a scheduler job would fire into the void) */
    uint32_t flags = claw_im_dispatch_channel_flags(request->source_channel);
    if (flags & (CLAW_IM_CHANNEL_FLAG_SILENT_PROGRESS |
                 CLAW_IM_CHANNEL_FLAG_EPHEMERAL_SESSION)) return RTK_FAIL;

    /* Look up the send-text cap name for this channel so the LLM can use it
     * in scheduler jobs without hardcoding channel-specific cap names. */
    const char *send_cap = claw_im_dispatch_send_cap(request->source_channel);
    const char *cap = send_cap ? send_cap : "(none)";
    const char *cid = request->source_chat_id;

    /* Build the example cap_args JSON via cJSON so chat_id is always properly
     * escaped — a raw DiagSnPrintf would break if chat_id contained '"' or '\'. */
    char *example_cap_args = NULL;
    {
        cJSON *ex = cJSON_CreateObject();
        if (ex) {
            cJSON_AddStringToObject(ex, "chat_id", cid);
            cJSON_AddStringToObject(ex, "text", "...");
            example_cap_args = cJSON_PrintUnformatted(ex);
            cJSON_Delete(ex);
        }
    }
    if (!example_cap_args) return RTK_ERR_NOMEM;

    const char *fmt =
        "## Current conversation\n"
        "channel: %s\n"
        "chat_id: %s\n"
        "reply_cap: %s\n"
        "To send a scheduled message to this user:\n"
        "  scheduler_add_job cap_id=%s cap_args=%s delay_sec=N interval_sec=0\n";

    int n = DiagSnPrintf(NULL, 0, fmt,
                         request->source_channel, cid, cap, cap, example_cap_args);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { free(example_cap_args); return RTK_ERR_NOMEM; }
    DiagSnPrintf(buf, (size_t)n + 1, fmt,
                 request->source_channel, cid, cap, cap, example_cap_args);
    free(example_cap_args);

    out_context->kind    = CLAW_AGENT_CONTEXT_KIND_SYSTEM_PROMPT;
    out_context->content = buf;
    return RTK_SUCCESS;
}

static claw_agent_context_provider_t s_im_context_provider = {
    .name    = "im_conversation",
    .collect = collect_im_context,
};

/* ---- call_cap bridge (claw_agent → claw_cap) ---- */

static int call_cap(const char *cap_name, const char *input_json,
                           const claw_agent_request_t *request,
                           char **out_output, void *user_ctx)
{
    char *buf = NULL;
    int err;
    claw_cap_call_context_t ctx = {
        .request_id = request->request_id,
        .session_id = request->session_id,
        .channel    = request->source_channel,
        .chat_id    = request->source_chat_id,
        .caller     = CLAW_CAP_CALLER_LLM,
    };

    (void)user_ctx;

    err = claw_cap_call(cap_name, input_json, &ctx, &buf);
    *out_output = buf;
    return err;
}

/* ---- Per-tool progress messages with per-request rate limiting ----
 *
 * WeChat (and most IM platforms) rate-limit outbound messages.
 * We allow at most CLAW_IM_PROGRESS_BUDGET progress messages per request;
 * the final reply is always sent via on_response regardless of budget.
 *
 * Budget resets whenever the request_id changes, so each user message
 * gets its own fresh quota.
 */

typedef struct {
    uint32_t last_req;
    int      count;
} prog_state_t;

static void on_tool_progress(uint32_t    req_id,
                             const char *tool_name,
                             const char *tool_args,
                             const char *channel,
                             const char *chat_id,
                             void       *user_ctx)
{
    prog_state_t *st = (prog_state_t *)user_ctx;
    if (!channel || !chat_id) return;
    if (claw_im_dispatch_channel_has_flag(channel, CLAW_IM_CHANNEL_FLAG_SILENT_PROGRESS)) return;

    /* tool_name==NULL signals narration text (LLM thinking before tool calls).
     * Send directly without consuming progress budget. */
    if (!tool_name) {
        if (tool_args && tool_args[0])
            claw_im_dispatch_send(channel, chat_id, tool_args);
        return;
    }

    if (req_id != st->last_req) {
        st->last_req = req_id;
        st->count    = 0;
    }
    if (st->count >= CLAW_IM_PROGRESS_BUDGET) return;
    st->count++;

    /* If args contain a "name" field, append it: tool_name(value)
     * Works generically for skill_activate, skill_delete, or any future tool. */
    char detail[64] = {0};
    if (tool_args) {
        const char *n = strstr(tool_args, "\"name\"");
        if (n) {
            n = strchr(n, ':');
            if (n) {
                while (*n == ':' || *n == ' ' || *n == '"') n++;
                size_t l = 0;
                while (n[l] && n[l] != '"' && l < 31) l++;
                if (l > 0)
                    DiagSnPrintf(detail, sizeof(detail), "%s(%.*s)", tool_name, (int)l, n);
            }
        }
    }

    char msg[96];
    DiagSnPrintf(msg, sizeof(msg), "🔧 %s...", detail[0] ? detail : tool_name);
    claw_im_dispatch_send(channel, chat_id, msg);
}

/* ---- LLM response callback: routes completed responses to the originating IM channel ---- */

static void on_response(const claw_agent_response_t *resp, void *user_ctx)
{
    (void)user_ctx;
    if (!resp->source_channel || !resp->source_chat_id) return;
    if (resp->status != CLAW_AGENT_RESPONSE_STATUS_OK) {
        /* Suppress "request cancelled" (and its "(partial: ran [...])" variant) —
         * this fires when a new message preempts the current loop. The context
         * is now preserved via the partial-turn save, so surfacing the error
         * is just noise. strncmp covers both the bare message and the variant
         * produced by the tool_tags append path. */
        if (resp->error_message &&
                strncmp(resp->error_message, "request cancelled", 17) != 0) {
            claw_im_dispatch_send(resp->source_channel, resp->source_chat_id, resp->error_message);
        }
        return;
    }
    if (!resp->text) return;

    /* For IM channels, append the tool trace so the user can see what steps
     * were taken. Excluded: "serial" (already prints per-call output) and
     * "local" (the web dashboard is an interactive chat — the raw
     * "[tool_calls]..." trace is noise there and clutters Markdown rendering). */
    if (resp->tool_trace && resp->tool_trace[0] &&
            !claw_im_dispatch_channel_has_flag(resp->source_channel,
                                               CLAW_IM_CHANNEL_FLAG_SILENT_TRACE)) {
        size_t len = strlen(resp->text) + strlen(resp->tool_trace) + 4;
        char *full = malloc(len);
        if (full) {
            DiagSnPrintf(full, len, "%s\n\n%s", resp->text, resp->tool_trace);
            claw_im_dispatch_send(resp->source_channel, resp->source_chat_id, full);
            free(full);
            return;
        }
    }

    claw_im_dispatch_send(resp->source_channel, resp->source_chat_id, resp->text);
}

/* ---- Initialisation phases ------------------------------------------------
 *
 * ameba_claw_main() is the system entry point called once from the RTOS boot
 * task.  It is decomposed into six named phases so each phase is independently
 * readable without scrolling through a 300-line monolith.
 *
 * Dependency order (must not be reordered):
 *   config → core (cap+memory+session) → capabilities → agent → dispatcher → network+tasks
 * ----------------------------------------------------------------------- */

/* Phase 1: config + system prompt */
static void phase_config(void)
{
    claw_config_init();
    claw_config_ensure_api_token();
    load_system_prompt();
}

/* Phase 2: core infrastructure — cap registry, memory, session */
static void phase_core(const claw_config_t *cfg)
{
    /* claw_cap must come before claw_memory (memory registers cap groups) */
    claw_cap_init();

    static claw_memory_config_t s_mem_cfg = {
        .memory_root_dir         = "vfs:/memory",
        .session_root_dir        = "vfs:/session",
        .profile_root_dir        = "vfs:",
        .max_session_turns       = CLAW_MEMORY_MAX_SESSION_TURNS,
        .compaction_protect_last = CLAW_MEMORY_COMPACTION_PROTECT,
    };
    s_mem_cfg.compaction_token_threshold = cfg->llm.compact_tokens;
    s_mem_cfg.context_window_tokens      = cfg->llm.window_tokens;
    claw_memory_init(&s_mem_cfg);

    cap_session_mgr_init("vfs:/session");
}

/* Phase 3: capability modules — each registers its cap group */
static void phase_capabilities(const claw_config_t *cfg)
{
    { const cap_time_config_t c = { .ntp_server = "pool.ntp.org", .timezone_hrs = 8 };
      cap_time_init(&c); }

    { const cap_files_config_t c = { .max_read_size = CAP_FILES_MAX_READ_SIZE };
      cap_files_init(&c); }

    cap_system_init();

    { const cap_board_mgr_config_t c = { .vfs_path = "vfs:/board.json" };
      cap_board_mgr_init(&c); }

    cap_net_discover_init();
    cap_audio_stream_init();

    { cap_web_search_config_t c = {
          .api_key     = cfg->web_search.api_key[0] ? cfg->web_search.api_key : "",
          .max_results = cfg->web_search.max_results > 0 ? cfg->web_search.max_results : 3 };
      cap_web_search_init(&c); }

    /* Pass NULL to use claw_config vision section (model/base_url/api_path from
     * claw_config.json); only keep max_image_bytes here as it drives heap sizing. */
    { const cap_vision_config_t c = { .max_image_bytes = 2*1024*1024 };
      cap_vision_init(&c); }

    { const cap_skill_mgr_config_t c = { .skills_dir = "vfs:/skills" };
      cap_skill_mgr_init(&c); }

    cap_lua_init();

    { const cap_scheduler_config_t c = { .schedule_root_dir = "vfs:/scheduler",
                                          .max_jobs = CLAW_SCHEDULER_MAX_JOBS };
      cap_scheduler_init(&c); }

    /* Wire wifi_connected → scheduler from the composition root so neither module
     * depends on the other at compile time. */
    claw_wifi_mgr_register_on_connected(on_wifi_connected_for_scheduler);

    cap_im_attachment_init();

    { cap_im_telegram_config_t c = { .poll_timeout_sec = 25,
          .bot_token = cfg->telegram.bot_token[0] ? cfg->telegram.bot_token : "" };
      cap_im_telegram_init(&c); }
}

/* Phase 4: LLM agent — init + context providers + start */
static void phase_agent(const claw_config_t *cfg)
{
    static claw_agent_context_provider_t s_tools_provider = {
        .name = "tools", .collect = collect_tools_context,
    };
    static prog_state_t s_prog_state = {0};

    static claw_agent_config_t s_core_cfg = {
        .api_key               = "",
        .model                 = "",
        .base_url              = "open.bigmodel.cn",
        .api_path              = "/api/coding/paas/v4/chat/completions",
        .backend               = CLAW_LLM_BACKEND_OPENAI_BEARER,
        /* max_tokens / max_tool_iterations: 0 = filled from claw_config below */
        .max_tokens            = 0,
        .supports_tools        = true,
        .system_prompt         = "",
        .append_session_turn   = claw_memory_append_session_turn,
        .call_cap              = call_cap,
        .on_response           = on_response,
        .on_tool_progress      = on_tool_progress,
        .task_stack_size       = CLAW_AGENT_STACK_SIZE,
        .task_priority         = 2,
        .max_tool_iterations   = 0,
        .request_queue_len     = CLAW_AGENT_REQUEST_QUEUE_LEN,
        .response_queue_len    = CLAW_AGENT_REQUEST_QUEUE_LEN,
        .max_context_providers = CLAW_AGENT_MAX_CONTEXT_PROVIDERS,
    };
    s_core_cfg.on_tool_progress_user_ctx = &s_prog_state;
    if (s_system_prompt_ptr)       s_core_cfg.system_prompt = s_system_prompt_ptr;
    if (cfg->llm.api_key[0])       s_core_cfg.api_key       = cfg->llm.api_key;
    s_core_cfg.model   = cfg->llm.model;
    s_core_cfg.backend = (claw_llm_backend_t)cfg->llm.backend;
    /* claw_config holds the single source of truth for these limits */
    s_core_cfg.max_tokens          = cfg->llm.max_tokens;
    s_core_cfg.max_tool_iterations = cfg->llm.max_iterations;
    if (s_core_cfg.max_tool_iterations < CLAW_AGENT_TOOL_ITER_MIN)
        s_core_cfg.max_tool_iterations = CLAW_AGENT_TOOL_ITER_MIN;

    claw_cap_start_all();
    cap_skill_mgr_apply_base_visibility();

    if (claw_agent_init(&s_core_cfg) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "claw_agent_init failed\n");
        return;
    }

    const claw_agent_context_provider_t *providers[] = {
        &claw_memory_profile_provider,
        &claw_memory_compaction_summary_provider,
        &claw_memory_session_history_provider,
        &claw_memory_long_term_label_provider,
        &s_tools_provider,
        &cap_time_context_provider,
        &cap_skill_mgr_context_provider,
        &cap_skill_catalog_provider,
        &cap_board_mgr_context_provider,
        &s_im_context_provider,
    };
    for (size_t i = 0; i < sizeof(providers) / sizeof(providers[0]); i++)
        claw_agent_add_context_provider(providers[i]);

    claw_memory_extract_init();
    claw_agent_add_completion_observer(claw_memory_extract_observer, NULL);
    claw_agent_add_completion_observer(cap_honesty_observe_completion, NULL);
    claw_agent_start();
}

/* Phase 5: event dispatcher + HTTP server + IM channels */
static void phase_network(const claw_config_t *cfg)
{
    static claw_event_dispatcher_config_t s_router_cfg = {
        .max_rules                       = 16,
        .max_actions_per_rule            = 4,
        .event_queue_len                 = 8,
        .task_stack_size                 = 8 * 1024,
        .task_priority                   = 2,
        .core_submit_timeout_ms          = 5000,
        .default_route_messages_to_agent = true,
    };
    s_router_cfg.session_builder = cap_session_mgr_build_session_id;

    if (claw_event_dispatcher_init(&s_router_cfg) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "claw_event_dispatcher_init failed\n");
        return;
    }
    claw_event_dispatcher_start();
    cap_im_attachment_start();
    cap_im_telegram_start();
    cap_scheduler_start();

    /* HTTP server: all routes must be registered before claw_http_server_start() */
    { claw_http_server_config_t c = CLAW_HTTP_SERVER_DEFAULT_CONFIG();
      claw_http_server_init(&c); }

    cap_webui_init();

    { cap_im_local_config_t c = CAP_IM_LOCAL_DEFAULT_CONFIG();
      cap_im_local_init(&c); cap_im_local_start(); }

    { cap_im_feishu_config_t c = CAP_IM_FEISHU_DEFAULT_CONFIG();
      if (cfg->feishu.app_id[0])     strlcpy(c.app_id,     cfg->feishu.app_id,     sizeof(c.app_id));
      if (cfg->feishu.app_secret[0]) strlcpy(c.app_secret, cfg->feishu.app_secret, sizeof(c.app_secret));
      cap_im_feishu_init(&c); cap_im_feishu_start(); }

    { cap_im_qq_config_t c = CAP_IM_QQ_DEFAULT_CONFIG();
      cap_im_qq_init(&c); cap_im_qq_start(); }

    { cap_im_wechat_config_t c = CAP_IM_WECHAT_DEFAULT_CONFIG();
      cap_im_wechat_init(&c); cap_im_wechat_start(); }

    { cap_mcp_server_config_t c = CAP_MCP_SERVER_DEFAULT_CONFIG();
      cap_mcp_server_init(&c); }

    claw_http_server_start();

    { const cap_router_mgr_config_t c = { .rules_dir = "vfs:/router_rules", .max_rules = 32 };
      cap_router_mgr_init(&c); }
}

/* Phase 6: background tasks + serial console */
static void phase_tasks(const claw_config_t *cfg)
{
    (void)cfg;

    { const cap_mcp_client_config_t c = { .config_dir = "vfs:/mcp" };
      cap_mcp_client_init(&c); }

    if (rtos_task_create(NULL, "lua_task", lua_task, NULL, 8192, 1) != RTK_SUCCESS)
        RTK_LOGE(TAG, "lua_task create failed\n");

    /* WiFi must start after scheduler (APIs require RTOS running) */
    if (rtos_task_create(NULL, "wifi_mgr", claw_wifi_mgr_task_entry, NULL, 8192, 2) != RTK_SUCCESS)
        RTK_LOGE(TAG, "wifi_mgr create failed\n");

    extern void at_claw_init(void);
    at_claw_init();
}

/* ---- Entry point ---- */

void ameba_claw_main(void)
{
    phase_config();
    const claw_config_t *cfg = claw_config_get();
    phase_core(cfg);
    phase_capabilities(cfg);
    phase_agent(cfg);
    phase_network(cfg);
    phase_tasks(cfg);
}

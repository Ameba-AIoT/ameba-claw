/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ameba_soc.h"
#include "ameba_claw_defs.h"
#include "claw_cap.h"
#include "claw_cap_registry.h"
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
#include "claw_im_dispatch.h"
#include "claw_http_server.h"
/* cap_im_local (send_for_alias, routes to the originating WebUI session) is
 * optional; a no-op stub when trimmed keeps the call sites below #ifdef-free. */
#ifdef CONFIG_CLAW_CAP_IM_LOCAL
#include "cap_im_local.h"
#else
static inline void cap_im_local_send_for_alias(const char *alias, const char *text) { (void)alias; (void)text; }
#endif
#include "claw_config.h"
#include "claw_wifi_mgr.h"
#include "claw_memory_extract.h"
#include "claw_memory_compact.h"
#include "cap_atcmd.h"

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

    /* Session id lets per-session visibility narrow the injected tools to the
     * groups this session activated via skills (plus the always-visible base). */
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

/* ---- IM conversation context provider ----
 * Injects the current channel + chat_id into the system prompt so the LLM can
 * target scheduler jobs / Lua at the right chat without the user restating it.
 * Skipped (RTK_FAIL) on serial/local, where chat_id is not IM-actionable. */
static int collect_im_context(const claw_agent_request_t *request,
                               claw_agent_context_t *out_context,
                               void *user_ctx)
{
    (void)user_ctx;
    if (!request || !request->source_channel || !request->source_chat_id) return RTK_FAIL;

    /* Skip channels where a scheduler reminder makes no sense: serial/AT console
     * (no persistent chat_id) and local WebUI (chat_id dies with the browser tab). */
    uint32_t flags = claw_im_dispatch_channel_flags(request->source_channel);
    if (flags & (CLAW_IM_CHANNEL_FLAG_SILENT_PROGRESS |
                 CLAW_IM_CHANNEL_FLAG_EPHEMERAL_SESSION)) return RTK_FAIL;

    /* send-text cap for this channel, so the LLM needn't hardcode it in jobs. */
    const char *send_cap = claw_im_dispatch_send_cap(request->source_channel);
    const char *cap = send_cap ? send_cap : "(none)";
    const char *cid = request->source_chat_id;

    /* Build via cJSON so chat_id is escaped (a raw printf breaks on '"' or '\'). */
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
    .name       = "im_conversation",
    .collect    = collect_im_context,
    .quiet_skip = true,  /* always skips on serial/local channels — expected */
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

/* ---- Per-tool progress messages, rate-limited per request ----
 * IM platforms (WeChat) rate-limit outbound, so cap at
 * CLAW_IM_PROGRESS_BUDGET progress msgs per request (the final reply always
 * goes out via on_response). Budget resets per request_id. */

typedef struct {
    uint32_t last_req;
    int      count;
} prog_state_t;

static void on_tool_progress(uint32_t    req_id,
                             const char *tool_name,
                             const char *tool_args,
                             const char *channel,
                             const char *chat_id,
                             const char *source_message_id,
                             void       *user_ctx)
{
    prog_state_t *st = (prog_state_t *)user_ctx;
    if (!channel || !chat_id) return;
    if (claw_im_dispatch_channel_has_flag(channel, CLAW_IM_CHANNEL_FLAG_SILENT_PROGRESS)) {
        /* Narration (tool_name==NULL) still echoed to serial when progress is silenced. */
        if (!tool_name && tool_args && tool_args[0]) at_claw_serial_echo(tool_args);
        return;
    }

    /* tool_name==NULL = narration text: route like a tool message so per-channel
     * rate limiters apply, and mirror to serial for non-serial active sessions. */
    if (!tool_name) {
        if (tool_args && tool_args[0]) {
            /* local WebUI: route by alias so a client-side session switch can't
             * misroute (cap_session_mgr_get_current would return the wrong one). */
            if (strcmp(channel, "local") == 0 &&
                    source_message_id && source_message_id[0]) {
                cap_im_local_send_for_alias(source_message_id, tool_args);
            } else if (claw_im_dispatch_send_progress(channel, chat_id, tool_args, req_id) != 0) {
                claw_im_dispatch_send(channel, chat_id, tool_args);
            }
            at_claw_serial_echo(tool_args);
        }
        return;
    }

    /* If args carry a "name" field, append it: tool_name(value). Generic. */
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

    /* local WebUI: route by originating alias (see above). */
    if (strcmp(channel, "local") == 0 &&
            source_message_id && source_message_id[0]) {
        cap_im_local_send_for_alias(source_message_id, msg);
        return;
    }

    /* A channel with its own progress handler owns rate-limiting/budgeting;
     * otherwise fall back to the generic budget-limited send below. */
    if (claw_im_dispatch_send_progress(channel, chat_id, msg, req_id) == 0)
        return;

    if (req_id != st->last_req) {
        st->last_req = req_id;
        st->count    = 0;
    }
    if (st->count >= CLAW_IM_PROGRESS_BUDGET) return;
    st->count++;
    claw_im_dispatch_send(channel, chat_id, msg);
}

/* ---- LLM response callback: routes completed responses to the originating IM channel ---- */

static void on_response(const claw_agent_response_t *resp, void *user_ctx)
{
    (void)user_ctx;
    if (!resp->source_channel || !resp->source_chat_id) return;
    if (resp->status != CLAW_AGENT_RESPONSE_STATUS_OK) {
        /* Suppress "request cancelled" (and its "(partial: ...)" variant, hence
         * strncmp): it fires when a new message preempts the loop, and the
         * partial-turn save already preserves context — surfacing it is noise. */
        if (resp->error_message &&
                strncmp(resp->error_message, "request cancelled", 17) != 0) {
            claw_im_dispatch_send(resp->source_channel, resp->source_chat_id, resp->error_message);
        }
        return;
    }
    if (!resp->text) return;

    /* Mirror final text to serial for non-serial sessions (serial channels get
     * it via their own send path, so skip to avoid duplication). */
    if (!claw_im_dispatch_channel_has_flag(resp->source_channel,
                                           CLAW_IM_CHANNEL_FLAG_SILENT_PROGRESS)) {
        at_claw_serial_echo(resp->text);
    }

    /* Append the tool trace for IM channels; suppressed on serial/local
     * (SILENT_TRACE) where the raw "[tool_calls]..." dump is just noise. */
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

    /* local WebUI: route by originating alias so the reply lands in the request's
     * session regardless of the server-current one. */
    if (strcmp(resp->source_channel, "local") == 0 &&
            resp->source_message_id && resp->source_message_id[0]) {
        cap_im_local_send_for_alias(resp->source_message_id, resp->text);
        return;
    }

    claw_im_dispatch_send(resp->source_channel, resp->source_chat_id, resp->text);
}

/* ---- Initialisation phases ----
 * ameba_claw_main() runs these once from the RTOS boot task, in this order
 * (must not be reordered):
 *   config → core (cap+memory+session) → capabilities → agent → io → tasks
 * ------------------------------------------------------------------------- */

/* Phase 1: config + system prompt */
static void phase_config(void)
{
    claw_config_init();
    load_system_prompt();
}

/* Phase 2: core infrastructure — cap registry, memory, session */
static void phase_core(const claw_config_t *cfg)
{
    /* claw_cap must come before claw_memory (memory registers cap groups) */
    claw_cap_init();

    claw_memory_config_t s_mem_cfg = {
        .memory_root_dir             = "vfs:/memory",
        .session_root_dir            = "vfs:/session",
        .profile_root_dir            = "vfs:",
        .max_session_turns           = CLAW_MEMORY_MAX_SESSION_TURNS,
        .compaction_protect_last     = CLAW_MEMORY_COMPACTION_PROTECT,
        .compaction_token_threshold  = cfg->llm.compact_tokens,
        .context_window_tokens       = cfg->llm.window_tokens,
    };
    claw_memory_init(&s_mem_cfg);

    cap_session_mgr_init(s_mem_cfg.session_root_dir);
}

/* Phase 3: drive every cap's INIT-phase hook (group registration). No cap is
 * named here; groups registered now are visible to phase_agent's snapshot. */
static void phase_capabilities(const claw_config_t *cfg)
{
    claw_cap_registry_run(CLAW_CAP_PHASE_INIT, cfg);
}

/* Phase 4: LLM agent — init + context providers + start */
static void phase_agent(const claw_config_t *cfg)
{
    static claw_agent_context_provider_t s_tools_provider = {
        .name = "tools", .collect = collect_tools_context,
    };
    static prog_state_t s_prog_state = {0};

    /* Fixed wiring only: endpoint defaults, callbacks, task/queue sizing.
     * The LLM settings (api_key/model/backend/max_tokens/max_tool_iterations/
     * system_prompt) are overlaid from claw_config below — claw_config is their
     * single source of truth, so they are intentionally left unset here. */
    static claw_agent_config_t s_core_cfg = {
        .base_url                  = "open.bigmodel.cn",
        .api_path                  = "/api/coding/paas/v4/chat/completions",
        .supports_tools            = true,
        .append_session_turn       = claw_memory_append_session_turn,
        .call_cap                  = call_cap,
        .on_response               = on_response,
        .on_tool_progress          = on_tool_progress,
        .on_tool_progress_user_ctx = &s_prog_state,
        .task_stack_size           = CLAW_AGENT_STACK_SIZE,
        .task_priority             = 2,
        .request_queue_len         = CLAW_AGENT_REQUEST_QUEUE_LEN,
        .response_queue_len        = CLAW_AGENT_REQUEST_QUEUE_LEN,
        .max_context_providers     = CLAW_AGENT_MAX_CONTEXT_PROVIDERS,
    };

    /* Overlay LLM settings from claw_config. */
    s_core_cfg.api_key       = cfg->llm.api_key[0] ? cfg->llm.api_key : "";
    s_core_cfg.model         = cfg->llm.model;
    s_core_cfg.backend       = (claw_llm_backend_t)cfg->llm.backend;
    s_core_cfg.max_tokens    = cfg->llm.max_tokens;
    s_core_cfg.system_prompt = s_system_prompt_ptr ? s_system_prompt_ptr : "";

    /* Floor max_tool_iterations so multi-step skill creation has enough rounds. */
    s_core_cfg.max_tool_iterations =
        (cfg->llm.max_iterations < CLAW_AGENT_TOOL_ITER_MIN)
            ? CLAW_AGENT_TOOL_ITER_MIN : cfg->llm.max_iterations;

    claw_cap_start_all();

    if (claw_agent_init(&s_core_cfg) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "claw_agent_init failed\n");
        return;
    }

    /* Core providers: memory pipeline + tools. Registered first so the system
     * prompt / message ordering matches the historical composition. */
    const claw_agent_context_provider_t *core_providers[] = {
        &claw_memory_profile_provider,
        &claw_memory_compaction_summary_provider,
        &claw_memory_session_history_provider,
        &claw_memory_long_term_label_provider,
        &s_tools_provider,
    };
    for (size_t i = 0; i < sizeof(core_providers) / sizeof(core_providers[0]); i++)
        claw_agent_add_context_provider(core_providers[i]);

    /* Caps' AGENT-phase hooks add their context providers / observers here, in
     * ascending `order` (table in claw_cap_registry.h): after the core providers
     * above, before the IM provider below — reproducing the prior order. */
    claw_cap_registry_run(CLAW_CAP_PHASE_AGENT, cfg);

    /* Composition-root IM context provider — last, trailing all cap providers. */
    claw_agent_add_context_provider(&s_im_context_provider);

    claw_memory_extract_init();
    claw_agent_add_completion_observer(claw_memory_extract_observer, NULL);
    claw_agent_start();
}

/* Phase 5: I/O layer — event routing, HTTP server, IM channels, outbound
 * services. Everything that moves data in or out of the agent core. */
static void phase_io(const claw_config_t *cfg)
{
    /* --- Step 1: event router (inbound core) ---
     * Owns the (match → action) rules and the session_builder ((channel,chat_id)
     * → session_id). Must start before anything publishes events into it. */
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

    /* --- Step 2: HTTP server init (core) ---
     * All HTTP routes must be registered before claw_http_server_start(), so
     * http_init precedes the route-registering cap IO hooks below. */
    { claw_http_server_config_t c = CLAW_HTTP_SERVER_DEFAULT_CONFIG();
      claw_http_server_init(&c); }

    /* --- Step 3: every cap's IO-phase hook, in ascending `order` ---
     * Dispatcher-dependent services (im_attachment, scheduler), wifi on-connected
     * hooks (before the wifi task in phase_tasks), HTTP-route registrars (before
     * http_server_start below), IM channels, and router_mgr last. Order table in
     * claw_cap_registry.h; router_mgr's high order keeps its group hidden from
     * the LLM (registered after phase_agent's visibility snapshot). */
    claw_cap_registry_run(CLAW_CAP_PHASE_IO, cfg);

    /* Serial/AT console — a core outbound channel ("serial", no HTTP route),
     * before http_server_start alongside the IM channels. */
    at_claw_init();

    /* --- Step 4: start the HTTP server (no more routes may be added after this) --- */
    claw_http_server_start();
}

/* Phase 6: background tasks */
static void phase_tasks(const claw_config_t *cfg)
{
    (void)cfg;

    if (rtos_task_create(NULL, "lua_task", lua_task, NULL, 8192, 1) != RTK_SUCCESS)
        RTK_LOGE(TAG, "lua_task create failed\n");

    /* WiFi must start after scheduler (APIs require RTOS running) */
    if (rtos_task_create(NULL, "wifi_mgr", claw_wifi_mgr_task_entry, NULL, 8192, 2) != RTK_SUCCESS)
        RTK_LOGE(TAG, "wifi_mgr create failed\n");
}

/* ---- Entry point ---- */

void ameba_claw_main(void)
{
    phase_config();
    const claw_config_t *cfg = claw_config_get();
    phase_core(cfg);
    phase_capabilities(cfg);
    phase_agent(cfg);
    phase_io(cfg);
    phase_tasks(cfg);
}

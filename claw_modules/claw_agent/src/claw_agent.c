/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Engine shell: lifecycle, FreeRTOS plumbing and the public API.
 *
 * This file owns the single engine instance (g_engine), the request/response
 * queues and node lifecycle, the receive / receive_for infrastructure, the
 * watchdog and worker tasks, and every claw_agent_* public entry point. The
 * actual per-request work is delegated to claw_agent_process_request()
 * (claw_agent_loop.c), which in turn uses claw_agent_context.c to build the
 * LLM request and materialize tool round-trips.
 */

#include "ameba_soc.h"
#include "claw_agent_internal.h"
#include "claw_agent_llm.h"
#include "claw_config.h"
#include "ameba_claw_defs.h"
#include "sys_api.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "os_wrapper.h"

static const char *TAG = "claw_agent";

/* ---- Engine-only build-time tunables ----
 * (Cross-TU limits such as TAGBUF_SIZE / TOOL_LOG_BUFSIZE / MAX_OBSERVERS live
 *  in claw_agent_internal.h; these are referenced only here.) */
#define ENGINE_STACK_DEFAULT    (16 * 1024)
#define ENGINE_PRIO_DEFAULT     3
#define ENGINE_REQ_DEPTH        4
#define ENGINE_RSP_DEPTH        4

/* Watchdog task constants */
#define WATCHDOG_CHECK_MS              15000u   /* check interval */
#define WATCHDOG_THRESHOLD_MS         420000u   /* 7 min silent flight → reboot */

/* The single engine instance (declared extern in claw_agent_internal.h). */
rtk_core_engine_t *g_engine = NULL;

/* ---- Response receive infrastructure (receive / receive_for) ---- */

/* Pending response node — holds responses dequeued by receive_for() that did
 * not match the caller's request_id.  Preserved for subsequent receive calls. */
typedef struct claw_pending_resp {
    rtk_resp_node_t          node;
    struct claw_pending_resp *next;
} claw_pending_resp_t;

static claw_pending_resp_t *s_pending_head = NULL;
static rtos_mutex_t         s_recv_lock;

/* ---- Memory helpers ---- */

char *claw_agent_str_clone(const char *s)
{
    return s ? strdup(s) : NULL;
}

/* ---- Request/response node lifecycle ---- */

static void req_node_free(rtk_req_node_t *n)
{
    if (!n) {
        return;
    }
    free(n->sid);
    free(n->utext);
    free(n->src_ch);
    free(n->src_cid);
    free(n->src_uid);
    free(n->src_mid);
    free(n->src_cap);
    _memset(n, 0, sizeof(*n));
}

static void resp_node_free(rtk_resp_node_t *n)
{
    if (!n) {
        return;
    }
    free(n->pub.source_channel);
    free(n->pub.source_chat_id);
    free(n->pub.text);
    free(n->pub.error_message);
    _memset(n, 0, sizeof(*n));
}

/* ---- Pending response list helpers (used by receive_for) ---- */

/* Append a response node to the pending list; ownership of string pointers
 * transfers to the pending node.  On OOM the node is left unchanged so the
 * caller can free it with resp_node_free(). */
static int recv_push_pending(rtk_resp_node_t *node)
{
    claw_pending_resp_t *n = malloc(sizeof(claw_pending_resp_t));
    if (!n) {
        return RTK_ERR_NOMEM;
    }
    n->node = *node;
    n->next = NULL;
    if (!s_pending_head) {
        s_pending_head = n;
    } else {
        claw_pending_resp_t *t = s_pending_head;
        while (t->next) {
            t = t->next;
        }
        t->next = n;
    }
    node->pub.text           = NULL;
    node->pub.error_message  = NULL;
    node->pub.source_channel = NULL;
    node->pub.source_chat_id = NULL;
    node->pub.tool_trace     = NULL;
    return RTK_SUCCESS;
}

/* Remove and return the first pending response matching req_id (or any if
 * match_any).  Returns RTK_SUCCESS and fills *out on success; RTK_FAIL if no
 * matching entry is found. */
static int recv_pop_pending(uint32_t req_id, bool match_any,
                            claw_agent_response_t *out)
{
    claw_pending_resp_t *prev = NULL;
    claw_pending_resp_t *cur  = s_pending_head;

    while (cur) {
        if (match_any || cur->node.pub.request_id == req_id) {
            if (prev) {
                prev->next = cur->next;
            } else {
                s_pending_head = cur->next;
            }
            *out = cur->node.pub;
            cur->node.pub.text           = NULL;
            cur->node.pub.error_message  = NULL;
            cur->node.pub.source_channel = NULL;
            cur->node.pub.source_chat_id = NULL;
            cur->node.pub.tool_trace     = NULL;
            free(cur);
            return RTK_SUCCESS;
        }
        prev = cur;
        cur  = cur->next;
    }
    return RTK_FAIL;
}

/* Push response onto the queue for receive / receive_for callers. */
static int resp_send(rtk_resp_node_t *node)
{
    if (rtos_queue_send(g_engine->rsp_q, node, 0) != RTK_SUCCESS) {
        return RTK_FAIL;
    }
    node->pub.source_channel = NULL;
    node->pub.source_chat_id = NULL;
    node->pub.text           = NULL;
    node->pub.error_message  = NULL;
    return RTK_SUCCESS;
}

/* ---- Watchdog task ---- */

static void watchdog_task(void *arg)
{
    (void)arg;
    while (true) {
        rtos_time_delay_ms(WATCHDOG_CHECK_MS);
        if (!g_engine || !g_engine->running) {
            break;
        }
        if (g_engine->flight_id == 0 || g_engine->last_heartbeat_ms == 0) {
            continue;   /* idle or not yet started */
        }
        uint32_t age = rtos_time_get_current_system_time_ms()
                       - g_engine->last_heartbeat_ms;
        if (age > WATCHDOG_THRESHOLD_MS) {
            RTK_LOGE(TAG, "watchdog: engine silent %ums (req=%" PRIu32 "), rebooting\n",
                     (unsigned)age, g_engine->flight_id);
            /* sys_reset() triggers a CPU soft-reset, similar to NVIC_SystemReset. */
            sys_reset();
        }
    }
    rtos_task_delete(NULL);
}

/* ---- Worker task ---- */

static void engine_task(void *arg)
{
    (void)arg;
    rtos_create_secure_context(RTOS_MINIMAL_SECURE_STACK_SIZE);

    while (true) {
        rtk_req_node_t  rn   = {0};
        rtk_resp_node_t resp = {0};

        /* Use struct-resident scratch buffers instead of stack locals (~768B saved) */
        char *prov_tags = g_engine->prov_tags;
        char *tool_tags = g_engine->tool_tags;
        _memset(prov_tags, 0, TAGBUF_SIZE);
        _memset(tool_tags, 0, TAGBUF_SIZE);

        if (rtos_queue_receive(g_engine->req_q, &rn, 0xFFFFFFFFUL) != RTK_SUCCESS) {
            continue;
        }

        if (rtos_mutex_take(g_engine->flight_lock, 0xFFFFFFFFUL) == RTK_SUCCESS) {
            g_engine->flight_id          = rn.pub.request_id;
            g_engine->last_heartbeat_ms  = rtos_time_get_current_system_time_ms();
            g_engine->abort_flag         = false;
            strncpy(g_engine->flight_session,
                    rn.pub.session_id ? rn.pub.session_id : "",
                    sizeof(g_engine->flight_session) - 1);
            g_engine->flight_session[sizeof(g_engine->flight_session) - 1] = '\0';
            rtos_mutex_give(g_engine->flight_lock);
        }

        resp.pub.request_id      = rn.pub.request_id;
        resp.pub.status          = CLAW_AGENT_RESPONSE_STATUS_ERROR;
        resp.pub.completion_type = CLAW_AGENT_COMPLETION_DONE;
        resp.pub.source_channel     = claw_agent_str_clone(rn.pub.source_channel);
        resp.pub.source_chat_id     = claw_agent_str_clone(rn.pub.source_chat_id);
        resp.pub.source_message_id  = claw_agent_str_clone(rn.pub.source_message_id);

        if (g_engine->on_start) {
            g_engine->on_start(&rn.pub, g_engine->on_start_ctx);
        }

        claw_agent_process_request(&rn, &resp,
                        prov_tags, TAGBUF_SIZE,
                        tool_tags, TAGBUF_SIZE);

        if (rtos_mutex_take(g_engine->flight_lock, 0xFFFFFFFFUL) == RTK_SUCCESS) {
            g_engine->flight_id        = 0;
            g_engine->flight_session[0] = '\0';
            g_engine->abort_flag       = false;
            rtos_mutex_give(g_engine->flight_lock);
        }

        if (rn.pub.flags & CLAW_AGENT_REQUEST_FLAG_SYNC_RECEIVE) {
            /* Caller uses receive_for() — push to response queue. */
            if (resp_send(&resp) != RTK_SUCCESS) {
                RTK_LOGE(TAG, "resp enqueue failed req=%" PRIu32 "\n",
                         rn.pub.request_id);
                resp_node_free(&resp);
            }
        } else if (g_engine->on_response) {
            g_engine->on_response(&resp.pub, g_engine->on_response_ctx);
            resp_node_free(&resp);
        } else if (resp_send(&resp) != RTK_SUCCESS) {
            RTK_LOGE(TAG, "resp enqueue failed req=%" PRIu32 "\n",
                     rn.pub.request_id);
            resp_node_free(&resp);
        }

        req_node_free(&rn);
    }
}

/* ---- Public API ---- */

int claw_agent_init(const claw_agent_config_t *cfg)
{
    char  *llm_err  = NULL;
    uint32_t req_depth, rsp_depth;
    int rc;

    if (!cfg || !cfg->system_prompt || !cfg->api_key || !cfg->model) {
        return RTK_ERR_BADARG;
    }
    if (g_engine && g_engine->ready) {
        return RTK_FAIL;
    }

    g_engine = calloc(1, sizeof(*g_engine));
    if (!g_engine) {
        return RTK_ERR_NOMEM;
    }

    g_engine->sys_prompt = claw_agent_str_clone(cfg->system_prompt);
    if (!g_engine->sys_prompt) {
        free(g_engine);
        g_engine = NULL;
        return RTK_ERR_NOMEM;
    }

    g_engine->save_turn        = cfg->append_session_turn;
    g_engine->save_turn_ctx    = cfg->append_session_turn_user_ctx;
    g_engine->on_start         = cfg->on_request_start;
    g_engine->on_start_ctx     = cfg->on_request_start_user_ctx;
    g_engine->dispatch_cap     = cfg->call_cap;
    g_engine->dispatch_cap_ctx = cfg->cap_user_ctx;
    g_engine->on_response          = cfg->on_response;
    g_engine->on_response_ctx      = cfg->on_response_user_ctx;
    g_engine->on_tool_progress     = cfg->on_tool_progress;
    g_engine->on_tool_progress_ctx = cfg->on_tool_progress_user_ctx;

    req_depth            = cfg->request_queue_len  ? cfg->request_queue_len  : ENGINE_REQ_DEPTH;
    rsp_depth            = cfg->response_queue_len ? cfg->response_queue_len : ENGINE_RSP_DEPTH;
    g_engine->stack_size = cfg->task_stack_size    ? cfg->task_stack_size    : ENGINE_STACK_DEFAULT;
    g_engine->priority   = cfg->task_priority      ? cfg->task_priority      : ENGINE_PRIO_DEFAULT;
    g_engine->provider_cap  = cfg->max_context_providers;

    if (g_engine->provider_cap > 0) {
        g_engine->providers = calloc(g_engine->provider_cap,
                                     sizeof(claw_agent_context_provider_t));
        if (!g_engine->providers) {
            free(g_engine->sys_prompt);
            free(g_engine);
            g_engine = NULL;
            return RTK_ERR_NOMEM;
        }
    }

    /* Initialize receive/receive_for mutex once. */
    if (!s_recv_lock) {
        if (rtos_mutex_create(&s_recv_lock) != RTK_SUCCESS) {
            free(g_engine->sys_prompt);
            free(g_engine);
            g_engine = NULL;
            return RTK_ERR_NOMEM;
        }
    }

    {
        int qrc1, qrc2, mrc;
        qrc1 = rtos_queue_create(&g_engine->req_q, req_depth, sizeof(rtk_req_node_t));
        qrc2 = rtos_queue_create(&g_engine->rsp_q, rsp_depth, sizeof(rtk_resp_node_t));
        mrc  = rtos_mutex_create(&g_engine->flight_lock);

        if (qrc1 != RTK_SUCCESS || qrc2 != RTK_SUCCESS || mrc != RTK_SUCCESS) {
            RTK_LOGE(TAG, "FreeRTOS primitives alloc failed\n");
            if (qrc1 == RTK_SUCCESS) rtos_queue_delete(g_engine->req_q);
            if (qrc2 == RTK_SUCCESS) rtos_queue_delete(g_engine->rsp_q);
            if (mrc  == RTK_SUCCESS) rtos_mutex_delete(g_engine->flight_lock);
            free(g_engine->providers);
            free(g_engine->sys_prompt);
            free(g_engine);
            g_engine = NULL;
            return RTK_ERR_NOMEM;
        }
    }

    /* Plumb compile-time defaults (api_key/model/host/path/backend baked
     * into main.c's s_core_cfg) into the LLM HTTP layer. Without this,
     * those fields would be dead — the HTTP layer reads claw_config
     * directly and would only see empty strings on a fresh device. */
    claw_agent_llm_set_defaults(cfg->api_key, cfg->model,
                                cfg->base_url, cfg->api_path,
                                cfg->backend);

    rc = claw_agent_llm_init(&llm_err);
    if (rc != RTK_SUCCESS) {
        RTK_LOGE(TAG, "LLM init: %s\n", llm_err ? llm_err : rtk_err_to_name(rc));
        free(llm_err);
        rtos_queue_delete(g_engine->req_q);
        rtos_queue_delete(g_engine->rsp_q);
        rtos_mutex_delete(g_engine->flight_lock);
        free(g_engine->providers);
        free(g_engine->sys_prompt);
        free(g_engine);
        g_engine = NULL;
        return rc;
    }

    g_engine->ready = true;
    RTK_LOGI(TAG, "engine ready model=%s\n", cfg->model);
    return RTK_SUCCESS;
}

int claw_agent_start(void)
{
    int ret;

    if (!g_engine || !g_engine->ready) {
        return RTK_FAIL;
    }
    if (g_engine->running) {
        return RTK_SUCCESS;
    }

    ret = rtos_task_create(&g_engine->worker, "claw_agent", engine_task,
                           NULL, g_engine->stack_size, g_engine->priority);
    if (ret != 0) {
        RTK_LOGE(TAG, "task create failed (%d)\n", ret);
        return RTK_FAIL;
    }

    /* Watchdog task: 2 KB stack, lowest priority, checks engine heartbeat. */
    rtos_task_create(&g_engine->watchdog_worker, "claw_wdg", watchdog_task,
                     NULL, 2 * 1024, 1);

    g_engine->running = true;
    RTK_LOGI(TAG, "engine task started\n");
    return RTK_SUCCESS;
}

int claw_agent_add_context_provider(const claw_agent_context_provider_t *p)
{
    claw_agent_context_provider_t *slot;

    if (!g_engine || !g_engine->ready || g_engine->running) {
        return RTK_FAIL;
    }
    if (!p || !p->name || !p->collect) {
        return RTK_ERR_BADARG;
    }
    if (g_engine->provider_cnt >= g_engine->provider_cap) {
        return RTK_ERR_NOMEM;
    }

    slot = &g_engine->providers[g_engine->provider_cnt];
    /* Copy every field (incl. quiet_skip and any future ones), then replace
     * the name with an owned clone — the caller's struct may be stack-local. */
    *slot = *p;
    slot->name = claw_agent_str_clone(p->name);
    if (!slot->name) {
        return RTK_ERR_NOMEM;
    }
    g_engine->provider_cnt++;
    return RTK_SUCCESS;
}

int claw_agent_add_completion_observer(claw_agent_completion_observer_fn fn,
                                            void *ctx)
{
    if (!g_engine || !g_engine->ready) {
        return RTK_FAIL;
    }
    if (!fn) {
        return RTK_ERR_BADARG;
    }
    if (g_engine->observer_cnt >= MAX_OBSERVERS) {
        return RTK_ERR_NOMEM;
    }
    g_engine->observers[g_engine->observer_cnt].fn  = fn;
    g_engine->observers[g_engine->observer_cnt].ctx = ctx;
    g_engine->observer_cnt++;
    return RTK_SUCCESS;
}

int claw_agent_call_cap(const char *cap_name, const char *input_json,
                             const claw_agent_request_t *req, char **out)
{
    if (!g_engine || !g_engine->ready || !g_engine->dispatch_cap) {
        return RTK_FAIL;
    }
    return g_engine->dispatch_cap(cap_name, input_json, req,
                                  out, g_engine->dispatch_cap_ctx);
}

int claw_agent_cancel_request(uint32_t rid)
{
    bool armed = false;

    if (!g_engine || !g_engine->ready) {
        return RTK_FAIL;
    }
    if (rtos_mutex_take(g_engine->flight_lock, 200) != RTK_SUCCESS) {
        return RTK_ERR_TIMEOUT;
    }
    if (g_engine->flight_id != 0 &&
            (rid == 0 || g_engine->flight_id == rid)) {
        g_engine->abort_flag = true;
        armed = true;
        RTK_LOGI(TAG, "cancel armed req=%" PRIu32 "\n", g_engine->flight_id);
    }
    rtos_mutex_give(g_engine->flight_lock);
    return armed ? RTK_SUCCESS : RTK_FAIL;
}

int claw_agent_cancel_for_session(const char *session_id)
{
    bool armed = false;

    if (!g_engine || !g_engine->ready || !session_id || !session_id[0]) {
        return RTK_FAIL;
    }
    if (rtos_mutex_take(g_engine->flight_lock, 200) != RTK_SUCCESS) {
        return RTK_ERR_TIMEOUT;
    }
    if (g_engine->flight_id != 0 &&
            strncmp(g_engine->flight_session, session_id,
                    sizeof(g_engine->flight_session)) == 0) {
        g_engine->abort_flag = true;
        armed = true;
        RTK_LOGI(TAG, "preempt session=%s req=%" PRIu32 "\n",
                 session_id, g_engine->flight_id);
    }
    rtos_mutex_give(g_engine->flight_lock);
    return armed ? RTK_SUCCESS : RTK_FAIL;
}

int claw_agent_submit(const claw_agent_request_t *req, uint32_t timeout_ms)
{
    rtk_req_node_t n = {0};
    uint32_t ticks;

    if (!g_engine || !g_engine->running ||
            !req || !req->user_text || !req->user_text[0]) {
        return (g_engine && g_engine->running) ? RTK_ERR_BADARG : RTK_FAIL;
    }

    n.pub.request_id = req->request_id;
    n.pub.flags      = req->flags;
    n.sid    = claw_agent_str_clone(req->session_id);
    n.utext  = claw_agent_str_clone(req->user_text);
    n.src_ch = claw_agent_str_clone(req->source_channel);
    n.src_cid = claw_agent_str_clone(req->source_chat_id);
    n.src_uid = claw_agent_str_clone(req->source_sender_id);
    n.src_mid = claw_agent_str_clone(req->source_message_id);
    n.src_cap = claw_agent_str_clone(req->source_cap);

    n.pub.session_id        = n.sid;
    n.pub.user_text         = n.utext;
    n.pub.source_channel    = n.src_ch;
    n.pub.source_chat_id    = n.src_cid;
    n.pub.source_sender_id  = n.src_uid;
    n.pub.source_message_id = n.src_mid;
    n.pub.source_cap        = n.src_cap;

    if (!n.utext ||
            (req->session_id        && !n.sid)    ||
            (req->source_channel    && !n.src_ch) ||
            (req->source_chat_id    && !n.src_cid) ||
            (req->source_sender_id  && !n.src_uid) ||
            (req->source_message_id && !n.src_mid) ||
            (req->source_cap        && !n.src_cap)) {
        req_node_free(&n);
        return RTK_ERR_NOMEM;
    }

    ticks = (timeout_ms == UINT32_MAX) ? 0xFFFFFFFFUL : timeout_ms;
    if (rtos_queue_send(g_engine->req_q, &n, ticks) != RTK_SUCCESS) {
        req_node_free(&n);
        return RTK_ERR_TIMEOUT;
    }
    return RTK_SUCCESS;
}

int claw_agent_receive_for(uint32_t req_id, claw_agent_response_t *out_resp,
                           uint32_t timeout_ms)
{
    bool match_any;
    uint32_t start_ms;

    if (!out_resp) {
        return RTK_ERR_BADARG;
    }
    if (!g_engine || !g_engine->running) {
        return RTK_FAIL;
    }
    match_any = (req_id == 0);

    /* Hold recv_lock while reading from queue: serialises all concurrent
     * receive / receive_for callers and protects s_pending_head. */
    if (rtos_mutex_take(s_recv_lock, timeout_ms == UINT32_MAX ?
                        0xFFFFFFFFUL : timeout_ms) != RTK_SUCCESS) {
        return RTK_ERR_TIMEOUT;
    }

    /* Fast path: check pending list first. */
    if (recv_pop_pending(req_id, match_any, out_resp) == RTK_SUCCESS) {
        rtos_mutex_give(s_recv_lock);
        return RTK_SUCCESS;
    }

    start_ms = rtos_time_get_current_system_time_ms();
    while (true) {
        uint32_t wait;
        rtk_resp_node_t node = {0};

        if (timeout_ms == UINT32_MAX) {
            wait = 0xFFFFFFFFUL;
        } else {
            uint32_t elapsed = rtos_time_get_current_system_time_ms() - start_ms;
            if (elapsed >= timeout_ms) {
                rtos_mutex_give(s_recv_lock);
                return RTK_ERR_TIMEOUT;
            }
            wait = timeout_ms - elapsed;
        }

        if (rtos_queue_receive(g_engine->rsp_q, &node, wait) != RTK_SUCCESS) {
            rtos_mutex_give(s_recv_lock);
            return RTK_ERR_TIMEOUT;
        }

        if (match_any || node.pub.request_id == req_id) {
            *out_resp = node.pub;
            node.pub.text           = NULL;
            node.pub.error_message  = NULL;
            node.pub.source_channel = NULL;
            node.pub.source_chat_id = NULL;
            node.pub.tool_trace     = NULL;
            rtos_mutex_give(s_recv_lock);
            return RTK_SUCCESS;
        }

        /* Not our response — stash in pending for a future caller. */
        if (recv_push_pending(&node) != RTK_SUCCESS) {
            RTK_LOGW(TAG, "receive_for: OOM stashing req=%" PRIu32 " in pending\n",
                     node.pub.request_id);
            resp_node_free(&node);
        }
    }
}

int claw_agent_receive(claw_agent_response_t *out_resp, uint32_t timeout_ms)
{
    return claw_agent_receive_for(0, out_resp, timeout_ms);
}

void claw_agent_response_free(claw_agent_response_t *resp)
{
    if (!resp) {
        return;
    }
    free(resp->source_channel);
    free(resp->source_chat_id);
    free(resp->source_message_id);
    free(resp->text);
    free(resp->error_message);
    free(resp->tool_trace);
    resp->source_channel    = NULL;
    resp->source_chat_id    = NULL;
    resp->source_message_id = NULL;
    resp->text              = NULL;
    resp->tool_trace        = NULL;
    resp->error_message     = NULL;
}

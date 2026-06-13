/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ameba_soc.h"
#include "claw_agent.h"
#include "claw_agent_llm.h"
#include "claw_config.h"
#include "ameba_claw_defs.h"
#include "sys_api.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "os_wrapper.h"
#include "claw_utf8.h"

static const char *TAG = "claw_agent";

/* ---- Build-time tunables (limits defined in ameba_claw_defs.h) ---- */
#include "ameba_claw_defs.h"

#define ENGINE_STACK_DEFAULT    (16 * 1024)
#define ENGINE_PRIO_DEFAULT     3
#define ENGINE_REQ_DEPTH        4
#define ENGINE_RSP_DEPTH        4
#define TOOL_LOG_BUFSIZE        768
#define MAX_OBSERVERS           4
#define TAGBUF_SIZE             384

#ifndef SNIPPET_LIMIT
#define SNIPPET_LIMIT           96
#endif

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

/* ---- Response receive infrastructure (receive / receive_for) ---- */

/* Tunable: maximum total wall-clock time per request (tool-call chain inclusive).
 * Prevents a single request from blocking the engine indefinitely when a cap
 * hangs or the LLM returns an unexpectedly long tool-call chain. */
#define CLAW_AGENT_REQUEST_BUDGET_MS  300000u   /* 5 minutes */

/* Watchdog task constants */
#define WATCHDOG_CHECK_MS              15000u   /* check interval */
#define WATCHDOG_THRESHOLD_MS         420000u   /* 7 min silent flight → reboot */

/* Pending response node — holds responses dequeued by receive_for() that did
 * not match the caller's request_id.  Preserved for subsequent receive calls. */
typedef struct claw_pending_resp {
    rtk_resp_node_t          node;
    struct claw_pending_resp *next;
} claw_pending_resp_t;

static claw_pending_resp_t *s_pending_head = NULL;
static rtos_mutex_t         s_recv_lock;

/* ---- Engine state ---- */

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

static rtk_core_engine_t *g_engine = NULL;

/* ---- Memory helpers ---- */

static char *str_clone(const char *s)
{
    return s ? strdup(s) : NULL;
}


/* ---- Tag-buffer helpers (comma-separated, for observer reporting) ---- */

static bool tagbuf_has(const char *buf, const char *tag)
{
    size_t tlen;
    const char *p;

    if (!buf || !tag) {
        return false;
    }
    tlen = strlen(tag);
    for (p = buf; *p;) {
        if (strncmp(p, tag, tlen) == 0 &&
                (p[tlen] == ',' || p[tlen] == '\0')) {
            return true;
        }
        p = strchr(p, ',');
        if (!p) {
            break;
        }
        p++;
    }
    return false;
}

static void tagbuf_add(char *buf, size_t bufsz, const char *tag, bool nodup)
{
    size_t cur;

    if (!buf || bufsz == 0 || !tag || !tag[0]) {
        return;
    }
    if (nodup && tagbuf_has(buf, tag)) {
        return;
    }
    cur = strlen(buf);
    if (cur >= bufsz - 1) {
        return;
    }
    DiagSnPrintf(buf + cur, bufsz - cur, "%s%s", cur ? "," : "", tag);
}

/* ---- Tool execution log ---- */

static void toollog_append(char *log, size_t logsz, const char *name, bool ok)
{
    size_t used;

    if (!log || logsz == 0 || !name || !name[0]) {
        return;
    }
    used = strlen(log);
    if (used >= logsz - 1) {
        return;
    }
    DiagSnPrintf(log + used, logsz - used,
             "%s- %s: %s\n",
             used == 0 ? "[tool_calls]\n" : "",
             name, ok ? "ok" : "failed");
}

/* ---- Request node lifecycle ---- */

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

/* ---- Message-array builders ---- */

static int msgs_add_user(cJSON *arr, const char *text)
{
    cJSON *m = cJSON_CreateObject();

    if (!m) {
        return RTK_ERR_NOMEM;
    }
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", text);
    cJSON_AddItemToArray(arr, m);
    return RTK_SUCCESS;
}

static int msgs_add_json(cJSON *arr, const char *json)
{
    cJSON *p = cJSON_Parse(json);
    cJSON *it;

    if (!p || !cJSON_IsArray(p)) {
        cJSON_Delete(p);
        return RTK_FAIL;
    }
    cJSON_ArrayForEach(it, p) {
        cJSON *d = cJSON_Duplicate(it, true);
        if (!d) {
            cJSON_Delete(p);
            return RTK_ERR_NOMEM;
        }
        cJSON_AddItemToArray(arr, d);
    }
    cJSON_Delete(p);
    return RTK_SUCCESS;
}

static int msgs_add_array(cJSON *arr, const cJSON *src)
{
    const cJSON *it;

    if (!arr || !src || !cJSON_IsArray((cJSON *)src)) {
        return RTK_ERR_BADARG;
    }
    cJSON_ArrayForEach(it, src) {
        cJSON *d = cJSON_Duplicate((cJSON *)it, true);
        if (!d) {
            return RTK_ERR_NOMEM;
        }
        cJSON_AddItemToArray(arr, d);
    }
    return RTK_SUCCESS;
}

static int tools_add_json(cJSON *arr, const char *json)
{
    cJSON *p = cJSON_Parse(json);
    cJSON *it;

    if (!p || !cJSON_IsArray(p)) {
        cJSON_Delete(p);
        return RTK_FAIL;
    }
    cJSON_ArrayForEach(it, p) {
        cJSON *d = cJSON_Duplicate(it, true);
        if (!d) {
            cJSON_Delete(p);
            return RTK_ERR_NOMEM;
        }
        cJSON_AddItemToArray(arr, d);
    }
    cJSON_Delete(p);
    return RTK_SUCCESS;
}

static int prompt_append_section(char **prompt, const char *title, const char *body)
{
    char *grown;
    size_t cur, extra;

    if (!prompt || !*prompt || !title || !body || !body[0]) {
        return RTK_ERR_BADARG;
    }
    cur   = strlen(*prompt);
    extra = strlen("\n\n## \n") + strlen(title) + strlen(body);
    grown = realloc(*prompt, cur + extra + 1);
    if (!grown) {
        return RTK_ERR_NOMEM;
    }
    *prompt = grown;
    DiagSnPrintf(*prompt + cur, extra + 1, "\n\n## %s\n%s", title, body);
    return RTK_SUCCESS;
}

static char *make_turn_ctx(const claw_agent_request_t *req)
{
    static const char FMT[] =
        "=== Request Context ===\n"
        "cap: %s\n"
        "channel: %s\n"
        "chat: %s\n"
        "sender: %s\n";
    int n;
    char *t;

    n = DiagSnPrintf(NULL, 0, FMT,
                 req->source_cap      ? req->source_cap      : "(unknown)",
                 req->source_channel  ? req->source_channel  : "(unknown)",
                 req->source_chat_id  ? req->source_chat_id  : "(unknown)",
                 req->source_sender_id ? req->source_sender_id : "(unknown)");
    if (n < 0) {
        return NULL;
    }
    t = calloc(1, (size_t)n + 1);
    if (!t) {
        return NULL;
    }
    DiagSnPrintf(t, (size_t)n + 1, FMT,
             req->source_cap      ? req->source_cap      : "(unknown)",
             req->source_channel  ? req->source_channel  : "(unknown)",
             req->source_chat_id  ? req->source_chat_id  : "(unknown)",
             req->source_sender_id ? req->source_sender_id : "(unknown)");
    return t;
}

static int build_tool_call_round(cJSON *arr, const llm_resp_t *rsp)
{
    cJSON *asst = cJSON_CreateObject();
    size_t i;

    if (!asst) return RTK_ERR_NOMEM;
    cJSON_AddStringToObject(asst, "role", "assistant");

    if (claw_config_get()->llm.backend == CLAW_LLM_BACKEND_ANTHROPIC) {
        /* Anthropic: content = [{type:text,...}, {type:tool_use,...}, ...] */
        cJSON *content = cJSON_CreateArray();
        if (!content) { cJSON_Delete(asst); return RTK_ERR_NOMEM; }
        if (rsp->reply && rsp->reply[0]) {
            cJSON *txt = cJSON_CreateObject();
            if (txt) {
                cJSON_AddStringToObject(txt, "type", "text");
                cJSON_AddStringToObject(txt, "text", rsp->reply);
                cJSON_AddItemToArray(content, txt);
            }
        }
        for (i = 0; i < rsp->call_cnt; i++) {
            cJSON *tu    = cJSON_CreateObject();
            cJSON *input = cJSON_Parse(rsp->calls[i].args_json);
            if (!tu) { cJSON_Delete(input); continue; }
            cJSON_AddStringToObject(tu, "type", "tool_use");
            cJSON_AddStringToObject(tu, "id",   rsp->calls[i].call_id);
            cJSON_AddStringToObject(tu, "name", rsp->calls[i].fn_name);
            cJSON_AddItemToObject(tu, "input", input ? input : cJSON_CreateObject());
            cJSON_AddItemToArray(content, tu);
        }
        cJSON_AddItemToObject(asst, "content", content);
    } else {
        /* OpenAI: content=str, tool_calls=[{id,type,function:{name,arguments}}] */
        if (rsp->reply && rsp->reply[0])
            cJSON_AddStringToObject(asst, "content", rsp->reply);
        else
            cJSON_AddNullToObject(asst, "content");
        if (rsp->thinking && rsp->thinking[0])
            cJSON_AddStringToObject(asst, "reasoning_content", rsp->thinking);
        cJSON *tcs = cJSON_CreateArray();
        if (!tcs) { cJSON_Delete(asst); return RTK_ERR_NOMEM; }
        for (i = 0; i < rsp->call_cnt; i++) {
            cJSON *tc = cJSON_CreateObject();
            cJSON *fn = cJSON_CreateObject();
            if (!tc || !fn) { cJSON_Delete(tc); cJSON_Delete(fn); continue; }
            cJSON_AddStringToObject(tc, "id",   rsp->calls[i].call_id);
            cJSON_AddStringToObject(tc, "type", "function");
            cJSON_AddStringToObject(fn, "name", rsp->calls[i].fn_name);
            cJSON_AddStringToObject(fn, "arguments", rsp->calls[i].args_json);
            cJSON_AddItemToObject(tc, "function", fn);
            cJSON_AddItemToArray(tcs, tc);
        }
        cJSON_AddItemToObject(asst, "tool_calls", tcs);
    }
    cJSON_AddItemToArray(arr, asst);
    return RTK_SUCCESS;
}

static void tool_output_truncate(char **p_out)
{
    static const char sfx[] = TOOL_RESULT_TRUNCATION_SUFFIX;
    char *out = *p_out;
    size_t len = strlen(out);
    if (len <= TOOL_RESULT_MAX_BYTES) return;

    /* JSON-aware truncation: parse the cap output, shrink the largest string
     * field at a UTF-8 boundary, then re-serialize.  Raw byte truncation would
     * leave a dangling backslash or half a UTF-8 sequence inside a JSON string,
     * producing malformed JSON that remote APIs reject. */
    char *new_out = NULL;
    cJSON *root = cJSON_Parse(out);
    if (root && cJSON_IsObject(root)) {
        cJSON *item, *largest = NULL;
        size_t largest_len = 0;
        cJSON_ArrayForEach(item, root) {
            if (cJSON_IsString(item)) {
                size_t vlen = strlen(item->valuestring);
                if (vlen > largest_len) { largest_len = vlen; largest = item; }
            }
        }
        if (largest && largest_len > sizeof(sfx)) {
            /* Keep a proportional slice so the re-serialized JSON fits the budget */
            size_t keep = largest_len * TOOL_RESULT_MAX_BYTES / len;
            if (keep + sizeof(sfx) > largest_len) keep = largest_len - sizeof(sfx);
            /* Retreat to a valid UTF-8 character boundary */
            char *vs = largest->valuestring;
            keep = claw_utf8_safe_len(vs, keep);
            char *ns = (char *)malloc(keep + sizeof(sfx));
            if (ns) {
                if (keep) memcpy(ns, vs, keep);
                memcpy(ns + keep, sfx, sizeof(sfx));
                /* Route through cJSON_SetValuestring so the node's string is
                 * freed/allocated with cJSON's own allocator rather than libc
                 * free() — correct even if cJSON_InitHooks is ever installed,
                 * and consistent with the rest of the codebase. ns is our own
                 * libc-malloc scratch buffer; SetValuestring copies from it. */
                cJSON_SetValuestring(largest, ns);
                free(ns);
            }
        }
        new_out = cJSON_PrintUnformatted(root);
    }
    cJSON_Delete(root);

    if (!new_out) {
        /* Fallback: replace with a compact error object */
        new_out = (char *)malloc(sizeof("{\"error\":\"tool output too large\"}"));
        if (new_out)
            memcpy(new_out, "{\"error\":\"tool output too large\"}",
                   sizeof("{\"error\":\"tool output too large\"}"));
    }
    if (new_out) { free(out); *p_out = new_out; }
}

static int build_tool_result_round(cJSON *arr,
                                    const llm_resp_t *rsp,
                                    const claw_agent_request_t *req,
                                    char *tlog, size_t tlog_sz)
{
    size_t i;

    for (i = 0; i < rsp->call_cnt; i++) {
        const char *tname = rsp->calls[i].fn_name;
        const char *targs = rsp->calls[i].args_json;
        char *out = NULL;
        cJSON *tmsg;
        int err;

        RTK_LOGI(TAG, ">> tool_call  name=%-20s args=%.60s\n",
                 tname ? tname : "(null)", targs ? targs : "{}");

        if (g_engine->on_tool_progress && tname) {
            g_engine->on_tool_progress(req->request_id, tname, targs,
                                       req->source_channel, req->source_chat_id,
                                       g_engine->on_tool_progress_ctx);
        }

        /* Keep watchdog alive: each tool call resets the heartbeat so the
         * 3-minute idle-detection window starts fresh per tool, not per
         * request.  A single truly-hung cap still triggers the watchdog
         * after WATCHDOG_THRESHOLD_MS of silence. */
        g_engine->last_heartbeat_ms = rtos_time_get_current_system_time_ms();

        err = claw_agent_call_cap(tname, targs, req, &out);
        if (err != RTK_SUCCESS && !out) {
            out = str_clone(rtk_err_to_name(err));
        }
        if (!out) {
            return RTK_ERR_NOMEM;
        }
        tool_output_truncate(&out);

        RTK_LOGI(TAG, "<< tool_result name=%-20s rc=%-12s out=%.80s\n",
                 tname ? tname : "(null)", rtk_err_to_name(err), out);

        if (tlog && tlog_sz > 0 && tname) {
            /* For lua_run / skill_activate, extract the name/path from args
             * to produce a more readable log entry, e.g. "skill_activate(gpio_read)". */
            char tname_detail[64];
            if (targs) {
                const char *n = strstr(targs, "\"name\"");
                if (n) {
                    n = strchr(n, ':');
                    if (n) {
                        while (*n == ':' || *n == ' ' || *n == '"') n++;
                        size_t l = 0;
                        while (n[l] && n[l] != '"' && l < 31) l++;
                        if (l > 0) {
                            DiagSnPrintf(tname_detail, sizeof(tname_detail),
                                     "%s(%.*s)", tname, (int)l, n);
                            toollog_append(tlog, tlog_sz, tname_detail, err == RTK_SUCCESS);
                            goto tlog_done;
                        }
                    }
                }
            }
            toollog_append(tlog, tlog_sz, tname, err == RTK_SUCCESS);
            tlog_done:;
        }

        if (claw_config_get()->llm.backend == CLAW_LLM_BACKEND_ANTHROPIC) {
            /* Anthropic: ALL tool_results for one assistant turn must be batched into
             * a single role=user message.  Accumulate into the first tmsg. */
            if (i == 0) {
                tmsg = cJSON_CreateObject();
                if (!tmsg) { free(out); return RTK_ERR_NOMEM; }
                cJSON_AddStringToObject(tmsg, "role", "user");
                cJSON *carr = cJSON_CreateArray();
                if (!carr) { cJSON_Delete(tmsg); free(out); return RTK_ERR_NOMEM; }
                cJSON_AddItemToObject(tmsg, "content", carr);
                cJSON_AddItemToArray(arr, tmsg);
            } else {
                /* reuse the tmsg already in arr (last element) */
                tmsg = cJSON_GetArrayItem(arr, cJSON_GetArraySize(arr) - 1);
            }
            if (!tmsg) { free(out); return RTK_ERR_NOMEM; }
            cJSON *content_arr  = cJSON_GetObjectItem(tmsg, "content");
            if (!content_arr)   { free(out); return RTK_ERR_NOMEM; }
            cJSON *result_block = cJSON_CreateObject();
            if (!result_block)  { free(out); return RTK_ERR_NOMEM; }
            cJSON_AddStringToObject(result_block, "type",        "tool_result");
            cJSON_AddStringToObject(result_block, "tool_use_id", rsp->calls[i].call_id);
            cJSON_AddStringToObject(result_block, "content",     out);
            cJSON_AddItemToArray(content_arr, result_block);
        } else {
            /* OpenAI: separate role=tool message per result */
            tmsg = cJSON_CreateObject();
            if (!tmsg) { free(out); return RTK_ERR_NOMEM; }
            cJSON_AddStringToObject(tmsg, "role",         "tool");
            cJSON_AddStringToObject(tmsg, "tool_call_id", rsp->calls[i].call_id);
            cJSON_AddStringToObject(tmsg, "content",      out);
            cJSON_AddItemToArray(arr, tmsg);
        }
        free(out);
    }
    return RTK_SUCCESS;
}

/* ---- Context assembly ---- */

typedef struct {
    char  *sys_prompt;
    cJSON *messages;
    char  *tools_json;
} llm_ctx_t;

static void llm_ctx_free(llm_ctx_t *c)
{
    free(c->sys_prompt);
    cJSON_Delete(c->messages);
    free(c->tools_json);
    _memset(c, 0, sizeof(*c));
}

static int assemble_context(const rtk_req_node_t *rn,
                             const cJSON *rt_msgs,
                             llm_ctx_t *out,
                             char *prov_tags,
                             size_t prov_tags_sz)
{
    char  *sys   = NULL;
    cJSON *msgs  = NULL;
    cJSON *tools = NULL;
    char  *turn  = NULL;
    size_t i;
    int    rc    = RTK_SUCCESS;

    _memset(out, 0, sizeof(*out));

    sys   = str_clone(g_engine->sys_prompt);
    msgs  = cJSON_CreateArray();
    tools = cJSON_CreateArray();
    if (!sys || !msgs || !tools) {
        rc = RTK_ERR_NOMEM;
        goto done;
    }

    for (i = 0; i < g_engine->provider_cnt; i++) {
        claw_agent_context_t ctx = {0};
        const claw_agent_context_provider_t *p = &g_engine->providers[i];

        rc = p->collect(&rn->pub, &ctx, p->user_ctx);
        if (rc != RTK_SUCCESS) {
            /* RTK_ERR_BADARG signals a programming error (wrong kind/NULL) —
             * abort so it surfaces loudly during development.
             * All other failures (RTK_FAIL, RTK_ERR_NOMEM, …) are treated as
             * "provider unavailable": skip and continue with reduced context
             * rather than aborting the user's request entirely. */
            if (rc == RTK_ERR_BADARG) {
                RTK_LOGE(TAG, "provider '%s' bad arg — aborting request\n", p->name);
                goto done;
            }
            RTK_LOGW(TAG, "provider '%s' skipped (%s)\n", p->name, rtk_err_to_name(rc));
            rc = RTK_SUCCESS;
            free(ctx.content);
            ctx.content = NULL;
            continue;
        }
        if (!ctx.content || !ctx.content[0]) {
            free(ctx.content);
            continue;
        }

        /* Only log session history (kind=1) at INFO; others are debug-level noise */
        if (ctx.kind == CLAW_AGENT_CONTEXT_KIND_MESSAGES) {
            RTK_LOGI(TAG, "ctx %s len=%u\n", p->name, (unsigned)strlen(ctx.content));
        } else {
            RTK_LOGD(TAG, "ctx provider=%s kind=%d len=%u\n",
                     p->name, (int)ctx.kind, (unsigned)strlen(ctx.content));
        }
        tagbuf_add(prov_tags, prov_tags_sz, p->name, true);

        switch (ctx.kind) {
        case CLAW_AGENT_CONTEXT_KIND_SYSTEM_PROMPT:
            rc = prompt_append_section(&sys, p->name, ctx.content);
            break;
        case CLAW_AGENT_CONTEXT_KIND_MESSAGES:
            rc = msgs_add_json(msgs, ctx.content);
            break;
        case CLAW_AGENT_CONTEXT_KIND_TOOLS:
            rc = tools_add_json(tools, ctx.content);
            break;
        default:
            rc = RTK_ERR_BADARG;
            break;
        }
        free(ctx.content);
        if (rc != RTK_SUCCESS) {
            goto done;
        }
    }

    turn = make_turn_ctx(&rn->pub);
    if (!turn) {
        rc = RTK_ERR_NOMEM;
        goto done;
    }
    rc = prompt_append_section(&sys, "Core Request", turn);
    free(turn);
    turn = NULL;
    if (rc != RTK_SUCCESS) {
        goto done;
    }

    rc = msgs_add_user(msgs, rn->pub.user_text);
    if (rc != RTK_SUCCESS) {
        goto done;
    }

    if (rt_msgs && cJSON_GetArraySize((cJSON *)rt_msgs) > 0) {
        rc = msgs_add_array(msgs, rt_msgs);
        if (rc != RTK_SUCCESS) {
            goto done;
        }
    }

    out->sys_prompt = sys;
    out->messages   = msgs;
    out->tools_json = cJSON_GetArraySize(tools) > 0
                      ? cJSON_PrintUnformatted(tools) : NULL;
    if (cJSON_GetArraySize(tools) > 0 && !out->tools_json) {
        /* OOM: reclaim ownership so the done: path can free them */
        rc = RTK_ERR_NOMEM;
        sys  = out->sys_prompt;
        msgs = out->messages;
        out->sys_prompt = NULL;
        out->messages   = NULL;
    } else {
        sys  = NULL;
        msgs = NULL;
    }

done:
    free(turn);
    free(sys);
    cJSON_Delete(msgs);
    cJSON_Delete(tools);
    if (rc != RTK_SUCCESS) {
        llm_ctx_free(out);
    }
    return rc;
}


/* ---- Per-request processing (encapsulates the agentic loop) ---- */

static void process_request(rtk_req_node_t *rn, rtk_resp_node_t *resp,
                             char *prov_tags, size_t prov_tags_sz,
                             char *tool_tags, size_t tool_tags_sz)
{
    cJSON *rt_msgs;
    /* Use struct-resident tlog instead of stack local (~768B saved) */
    char     *tlog = g_engine->tlog;
    uint32_t  iter;
    uint32_t  final_prompt_tokens = 0;   /* real context size of the last request */
    int       rc;

    _memset(tlog, 0, TOOL_LOG_BUFSIZE);
    rt_msgs = cJSON_CreateArray();
    if (!rt_msgs) {
        resp->pub.error_message = str_clone("alloc rt_msgs failed");
        return;
    }

    uint32_t req_start_ms = rtos_time_get_current_system_time_ms();
    rc = RTK_SUCCESS;
    for (iter = 0; ; iter++) {
        llm_ctx_t  ctx = {0};
        llm_resp_t llm = {0};

        if (g_engine->abort_flag) {
            free(resp->pub.error_message);
            resp->pub.error_message = str_clone("request cancelled");
            resp->pub.status = CLAW_AGENT_RESPONSE_STATUS_ERROR;
            rc = RTK_FAIL;
            break;
        }

        rc = assemble_context(rn, rt_msgs, &ctx, prov_tags, prov_tags_sz);
        if (rc != RTK_SUCCESS) {
            free(resp->pub.error_message);
            resp->pub.error_message = str_clone(rtk_err_to_name(rc));
            break;
        }

        rc = claw_agent_llm_chat_messages(ctx.sys_prompt, ctx.messages,
                                         ctx.tools_json, &llm,
                                         &resp->pub.error_message);
        llm_ctx_free(&ctx);
        if (rc != RTK_SUCCESS) {
            claw_agent_llm_response_free(&llm);
            break;
        }

        /* TTFB guard (streaming mode only): in streaming the first token arrives
         * within seconds; a long TTFB means the server is unresponsive.
         * Skipped in non-streaming mode because TTFB ≈ full generation time there. */
        if (claw_config_get()->llm.stream_enabled &&
                llm.ttfb_ms > CLAW_AGENT_LLM_TTFB_TIMEOUT_MS) {
            RTK_LOGW(TAG, "req=%" PRIu32 " TTFB %u ms exceeded limit\n",
                     rn->pub.request_id, (unsigned)llm.ttfb_ms);
            free(resp->pub.error_message);
            resp->pub.error_message = str_clone("LLM response timeout (TTFB)");
            resp->pub.status = CLAW_AGENT_RESPONSE_STATUS_ERROR;
            claw_agent_llm_response_free(&llm);
            rc = RTK_FAIL;
            break;
        }

        if (llm.call_cnt == 0) {
            const char *txt = llm.reply ? llm.reply : "";

            RTK_LOGI(TAG, "== FINAL iter=%lu  reply=%.160s\n", (unsigned long)iter, txt);
            RTK_LOGI(TAG, "done req=%" PRIu32 " text_len=%zu prompt_tokens=%u\n",
                     rn->pub.request_id, strlen(txt), (unsigned)llm.prompt_tokens);
            free(resp->pub.text);
            free(resp->pub.error_message);
            resp->pub.text          = str_clone(txt);
            resp->pub.error_message = NULL;
            /* Real context size of this (final) request — drives token-budget
             * compaction. 0 if the endpoint did not report usage. */
            final_prompt_tokens     = llm.prompt_tokens;
            claw_agent_llm_response_free(&llm);
            rc = RTK_SUCCESS;
            break;
        }

        /* Record tool names for observer reporting */
        {
            size_t tc;
            for (tc = 0; tc < llm.call_cnt; tc++) {
                tagbuf_add(tool_tags, tool_tags_sz,
                           llm.calls[tc].fn_name, false);
            }
        }

        RTK_LOGI(TAG, "-- iter=%lu  LLM wants %u tool call(s)\n",
                 (unsigned long)iter, (unsigned)llm.call_cnt);
        RTK_LOGI(TAG, "tool_calls req=%" PRIu32 " count=%u iter=%u\n",
                 rn->pub.request_id, (unsigned)llm.call_cnt, iter);

        /* Forward narration text (LLM's plan/thinking before tool calls) to the
         * user in real time. tool_name=NULL distinguishes narration from tool-call
         * progress in on_tool_progress. Does not consume the tool progress budget. */
        if (llm.reply && llm.reply[0] && g_engine->on_tool_progress) {
            g_engine->on_tool_progress(rn->pub.request_id, NULL, llm.reply,
                                       rn->pub.source_channel, rn->pub.source_chat_id,
                                       g_engine->on_tool_progress_ctx);
        }

        rc = build_tool_call_round(rt_msgs, &llm);
        if (rc == RTK_SUCCESS) {
            rc = build_tool_result_round(rt_msgs, &llm, &rn->pub, tlog, TOOL_LOG_BUFSIZE);
        }
        claw_agent_llm_response_free(&llm);

        if (rc != RTK_SUCCESS) {
            free(resp->pub.error_message);
            resp->pub.error_message = str_clone(rtk_err_to_name(rc));
            break;
        }

        {
            const claw_config_t *ccfg = claw_config_get();
            uint32_t iter_limit = (ccfg->llm.max_iterations > 0)
                                  ? (uint32_t)ccfg->llm.max_iterations : CLAW_CONFIG_DEFAULT_LLM_MAX_ITER;
            if (iter_limit < CLAW_AGENT_TOOL_ITER_MIN) iter_limit = CLAW_AGENT_TOOL_ITER_MIN;
            if (iter + 1 >= iter_limit) {
                free(resp->pub.error_message);
                resp->pub.error_message = str_clone("max tool call rounds exceeded");
                rc = RTK_FAIL;
                break;
            }
        }

        /* Wall-clock budget guard: checked between iterations, so it fires after
         * the current LLM round-trip + tool calls complete.  Individual cap calls
         * enforce their own per-call timeouts; this guard aborts runaway iteration
         * chains.  Fires the same session-save path as abort_flag so partial work
         * is preserved. */
        {
            uint32_t elapsed_ms = rtos_time_get_current_system_time_ms()
                                  - req_start_ms;
            if (elapsed_ms >= CLAW_AGENT_REQUEST_BUDGET_MS) {
                free(resp->pub.error_message);
                resp->pub.error_message = str_clone("request budget exceeded");
                rc = RTK_FAIL;
                break;
            }
        }
    }

    /* Serialize this turn's tool round-trips (assistant tool_calls + tool
     * results, already in the active backend's wire format) BEFORE freeing
     * rt_msgs, so the session layer can persist them for verbatim cross-turn
     * replay. NULL when no tool call happened this turn. The final assistant
     * reply is NOT in rt_msgs — it is stored separately as the turn's
     * `assistant` text and replayed after the tool history. */
    char *tool_msgs_json = NULL;
    if (cJSON_GetArraySize(rt_msgs) > 0) {
        tool_msgs_json = cJSON_PrintUnformatted(rt_msgs);
    }
    cJSON_Delete(rt_msgs);

    if (tlog[0])
        resp->pub.tool_trace = str_clone(tlog);

    if (rc == RTK_SUCCESS && resp->pub.text) {
        resp->pub.status = CLAW_AGENT_RESPONSE_STATUS_OK;

        if (resp->pub.text[0] && g_engine->save_turn &&
                rn->pub.session_id && rn->pub.session_id[0]) {
            int ae = g_engine->save_turn(rn->pub.session_id,
                                          rn->pub.user_text,
                                          resp->pub.text,
                                          tool_msgs_json,
                                          (int)claw_config_get()->llm.backend,
                                          final_prompt_tokens,
                                          g_engine->save_turn_ctx);
            if (ae != RTK_SUCCESS) {
                RTK_LOGW(TAG, "save_turn failed: %s\n", rtk_err_to_name(ae));
            }
        }

        if (g_engine->observer_cnt > 0) {
            claw_agent_completion_summary_t s = {
                .request_id            = rn->pub.request_id,
                .session_id            = rn->pub.session_id,
                .user_text             = rn->pub.user_text,
                .final_text            = resp->pub.text,
                .context_providers_csv = prov_tags,
                .tool_calls_csv        = tool_tags,
            };
            size_t oi;
            for (oi = 0; oi < g_engine->observer_cnt; oi++) {
                g_engine->observers[oi].fn(&s, g_engine->observers[oi].ctx);
            }
        }
    } else if (rc != RTK_SUCCESS) {
        RTK_LOGE(TAG, "req=%" PRIu32 " failed: %s\n",
                 rn->pub.request_id,
                 resp->pub.error_message ?
                 resp->pub.error_message : rtk_err_to_name(rc));

        /* Save completed tool round-trips to session history for two failure modes:
         *   1. Preempted by a new message (abort_flag): next request can replay the
         *      identical tool history prefix and cache-hit.
         *   2. Request time budget exceeded: the model finished real work (tool calls
         *      with side-effects like file writes) before the deadline — dropping that
         *      context causes the next turn to lose the entire in-progress task.
         * Other failure modes (LLM error, alloc failure) still skip the save. */
        bool budget_exceeded = resp->pub.error_message &&
                               strstr(resp->pub.error_message, "budget exceeded") != NULL;
        if ((g_engine->abort_flag || budget_exceeded) && tool_msgs_json &&
                rn->pub.session_id && rn->pub.session_id[0] && g_engine->save_turn) {
            const char *interrupt_note = g_engine->abort_flag
                ? "(task was interrupted by a new message)"
                : "(request timed out; partial work above was completed)";
            g_engine->save_turn(rn->pub.session_id,
                                rn->pub.user_text,
                                interrupt_note,
                                tool_msgs_json,
                                (int)claw_config_get()->llm.backend,
                                0,
                                g_engine->save_turn_ctx);
        }

        /* Failed requests are not saved to session history — recording an error
         * note as assistant text causes the LLM to fixate on the failure in
         * subsequent turns instead of answering the new question.
         * (Preemption and budget-exceeded are handled above.) */

        if (!resp->pub.error_message) {
            resp->pub.error_message = str_clone(rtk_err_to_name(rc));
        }

        /* When the failure happened mid-iteration AFTER one or more tool calls
         * already executed, those side effects (memory_store, file writes …)
         * persisted but the user only sees ERROR. Surface the partial-success
         * tools in the error message so the next turn can reference them. */
        if (tool_tags && tool_tags[0] && resp->pub.error_message) {
            char *old = resp->pub.error_message;
            size_t need = strlen(old) + strlen(tool_tags) + 32;
            char *combined = (char *)malloc(need);
            if (combined) {
                snprintf(combined, need, "%s (partial: ran [%s])", old, tool_tags);
                free(old);
                resp->pub.error_message = combined;
            }
        }
    }

    free(tool_msgs_json);
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
        resp.pub.source_channel  = str_clone(rn.pub.source_channel);
        resp.pub.source_chat_id  = str_clone(rn.pub.source_chat_id);

        if (g_engine->on_start) {
            g_engine->on_start(&rn.pub, g_engine->on_start_ctx);
        }

        process_request(&rn, &resp,
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

    g_engine->sys_prompt = str_clone(cfg->system_prompt);
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
    slot->name = str_clone(p->name);
    if (!slot->name) {
        return RTK_ERR_NOMEM;
    }
    slot->collect  = p->collect;
    slot->user_ctx = p->user_ctx;
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
    n.sid    = str_clone(req->session_id);
    n.utext  = str_clone(req->user_text);
    n.src_ch = str_clone(req->source_channel);
    n.src_cid = str_clone(req->source_chat_id);
    n.src_uid = str_clone(req->source_sender_id);
    n.src_mid = str_clone(req->source_message_id);
    n.src_cap = str_clone(req->source_cap);

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
    free(resp->text);
    free(resp->error_message);
    free(resp->tool_trace);
    resp->source_channel = NULL;
    resp->source_chat_id = NULL;
    resp->text           = NULL;
    resp->tool_trace     = NULL;
    resp->error_message  = NULL;
}

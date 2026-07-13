/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Request materialization for the agent engine.
 *
 * Two directions of message construction live here:
 *   1. assemble_context() — fuse the context providers, the user turn and the
 *      replayed tool history into the outgoing {sys_prompt, messages, tools}.
 *   2. build_tool_call_round() / build_tool_result_round() — turn an LLM
 *      response's tool calls into wire-format messages (and run the caps),
 *      which the loop appends back into the running message array.
 *
 * Everything here is wire-format aware (OpenAI vs Anthropic) but stateless with
 * respect to the loop — the loop in claw_agent_loop.c owns the iteration.
 */

#include "ameba_soc.h"
#include "claw_agent_internal.h"
#include "claw_config.h"
#include "ameba_claw_defs.h"
#include "claw_utf8.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "claw_agent";

/* Emit a one-line serial trace for every tool call/result (dev visibility). */
#define CLAW_AGENT_ENABLE_TOOLCALL_SERIAL_LOG

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

void claw_agent_tagbuf_add(char *buf, size_t bufsz, const char *tag, bool nodup)
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

/* ---- Tool-call round (assistant message carrying the tool calls) ---- */

int claw_agent_build_tool_call_round(cJSON *arr, const llm_resp_t *rsp)
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

int claw_agent_build_tool_result_round(cJSON *arr,
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

#ifdef CLAW_AGENT_ENABLE_TOOLCALL_SERIAL_LOG
        printf(">> tool_call  name=%s args=%.60s\n", tname ? tname : "(null)", targs ? targs : "{}");
#endif
        if (g_engine->on_tool_progress && tname) {
            g_engine->on_tool_progress(req->request_id, tname, targs,
                                       req->source_channel, req->source_chat_id,
                                       req->source_message_id,
                                       g_engine->on_tool_progress_ctx);
        }

        /* Keep watchdog alive: each tool call resets the heartbeat so the
         * 3-minute idle-detection window starts fresh per tool, not per
         * request.  A single truly-hung cap still triggers the watchdog
         * after WATCHDOG_THRESHOLD_MS of silence. */
        g_engine->last_heartbeat_ms = rtos_time_get_current_system_time_ms();

        err = claw_agent_call_cap(tname, targs, req, &out);
        if (err != RTK_SUCCESS && !out) {
            out = claw_agent_str_clone(rtk_err_to_name(err));
        }
        if (!out) {
            return RTK_ERR_NOMEM;
        }
        tool_output_truncate(&out);
        /* Repair any invalid UTF-8 a cap may have emitted (e.g. a Lua error
         * whose chunkid was byte-truncated mid-character) BEFORE it enters the
         * messages array.  This same `out` is both sent in this turn's request
         * and serialized into the persisted tool_msgs, so one invalid byte here
         * would otherwise 400 the request now AND every replay of this session
         * thereafter.  Sanitize at this single chokepoint to cover all caps. */
        claw_utf8_sanitize_inplace(out);
#ifdef CLAW_AGENT_ENABLE_TOOLCALL_SERIAL_LOG
        printf("<< tool_result name=%s rc=%s out=%.80s\n", tname ? tname : "(null)", rtk_err_to_name(err), out);
#endif
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

void claw_agent_llm_ctx_free(llm_ctx_t *c)
{
    free(c->sys_prompt);
    cJSON_Delete(c->messages);
    free(c->tools_json);
    _memset(c, 0, sizeof(*c));
}

int claw_agent_assemble_context(const rtk_req_node_t *rn,
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

    sys   = claw_agent_str_clone(g_engine->sys_prompt);
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
            if (!p->quiet_skip)
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

        RTK_LOGD(TAG, "ctx provider=%s kind=%d len=%u\n",
                 p->name, (int)ctx.kind, (unsigned)strlen(ctx.content));
        claw_agent_tagbuf_add(prov_tags, prov_tags_sz, p->name, true);

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
        claw_agent_llm_ctx_free(out);
    }
    return rc;
}

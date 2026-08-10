/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "claw_event_dispatcher.h"
#include "claw_event_publisher.h"
#include "claw_agent.h"
#include "claw_cap.h"
#include "claw_im_dispatch.h"
#include "ameba_claw_defs.h"
#include "os_wrapper.h"
#include "session_cmd.h"
#include <cJSON.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Use rtos_mem allocator for all string copies so free() pairs correctly. */
static inline char *claw_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)rtos_mem_malloc(n);
    if (p) _memcpy(p, s, n);
    return p;
}

static const char *TAG = "evr";

/* Max rules that can match a single event in one pass */
#define MAX_MATCH_PER_EVENT   8

/* ---- Runtime state ---- */

typedef struct {
    claw_event_dispatcher_config_t cfg;
    rtos_mutex_t lock;
    rtos_queue_t inbox;
    rtos_task_t       worker;
    claw_event_dispatcher_rule_t *rules;
    size_t nrules;
    bool ready;
    bool active;
} ev_router_t;

static ev_router_t s_rt;
static volatile uint32_t s_call_seq;

/* ================================================================
 * Template context: build a cJSON tree from the event, then render
 * @{dotted.path} placeholders by looking them up in the tree.
 *
 * Rendering happens at the cJSON *node* level (each string value is
 * rendered independently), so substituted values that contain quotes /
 * braces / newlines can never corrupt the surrounding JSON — the tree is
 * re-serialized by cJSON afterwards with correct escaping.
 * ================================================================ */

/* Append a byte range to a growable rtos_mem buffer. Returns false on OOM. */
static bool sb_append(char **buf, size_t *cap, size_t *len, const char *s, size_t slen)
{
    if (*len + slen + 1 > *cap) {
        size_t nc = (*cap * 2 > *len + slen + 64) ? *cap * 2 : *len + slen + 64;
        char *t = (char *)rtos_mem_malloc(nc);
        if (!t) return false;
        _memcpy(t, *buf, *len);
        rtos_mem_free(*buf);
        *buf = t;
        *cap = nc;
    }
    _memcpy(*buf + *len, s, slen);
    *len += slen;
    (*buf)[*len] = '\0';
    return true;
}

/* Convert a cJSON value to a freshly allocated string (rtos_mem). */
static char *json_value_to_str(const cJSON *v)
{
    if (!v) return claw_strdup("");
    if (cJSON_IsString(v)) return claw_strdup(v->valuestring ? v->valuestring : "");
    if (cJSON_IsBool(v))   return claw_strdup(cJSON_IsTrue(v) ? "true" : "false");
    if (cJSON_IsNull(v))   return claw_strdup("");
    if (cJSON_IsNumber(v)) {
        char b[32];
        double d = v->valuedouble;
        /* Print integers without a decimal point; leave fractional numbers
         * to cJSON's own formatter (avoids relying on %f in DiagSnPrintf). */
        if (d == (double)(long long)d && d < 2147483647.0 && d > -2147483647.0) {
            DiagSnPrintf(b, sizeof(b), "%d", (int)d);
            return claw_strdup(b);
        }
        char *s = cJSON_PrintUnformatted(v);
        if (!s) return claw_strdup("");
        char *out = claw_strdup(s);
        cJSON_free(s);
        return out;
    }
    /* object / array → serialized */
    char *s = cJSON_PrintUnformatted(v);
    if (!s) return claw_strdup("");
    char *out = claw_strdup(s);
    cJSON_free(s);
    return out;
}

/* Walk a dotted path ("event.payload.temp") through the ctx tree. */
static cJSON *ctx_lookup(const cJSON *ctx, const char *path)
{
    const cJSON *cur = ctx;
    const char *p = path;
    char seg[64];
    while (*p && cur) {
        size_t n = 0;
        while (*p && *p != '.' && n < sizeof(seg) - 1) seg[n++] = *p++;
        seg[n] = '\0';
        if (*p == '.') p++;
        cur = cJSON_GetObjectItemCaseSensitive(cur, seg);
    }
    return (cJSON *)cur;
}

/* Render @{a.b.c} placeholders in a plain string. Unmatched placeholders and
 * lookups that miss render to empty (so no stray "@{...}" leaks through). */
static char *render_str(const char *tpl, const cJSON *ctx)
{
    if (!tpl) return NULL;
    size_t cap = strlen(tpl) + 64, len = 0;
    char *out = (char *)rtos_mem_malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';

    const char *p = tpl;
    while (*p) {
        if (p[0] == '@' && p[1] == '{') {
            const char *end = strchr(p + 2, '}');
            size_t plen = end ? (size_t)(end - (p + 2)) : 0;
            if (end && plen < 96) {
                char path[96];
                _memcpy(path, p + 2, plen);
                path[plen] = '\0';
                cJSON *v = ctx_lookup(ctx, path);
                char *val = json_value_to_str(v);
                if (val) {
                    if (!sb_append(&out, &cap, &len, val, strlen(val))) {
                        rtos_mem_free(val);
                        return out;
                    }
                    rtos_mem_free(val);
                }
                p = end + 1;
                continue;
            }
        }
        if (!sb_append(&out, &cap, &len, p, 1)) return out;
        p++;
    }
    return out;
}

/* Recursively render every string value of a cJSON tree in place. */
static void render_json_inplace(cJSON *node, const cJSON *ctx)
{
    if (!node) return;
    if (cJSON_IsObject(node) || cJSON_IsArray(node)) {
        for (cJSON *c = node->child; c; c = c->next) render_json_inplace(c, ctx);
    } else if (cJSON_IsString(node) && node->valuestring) {
        char *r = render_str(node->valuestring, ctx);
        if (r) {
            cJSON_SetValuestring(node, r);
            rtos_mem_free(r);
        }
    }
}

/* Render an action's input template. For CAP/EMIT the template is a JSON
 * object rendered at node level (escaping-safe); if it does not parse as
 * JSON we fall back to plain string substitution. Caller frees. */
static char *render_input_json(const char *tpl, const cJSON *ctx)
{
    if (!tpl) return NULL;
    cJSON *j = cJSON_Parse(tpl);
    if (j) {
        render_json_inplace(j, ctx);
        char *s = cJSON_PrintUnformatted(j);
        cJSON_Delete(j);
        if (s) {
            char *out = claw_strdup(s);
            cJSON_free(s);
            return out;
        }
        return NULL;
    }
    /* Not valid JSON — best-effort plain substitution. */
    return render_str(tpl, ctx);
}

/* Apply an rtk_emit action's rendered input onto the derived (cloned) event.
 * The input is normally a JSON object that can BOTH (a) override the derived
 * event's routing fields — event_type / text / channel / chat_id — so the
 * derived event can be matched by a DIFFERENT rule (real A→B chaining, not
 * just a self-loop), and (b) supply the new payload. Payload rule: an explicit
 * "payload" child becomes payload_json; otherwise the whole object is the
 * payload (back-compat with emit rules that just carry a flat payload). A non-
 * object / unparseable input keeps the old contract: the whole string is the
 * payload. Match keys not set here are inherited from the parent via the clone.
 * Takes ownership of `rendered` (frees it or transfers it into the event). */
static void emit_apply_input(claw_event_t *derived, char *rendered)
{
    if (!rendered) return;

    cJSON *obj = cJSON_Parse(rendered);
    if (!cJSON_IsObject(obj)) {              /* array / scalar / parse-fail → payload */
        rtos_mem_free(derived->payload_json);
        derived->payload_json = rendered;    /* transfer ownership */
        if (obj) cJSON_Delete(obj);
        return;
    }

    /* (a) routing overrides (string values only) */
    const cJSON *v;
    v = cJSON_GetObjectItemCaseSensitive(obj, "event_type");
    if (cJSON_IsString(v) && v->valuestring)
        strlcpy(derived->event_type, v->valuestring, sizeof(derived->event_type));
    v = cJSON_GetObjectItemCaseSensitive(obj, "channel");
    if (cJSON_IsString(v) && v->valuestring)
        strlcpy(derived->source_channel, v->valuestring, sizeof(derived->source_channel));
    v = cJSON_GetObjectItemCaseSensitive(obj, "chat_id");
    if (cJSON_IsString(v) && v->valuestring)
        strlcpy(derived->chat_id, v->valuestring, sizeof(derived->chat_id));
    v = cJSON_GetObjectItemCaseSensitive(obj, "text");   /* heap field on the event */
    if (cJSON_IsString(v) && v->valuestring) {
        char *nt = claw_strdup(v->valuestring);
        if (nt) { rtos_mem_free(derived->text); derived->text = nt; }
    }

    /* (b) payload: explicit "payload" child wins, else the whole object */
    const cJSON *pl = cJSON_GetObjectItemCaseSensitive(obj, "payload");
    rtos_mem_free(derived->payload_json);
    if (pl) {
        char *s = cJSON_PrintUnformatted(pl);
        derived->payload_json = claw_strdup(s ? s : "");
        if (s) cJSON_free(s);
        rtos_mem_free(rendered);             /* not reused */
    } else {
        derived->payload_json = rendered;    /* transfer ownership */
    }
    cJSON_Delete(obj);
}

/* ---- Build the per-rule render context ---- */

static void add_str(cJSON *obj, const char *k, const char *v)
{
    cJSON_AddStringToObject(obj, k, v ? v : "");
}

static cJSON *build_ctx(const claw_event_t *ev,
                        const claw_event_dispatcher_rule_t *rule,
                        const char *remainder)
{
    cJSON *ctx = cJSON_CreateObject();
    if (!ctx) return NULL;

    /* event.* — full field names */
    cJSON *e = cJSON_CreateObject();
    if (e) {
        add_str(e, "event_id",       ev->event_id);
        add_str(e, "event_type",     ev->event_type);
        add_str(e, "source_cap",     ev->source_cap);
        add_str(e, "source_channel", ev->source_channel);
        add_str(e, "target_channel", ev->target_channel);
        add_str(e, "chat_id",        ev->chat_id);
        add_str(e, "sender_id",      ev->sender_id);
        add_str(e, "message_id",     ev->message_id);
        add_str(e, "correlation_id", ev->correlation_id);
        add_str(e, "content_type",   ev->content_type);
        add_str(e, "text",           ev->text);
        /* payload parsed only when present (lazy w.r.t. empty payloads) */
        if (ev->payload_json && ev->payload_json[0]) {
            cJSON *pl = cJSON_Parse(ev->payload_json);
            if (pl) cJSON_AddItemToObject(e, "payload", pl);
        }
        cJSON_AddItemToObject(ctx, "event", e);
    }

    /* ev.* — legacy abbreviations (kept as exact aliases, field names differ) */
    cJSON *a = cJSON_CreateObject();
    if (a) {
        add_str(a, "text",    ev->text);
        add_str(a, "chat",    ev->chat_id);
        add_str(a, "sender",  ev->sender_id);
        add_str(a, "channel", ev->source_channel);
        add_str(a, "type",    ev->event_type);
        add_str(a, "cap",     ev->source_cap);
        cJSON_AddItemToObject(ctx, "ev", a);
    }

    /* vars.* — rule-level variables */
    if (rule && rule->vars_json && rule->vars_json[0]) {
        cJSON *v = cJSON_Parse(rule->vars_json);
        if (v) cJSON_AddItemToObject(ctx, "vars", v);
    }

    /* match.* — text-match info (remainder set for PREFIX) */
    cJSON *m = cJSON_CreateObject();
    if (m) {
        const char *mrule = "";
        if (rule) {
            mrule = rule->match.text_match_rule == CLAW_DISPATCHER_TEXT_MATCH_PREFIX ? "prefix" :
                    rule->match.text_match_rule == CLAW_DISPATCHER_TEXT_MATCH_EXACT  ? "exact"  : "";
        }
        add_str(m, "text",      rule ? rule->match.text : "");
        add_str(m, "rule",      mrule);
        add_str(m, "remainder", remainder);
        cJSON_AddItemToObject(ctx, "match", m);
    }

    /* rule.id */
    cJSON *r = cJSON_CreateObject();
    if (r) {
        add_str(r, "id", rule ? rule->id : "");
        cJSON_AddItemToObject(ctx, "rule", r);
    }

    /* last.* — updated after each capture_output action */
    cJSON *last = cJSON_CreateObject();
    if (last) cJSON_AddItemToObject(ctx, "last", last);

    return ctx;
}

/* Record an action's output into ctx.last for @{last.output} chaining. */
static void ctx_set_last(cJSON *ctx, const char *kind, const char *target,
                         int status, const char *output)
{
    cJSON *last = cJSON_GetObjectItem(ctx, "last");
    if (!last) return;
    cJSON_DeleteItemFromObject(last, "kind");
    cJSON_DeleteItemFromObject(last, "target");
    cJSON_DeleteItemFromObject(last, "status");
    cJSON_DeleteItemFromObject(last, "output");
    add_str(last, "kind",   kind);
    add_str(last, "target", target);
    cJSON_AddNumberToObject(last, "status", status);
    if (output && output[0]) {
        size_t ol = strlen(output);
        /* Structured output → store parsed so a later action can pull a single
         * field (@{last.output.answer}), not just splice the whole blob. Only
         * for JSON object/array within the parse cap; @{last.output} whole
         * still renders because json_value_to_str serializes objects. */
        cJSON *parsed = (ol <= CLAW_DISPATCHER_LAST_OUTPUT_PARSE_MAX)
                            ? cJSON_Parse(output) : NULL;
        if (parsed && (cJSON_IsObject(parsed) || cJSON_IsArray(parsed))) {
            cJSON_AddItemToObject(last, "output", parsed);
        } else {
            if (parsed) cJSON_Delete(parsed);
            /* Non-JSON / scalar / oversized → bounded string prefix. */
            if (ol > CLAW_DISPATCHER_LAST_OUTPUT_MAX) {
                char *t = (char *)rtos_mem_malloc(CLAW_DISPATCHER_LAST_OUTPUT_MAX + 1);
                if (t) {
                    _memcpy(t, output, CLAW_DISPATCHER_LAST_OUTPUT_MAX);
                    t[CLAW_DISPATCHER_LAST_OUTPUT_MAX] = '\0';
                    add_str(last, "output", t);
                    rtos_mem_free(t);
                } else {
                    add_str(last, "output", "");
                }
            } else {
                add_str(last, "output", output);
            }
        }
    } else {
        add_str(last, "output", "");
    }
}

/* ---- only_if guard evaluation (single comparison) ---- */

static bool guard_passes(const claw_event_dispatcher_guard_t *g, const cJSON *ctx)
{
    if (g->op == CLAW_DISPATCHER_OP_NONE) return true;

    char *lv = render_str(g->left, ctx);
    const char *l = lv ? lv : "";
    const char *r = g->right;
    bool res = false;

    if (g->op == CLAW_DISPATCHER_OP_EXISTS) {
        res = (l[0] != '\0');
    } else if (g->op == CLAW_DISPATCHER_OP_CONTAINS) {
        res = (strstr(l, r) != NULL);
    } else {
        char *le = NULL, *re = NULL;
        double ld = strtod(l, &le);
        double rd = strtod(r, &re);
        bool numeric = (le != l && *le == '\0' && re != r && *re == '\0');
        int cmp;
        if (numeric) {
            cmp = (ld < rd) ? -1 : (ld > rd) ? 1 : 0;
        } else {
            cmp = strcmp(l, r);
        }
        switch (g->op) {
        case CLAW_DISPATCHER_OP_EQ: res = (cmp == 0); break;
        case CLAW_DISPATCHER_OP_NE: res = (cmp != 0); break;
        case CLAW_DISPATCHER_OP_GT: res = (cmp > 0);  break;
        case CLAW_DISPATCHER_OP_LT: res = (cmp < 0);  break;
        case CLAW_DISPATCHER_OP_GE: res = (cmp >= 0); break;
        case CLAW_DISPATCHER_OP_LE: res = (cmp <= 0); break;
        default: res = false; break;
        }
    }
    rtos_mem_free(lv);
    return res;
}

/* ---- Rule filtering ---- */

static bool filter_matches(const claw_event_dispatcher_rule_t *rule, const claw_event_t *ev,
                           char *remainder_out, size_t rem_sz)
{
    const claw_event_dispatcher_match_t *m = &rule->match;
    bool ok = rule->enabled;

    if (remainder_out && rem_sz) remainder_out[0] = '\0';

    /* Accumulated-boolean: each non-empty criterion must be satisfied */
    ok &= (m->event_type[0] == '\0' || strcmp(m->event_type, ev->event_type)     == 0);
    ok &= (m->source_cap[0] == '\0' || strcmp(m->source_cap, ev->source_cap)     == 0);
    ok &= (m->channel[0]    == '\0' || strcmp(m->channel,    ev->source_channel) == 0);
    ok &= (m->chat_id[0]    == '\0' || strcmp(m->chat_id,    ev->chat_id)        == 0);
    ok &= (m->event_key[0]  == '\0' || strcmp(m->event_key,  ev->correlation_id) == 0);

    if (ok && m->text_contains[0]) {
        ok = ev->text != NULL && strstr(ev->text, m->text_contains) != NULL;
    }

    if (ok && m->text_match_rule != CLAW_DISPATCHER_TEXT_MATCH_NONE) {
        const char *t = ev->text ? ev->text : "";
        if (m->text_match_rule == CLAW_DISPATCHER_TEXT_MATCH_EXACT) {
            ok = (strcmp(t, m->text) == 0);
        } else { /* PREFIX */
            size_t pl = strlen(m->text);
            if (strncmp(t, m->text, pl) == 0) {
                const char *rem = t + pl;
                while (*rem == ' ') rem++;   /* trim leading spaces before the remainder */
                if (remainder_out && rem_sz) strlcpy(remainder_out, rem, rem_sz);
                ok = true;
            } else {
                ok = false;
            }
        }
    }
    return ok;
}

/* ---- One-shot task for async SEND_MESSAGE delivery ---- */
#define SEND_IM_TEXT_MAX  2048
#define MAX_SEND_IM_TASKS 3
static volatile int s_send_im_inflight = 0;
typedef struct { char channel[32]; char chat_id[96]; char text[SEND_IM_TEXT_MAX]; } send_im_arg_t;
static void send_im_task(void *p)
{
    send_im_arg_t *a = (send_im_arg_t *)p;
    claw_im_dispatch_send(a->channel, a->chat_id, a->text);
    s_send_im_inflight--;  /* decrement before free so dispatcher sees freed slot */
    rtos_mem_free(a);
    rtos_task_delete(NULL);
}

/* ---- Single action dispatch (executed lock-free) ---- */

static int dispatch_one_action(const claw_event_dispatcher_action_t *act,
                               const claw_event_t *ev,
                               cJSON *ctx,
                               int depth,
                               const char *rule_id)
{
    switch (act->kind) {

    case CLAW_DISPATCHER_ACT_AGENT: {
        /* NOTE: the instant ack is sent by dispatch_rule() BEFORE actions run,
         * so no ack is sent here. */
        char sid[140] = {0};
        if (s_rt.cfg.session_builder) {
            s_rt.cfg.session_builder(ev, sid, sizeof(sid), s_rt.cfg.session_builder_ctx);
        } else {
            claw_event_build_session_id(ev, sid, sizeof(sid));
        }

        claw_agent_request_t req;
        _memset(&req, 0, sizeof(req));
        req.request_id        = ++s_call_seq;
        req.session_id        = sid;
        req.user_text         = ev->text;
        req.source_channel    = ev->source_channel;
        req.source_chat_id    = ev->chat_id;
        req.source_sender_id  = ev->sender_id;
        req.source_message_id = ev->message_id[0] ? ev->message_id : NULL;
        req.source_cap        = ev->source_cap[0]  ? ev->source_cap  : NULL;

        /* Cancel any in-flight request on this session so the new message is
         * processed immediately. */
        claw_agent_cancel_for_session(sid);

        if (claw_agent_submit(&req, s_rt.cfg.core_submit_timeout_ms) != RTK_SUCCESS) {
            RTK_LOGE(TAG, "RUN_AGENT submit failed (queue full) for %s\n", sid);
            if (req.source_channel && req.source_chat_id) {
                claw_im_dispatch_send(req.source_channel, req.source_chat_id,
                                      "I'm too busy right now, please try again.");
            }
            return RTK_FAIL;
        }
        return RTK_SUCCESS;
    }

    case CLAW_DISPATCHER_ACT_CAP: {
        char *input  = render_input_json(act->input_json, ctx);
        char *output = NULL;

        claw_cap_call_context_t cctx;
        _memset(&cctx, 0, sizeof(cctx));
        cctx.request_id = ++s_call_seq;
        cctx.channel    = ev->source_channel;
        cctx.chat_id    = ev->chat_id;
        cctx.source_cap = ev->source_cap[0] ? ev->source_cap : NULL;
        cctx.caller     = CLAW_CAP_CALLER_INTERNAL;

        int rc = claw_cap_call(act->cap, input ? input : "{}", &cctx, &output);
        if (rc != RTK_SUCCESS) {
            RTK_LOGE(TAG, "CALL_CAP '%s' failed (%d)\n", act->cap, rc);
        }
        if (act->capture_output) {
            ctx_set_last(ctx, "cap", act->cap, rc, output);
        }
        rtos_mem_free(input);
        rtos_mem_free(output);
        return rc;
    }

    case CLAW_DISPATCHER_ACT_SEND: {
        /* Resolve target channel: explicit cap field, then ev->target_channel,
         * then source. */
        const char *ch = act->cap[0]          ? act->cap :
                         ev->target_channel[0] ? ev->target_channel :
                                                  ev->source_channel;
        if (!ch[0]) {
            RTK_LOGW(TAG, "SEND_MESSAGE: no channel\n");
            return RTK_FAIL;
        }
        char *text = render_str(act->input_json, ctx);
        if (!text) {
            RTK_LOGW(TAG, "SEND_MESSAGE: empty template, dropped\n");
            return RTK_FAIL;
        }
        if (act->capture_output) {
            ctx_set_last(ctx, "send", ch, 0, text);
        }
        if (s_send_im_inflight >= MAX_SEND_IM_TASKS) {
            RTK_LOGW(TAG, "SEND_MESSAGE: max concurrent tasks reached, dropped\n");
            rtos_mem_free(text);
            return RTK_FAIL;
        }
        send_im_arg_t *sia = (send_im_arg_t *)rtos_mem_malloc(sizeof(send_im_arg_t));
        if (!sia) {
            RTK_LOGW(TAG, "SEND_MESSAGE: alloc failed, message dropped\n");
            rtos_mem_free(text);
            return RTK_FAIL;
        }
        strlcpy(sia->channel, ch, sizeof(sia->channel));
        strlcpy(sia->chat_id, ev->chat_id, sizeof(sia->chat_id));
        if (strlen(text) >= SEND_IM_TEXT_MAX) {
            RTK_LOGW(TAG, "SEND_MESSAGE: text truncated (%u->%u bytes)\n",
                     (unsigned)strlen(text), SEND_IM_TEXT_MAX - 1);
        }
        strlcpy(sia->text, text, sizeof(sia->text));
        rtos_mem_free(text);
        s_send_im_inflight++;
        if (rtos_task_create(NULL, "im_send", send_im_task, sia,
                             8192, 1) != RTK_SUCCESS) {
            s_send_im_inflight--;
            rtos_mem_free(sia);
            RTK_LOGW(TAG, "SEND_MESSAGE: task create failed, message dropped\n");
            return RTK_FAIL;
        }
        return RTK_SUCCESS;
    }

    case CLAW_DISPATCHER_ACT_EMIT: {
        /* `depth` is the CURRENT event's emit_depth (threaded from handle_event
         * via ev->emit_depth). Refuse once the chain has already reached the
         * cap — otherwise a derived event that re-matches (it keeps the parent
         * text) would loop forever, since each re-dispatch is async and would
         * otherwise restart the counter. */
        if (depth >= CLAW_DISPATCHER_MAX_EMIT_DEPTH) {
            RTK_LOGW(TAG, "EMIT_EVENT: depth limit %d reached, dropping\n",
                     CLAW_DISPATCHER_MAX_EMIT_DEPTH);
            return RTK_FAIL;
        }
        claw_event_t *derived = rtos_mem_malloc(sizeof(claw_event_t));
        if (!derived) return RTK_FAIL;
        if (claw_event_clone(ev, derived) != RTK_SUCCESS) {
            rtos_mem_free(derived);
            return RTK_FAIL;
        }
        derived->emit_depth = (uint8_t)(depth + 1);  /* next generation */
        if (act->input_json) {
            /* Rendered input may override routing fields (so the derived event
             * targets a different rule) and/or set the payload. */
            emit_apply_input(derived, render_input_json(act->input_json, ctx));
        }
        if (rtos_queue_send(s_rt.inbox, &derived, 100) != RTK_SUCCESS) {
            RTK_LOGE(TAG, "EMIT_EVENT: queue full\n");
            claw_event_free(derived);
            rtos_mem_free(derived);
            return RTK_FAIL;
        }
        return RTK_SUCCESS;
    }

    case CLAW_DISPATCHER_ACT_DROP:
        return RTK_SUCCESS;

    case CLAW_DISPATCHER_ACT_SCRIPT: {
        if (!act->script[0]) {
            RTK_LOGW(TAG, "RUN_SCRIPT: no script path\n");
            return RTK_FAIL;
        }

        /* Reach lua ONLY through the cap registry (claw_cap_call), exactly
         * like ACT_CAP — the dispatcher (core layer) stays decoupled from the
         * cap_lua capability. The lua cap owns job-budget/concurrency policy;
         * we just launch and, for sync, capture the result. */
        cJSON *in = cJSON_CreateObject();
        if (!in) return RTK_FAIL;
        cJSON_AddStringToObject(in, "path", act->script);

        /* Rendered args template → attach as a parsed object/array when it is
         * one (so the script receives structured args), else as a string.
         * Mirrors how input_json is treated for CAP/EMIT. */
        char *args = render_input_json(act->input_json, ctx);
        if (args) {
            cJSON *pa = cJSON_Parse(args);
            if (pa && (cJSON_IsObject(pa) || cJSON_IsArray(pa))) {
                cJSON_AddItemToObject(in, "args", pa);
            } else {
                if (pa) cJSON_Delete(pa);
                cJSON_AddStringToObject(in, "args", args);
            }
            rtos_mem_free(args);
        }

        const char *lua_cap;
        if (act->script_sync) {
            /* Blocks this dispatcher task until the script returns; its result
             * is then available to a later action via @{last.output}. */
            lua_cap = "lua_run";
        } else {
            /* Fire-and-forget background job. Per-rule dedup: a given rule
             * cannot stack overlapping runs of its own script (noisy-sensor
             * storm). replace=false → a re-fire while the previous run is still
             * active is dropped, not interrupted. Aggregate concurrency is
             * bounded by the lua cap's own LUA_JOB_MAX_RUNNING budget. */
            lua_cap = "lua_run_async";
            if (rule_id && rule_id[0]) {
                cJSON_AddStringToObject(in, "name", rule_id);
                cJSON_AddStringToObject(in, "exclusive", rule_id);
            }
            cJSON_AddBoolToObject(in, "replace", false);
        }

        char *input = cJSON_PrintUnformatted(in);
        cJSON_Delete(in);
        if (!input) return RTK_FAIL;

        claw_cap_call_context_t cctx;
        _memset(&cctx, 0, sizeof(cctx));
        cctx.request_id = ++s_call_seq;
        cctx.channel    = ev->source_channel;  /* origin for script event.notify() */
        cctx.chat_id    = ev->chat_id;
        cctx.source_cap = ev->source_cap[0] ? ev->source_cap : NULL;
        cctx.caller     = CLAW_CAP_CALLER_INTERNAL;

        char *output = NULL;
        int rc = claw_cap_call(lua_cap, input, &cctx, &output);
        if (rc != RTK_SUCCESS) {
            RTK_LOGW(TAG, "RUN_SCRIPT '%s' via %s failed (%d)\n",
                     act->script, lua_cap, rc);
        }
        /* Note: async capture yields the job-start JSON, not the script's
         * result — result chaining needs mode=sync. */
        if (act->capture_output) {
            ctx_set_last(ctx, "script", act->script, rc, output);
        }
        cJSON_free(input);
        rtos_mem_free(output);
        return rc;
    }

    default:
        RTK_LOGW(TAG, "Unknown action kind %d\n", (int)act->kind);
        return RTK_FAIL;
    }
}

/* Send the rule's instant acknowledgement (before its actions run).
 * - explicit ack template → render and send (any rule kind)
 * - no ack template but rule has an AGENT action → default CLAW_IM_ACK_MSG
 * - drop / send-only rules with no ack template → no ack (no phantom "收到") */
static void send_rule_ack(const claw_event_dispatcher_rule_t *rule,
                          const claw_event_t *ev, cJSON *ctx)
{
    if (!ev->source_channel[0] || !ev->chat_id[0]) return;
    if (claw_im_dispatch_channel_has_flag(ev->source_channel,
                                          CLAW_IM_CHANNEL_FLAG_NO_ACK)) return;

    char *rendered = NULL;
    const char *text = NULL;

    if (rule->ack[0]) {
        rendered = render_str(rule->ack, ctx);
        text = rendered;
    } else {
        bool has_agent = false;
        for (size_t i = 0; i < rule->action_count; i++) {
            if (rule->actions[i].kind == CLAW_DISPATCHER_ACT_AGENT) { has_agent = true; break; }
        }
        if (has_agent) text = CLAW_IM_ACK_MSG;
    }

    if (text && text[0]) {
        claw_im_dispatch_send(ev->source_channel, ev->chat_id, text);
    }
    rtos_mem_free(rendered);
}

/* Execute one matched rule: ack (before actions) → guarded actions → chain. */
static void dispatch_rule(const claw_event_dispatcher_rule_t *rule,
                          const claw_event_t *ev, const char *remainder, int depth)
{
    cJSON *ctx = build_ctx(ev, rule, remainder);
    if (!ctx) return;

    send_rule_ack(rule, ev, ctx);

    for (size_t i = 0; i < rule->action_count; i++) {
        const claw_event_dispatcher_action_t *a = &rule->actions[i];
        if (!guard_passes(&a->only_if, ctx)) continue;
        int rc = dispatch_one_action(a, ev, ctx, depth, rule->id);
        if (rc != RTK_SUCCESS && a->on_error == CLAW_DISPATCHER_ON_ERROR_STOP) {
            RTK_LOGW(TAG, "rule '%s' action %u failed, stopping chain\n",
                     rule->id, (unsigned)i);
            break;
        }
    }
    cJSON_Delete(ctx);
}

/* ---- Rule deep-copy helpers (heap; libc calloc for actions array, rtos_mem
 * for input_json / vars_json to match the existing free paths) ---- */

static void free_rule_contents(claw_event_dispatcher_rule_t *r)
{
    if (!r) return;
    if (r->actions) {
        for (size_t i = 0; i < r->action_count; i++) {
            rtos_mem_free(r->actions[i].input_json);
            r->actions[i].input_json = NULL;
        }
        free(r->actions);
        r->actions = NULL;
    }
    r->action_count = 0;
    rtos_mem_free(r->vars_json);
    r->vars_json = NULL;
}

/* Copy `src` into `dst` (dst must be zero/uninitialised — no old contents freed).
 * Deep-copies actions and vars_json. Runtime stats (fire_count/last_fired_ts)
 * are carried over via the memcpy: a freshly-parsed rule brings 0 (so add/update
 * start clean), while get_rule/list_rules preserve the live counters. */
static int copy_rule(claw_event_dispatcher_rule_t *dst,
                     const claw_event_dispatcher_rule_t *src, size_t action_cap)
{
    _memcpy(dst, src, sizeof(*dst));
    dst->actions      = NULL;
    dst->action_count = 0;
    dst->vars_json    = NULL;

    if (src->vars_json) {
        dst->vars_json = claw_strdup(src->vars_json);
        if (!dst->vars_json) return RTK_ERR_NOMEM;
    }

    if (src->action_count > 0 && src->actions) {
        size_t cnt = src->action_count < action_cap ? src->action_count : action_cap;
        dst->actions = calloc(cnt, sizeof(claw_event_dispatcher_action_t));
        if (!dst->actions) {
            rtos_mem_free(dst->vars_json);
            dst->vars_json = NULL;
            return RTK_ERR_NOMEM;
        }
        for (size_t i = 0; i < cnt; i++) {
            _memcpy(&dst->actions[i], &src->actions[i], sizeof(claw_event_dispatcher_action_t));
            dst->actions[i].input_json = src->actions[i].input_json
                                         ? claw_strdup(src->actions[i].input_json) : NULL;
        }
        dst->action_count = cnt;
    }
    return RTK_SUCCESS;
}

/* Caller must hold s_rt.lock. Returns index or -1. */
static int find_rule_index_locked(const char *id)
{
    for (size_t i = 0; i < s_rt.nrules; i++) {
        if (strcmp(s_rt.rules[i].id, id) == 0) return (int)i;
    }
    return -1;
}

/* ---- Event processing: snapshot matched rules onto the heap, execute
 * lock-free (preserving rule boundaries so each rule gets its own ctx). ---- */

static void handle_event(const claw_event_t *ev)
{
    /* Intercept slash commands before rule processing. */
    if (strcmp(ev->event_type, "message") == 0 &&
            ev->source_channel[0] &&
            claw_im_dispatch_has_channel(ev->source_channel) &&
            session_cmd_try_handle(ev)) {
        return;
    }

    uint32_t now = (uint32_t)rtos_time_get_current_system_time_ms();

    /* Phase 1 + 2 (locked): find matching rules (honouring cooldown), then
     * deep-copy each onto the heap so execution can run without the lock. */
    claw_event_dispatcher_rule_t *snap[MAX_MATCH_PER_EVENT];
    char snap_rem[MAX_MATCH_PER_EVENT][CLAW_DISPATCHER_MATCH_TEXT_MAX];
    size_t snap_cnt = 0;
    bool consumed = false;
    size_t matched_total = 0;
    char remainder[CLAW_DISPATCHER_MATCH_TEXT_MAX];

    rtos_mutex_take(s_rt.lock, 0xFFFFFFFFUL);
    for (size_t ri = 0; ri < s_rt.nrules && snap_cnt < MAX_MATCH_PER_EVENT; ri++) {
        claw_event_dispatcher_rule_t *rule = &s_rt.rules[ri];
        if (!filter_matches(rule, ev, remainder, sizeof(remainder))) continue;

        /* Cooldown: skip (do not consume) if fired too recently. */
        if (rule->cooldown_ms > 0 && rule->last_fired_ts != 0 &&
                (now - rule->last_fired_ts) < rule->cooldown_ms) {
            RTK_LOGI(TAG, "rule '%s' in cooldown, skipped\n", rule->id);
            continue;
        }

        RTK_LOGI(TAG, "event '%s' matched rule '%s'\n", ev->event_type, rule->id);
        matched_total++;
        rule->last_fired_ts = now;
        rule->fire_count++;

        claw_event_dispatcher_rule_t *copy =
            (claw_event_dispatcher_rule_t *)rtos_mem_malloc(sizeof(*copy));
        if (copy) {
            size_t acap = s_rt.cfg.max_actions_per_rule > 0 ? s_rt.cfg.max_actions_per_rule : 4;
            if (copy_rule(copy, rule, acap) == RTK_SUCCESS) {
                snap[snap_cnt] = copy;
                strlcpy(snap_rem[snap_cnt], remainder, sizeof(snap_rem[snap_cnt]));
                snap_cnt++;
            } else {
                free_rule_contents(copy);
                rtos_mem_free(copy);
            }
        }
        if (rule->consume_on_match) { consumed = true; break; }
    }
    rtos_mutex_give(s_rt.lock);

    /* Phase 3 (lock-free): execute each snapshot rule with its own context.
     * Thread the event's emit generation so nested rtk_emit stays bounded. */
    for (size_t i = 0; i < snap_cnt; i++) {
        dispatch_rule(snap[i], ev, snap_rem[i], ev->emit_depth);
        free_rule_contents(snap[i]);
        rtos_mem_free(snap[i]);
    }

    /* Built-in route: a cap_scheduler agent-wake with no user-rule match goes
     * straight to the agent — one scheduler item = one intent, no router rule
     * required. Independent of default_route_messages_to_agent (that flag gates
     * only "message" events); a user rule matching the same event still wins
     * (matched_total>0 skips this). No instant-ack: a scheduled push shouldn't
     * greet the chat with "working on it...". */
    if (matched_total == 0
        && !consumed
        && strcmp(ev->event_type, CLAW_SCHED_AGENT_WAKE_EVENT) == 0
        && strcmp(ev->source_cap, "cap_scheduler") == 0) {
        claw_event_dispatcher_action_t wake;
        _memset(&wake, 0, sizeof(wake));
        wake.kind = CLAW_DISPATCHER_ACT_AGENT;
        dispatch_one_action(&wake, ev, NULL, 0, NULL);
    }

    /* Default: forward unmatched messages to the agent (with instant ack). */
    else if (matched_total == 0
        && !consumed
        && s_rt.cfg.default_route_messages_to_agent
        && strcmp(ev->event_type, "message") == 0) {

        if (ev->source_channel[0] && ev->chat_id[0] &&
                !claw_im_dispatch_channel_has_flag(ev->source_channel,
                                                   CLAW_IM_CHANNEL_FLAG_NO_ACK)) {
            claw_im_dispatch_send(ev->source_channel, ev->chat_id, CLAW_IM_ACK_MSG);
        }
        claw_event_dispatcher_action_t dflt;
        _memset(&dflt, 0, sizeof(dflt));
        dflt.kind = CLAW_DISPATCHER_ACT_AGENT;
        dispatch_one_action(&dflt, ev, NULL, 0, NULL);
    }

    /* Event-history sink hook (A7): funnel point for all events. NULL = off. */
    if (s_rt.cfg.on_event_logged) {
        s_rt.cfg.on_event_logged(ev, matched_total);
    }
}

/* ---- Worker task ---- */

static void claw_event_dispatcher_task(void *arg)
{
    (void)arg;
    claw_event_t *ev;

    while (s_rt.active) {
        if (rtos_queue_receive(s_rt.inbox, &ev, 1000) == RTK_SUCCESS) {
            handle_event(ev);
            claw_event_free(ev);
            rtos_mem_free(ev);
        }
    }
    rtos_task_delete(NULL);
}

/* ---- Public API ---- */

int claw_event_dispatcher_init(const claw_event_dispatcher_config_t *config)
{
    if (!config) return RTK_ERR_BADARG;
    if (s_rt.ready) return RTK_SUCCESS;

    _memset(&s_rt, 0, sizeof(s_rt));
    _memcpy(&s_rt.cfg, config, sizeof(claw_event_dispatcher_config_t));

    if (!s_rt.cfg.max_rules)              s_rt.cfg.max_rules              = 16;
    if (!s_rt.cfg.event_queue_len)        s_rt.cfg.event_queue_len        = 8;
    if (!s_rt.cfg.task_stack_size)        s_rt.cfg.task_stack_size        = 8 * 1024;
    if (!s_rt.cfg.task_priority)          s_rt.cfg.task_priority          = 2;
    if (!s_rt.cfg.core_submit_timeout_ms) s_rt.cfg.core_submit_timeout_ms = 5000;

    int err;
    err = rtos_mutex_create(&s_rt.lock);
    if (err != RTK_SUCCESS) return RTK_ERR_NOMEM;

    err = rtos_queue_create(&s_rt.inbox, s_rt.cfg.event_queue_len, sizeof(claw_event_t *));
    if (err != RTK_SUCCESS) {
        rtos_mutex_delete(s_rt.lock);
        return RTK_ERR_NOMEM;
    }

    s_rt.rules = calloc(s_rt.cfg.max_rules, sizeof(claw_event_dispatcher_rule_t));
    if (!s_rt.rules) {
        rtos_queue_delete(s_rt.inbox);
        rtos_mutex_delete(s_rt.lock);
        return RTK_ERR_NOMEM;
    }

    s_rt.ready = true;
    RTK_LOGI(TAG, "ameba claw event dispatcher ready (rules=%u q=%u)\n",
             (unsigned)s_rt.cfg.max_rules, (unsigned)s_rt.cfg.event_queue_len);
    return RTK_SUCCESS;
}

int claw_event_dispatcher_start(void)
{
    if (!s_rt.ready)  return RTK_FAIL;
    if (s_rt.active)  return RTK_SUCCESS;

    s_rt.active = true;
    if (rtos_task_create(NULL, "rtk_evr", claw_event_dispatcher_task, NULL,
                         s_rt.cfg.task_stack_size,
                         s_rt.cfg.task_priority) != RTK_SUCCESS) {
        s_rt.active = false;
        RTK_LOGE(TAG, "task create failed\n");
        return RTK_FAIL;
    }
    return RTK_SUCCESS;
}

int claw_event_dispatcher_stop(void)
{
    s_rt.active = false;
    return RTK_SUCCESS;
}

int claw_event_dispatcher_add_rule(const claw_event_dispatcher_rule_t *rule)
{
    if (!rule || !s_rt.ready) return RTK_ERR_BADARG;

    size_t acap = s_rt.cfg.max_actions_per_rule > 0 ? s_rt.cfg.max_actions_per_rule : 4;

    rtos_mutex_take(s_rt.lock, 0xFFFFFFFFUL);

    /* Same-id overwrite: keeps the running ruleset consistent with the file
     * layer (which already de-dupes by id) and avoids two live rules sharing
     * an id once hot add/update make edits frequent. */
    int existing = rule->id[0] ? find_rule_index_locked(rule->id) : -1;
    if (existing >= 0) {
        claw_event_dispatcher_rule_t tmp;
        _memset(&tmp, 0, sizeof(tmp));
        if (copy_rule(&tmp, rule, acap) != RTK_SUCCESS) {
            rtos_mutex_give(s_rt.lock);
            return RTK_ERR_NOMEM;
        }
        free_rule_contents(&s_rt.rules[existing]);
        s_rt.rules[existing] = tmp;
        rtos_mutex_give(s_rt.lock);
        RTK_LOGI(TAG, "rule '%s' replaced\n", rule->id);
        return RTK_SUCCESS;
    }

    if (s_rt.nrules >= s_rt.cfg.max_rules) {
        rtos_mutex_give(s_rt.lock);
        RTK_LOGE(TAG, "ruleset full (%u)\n", (unsigned)s_rt.cfg.max_rules);
        return RTK_ERR_NOMEM;
    }

    claw_event_dispatcher_rule_t *slot = &s_rt.rules[s_rt.nrules];
    if (copy_rule(slot, rule, acap) != RTK_SUCCESS) {
        _memset(slot, 0, sizeof(*slot));
        rtos_mutex_give(s_rt.lock);
        return RTK_ERR_NOMEM;
    }

    s_rt.nrules++;
    rtos_mutex_give(s_rt.lock);
    RTK_LOGI(TAG, "rule '%s' added (total=%u)\n", slot->id, (unsigned)s_rt.nrules);
    return RTK_SUCCESS;
}

void claw_event_dispatcher_free_rule(claw_event_dispatcher_rule_t *rule)
{
    free_rule_contents(rule);
}

/* ---- Runtime rule management (batch B) ---- */

int claw_event_dispatcher_get_rule(const char *id, claw_event_dispatcher_rule_t *out)
{
    if (!id || !out || !s_rt.ready) return RTK_ERR_BADARG;
    size_t acap = s_rt.cfg.max_actions_per_rule > 0 ? s_rt.cfg.max_actions_per_rule : 4;

    rtos_mutex_take(s_rt.lock, 0xFFFFFFFFUL);
    int idx = find_rule_index_locked(id);
    if (idx < 0) {
        rtos_mutex_give(s_rt.lock);
        return RTK_FAIL;
    }
    _memset(out, 0, sizeof(*out));
    int rc = copy_rule(out, &s_rt.rules[idx], acap);
    rtos_mutex_give(s_rt.lock);
    return rc;
}

int claw_event_dispatcher_list_rules(claw_event_dispatcher_rule_t **out, size_t *count)
{
    if (!out || !count || !s_rt.ready) return RTK_ERR_BADARG;
    size_t acap = s_rt.cfg.max_actions_per_rule > 0 ? s_rt.cfg.max_actions_per_rule : 4;

    rtos_mutex_take(s_rt.lock, 0xFFFFFFFFUL);
    *out = NULL;
    *count = 0;
    if (s_rt.nrules == 0) {
        rtos_mutex_give(s_rt.lock);
        return RTK_SUCCESS;
    }
    claw_event_dispatcher_rule_t *arr =
        calloc(s_rt.nrules, sizeof(claw_event_dispatcher_rule_t));
    if (!arr) {
        rtos_mutex_give(s_rt.lock);
        return RTK_ERR_NOMEM;
    }
    size_t n = 0;
    for (size_t i = 0; i < s_rt.nrules; i++) {
        if (copy_rule(&arr[n], &s_rt.rules[i], acap) == RTK_SUCCESS) n++;
    }
    rtos_mutex_give(s_rt.lock);
    *out = arr;
    *count = n;
    return RTK_SUCCESS;
}

void claw_event_dispatcher_free_rule_list(claw_event_dispatcher_rule_t *list, size_t count)
{
    if (!list) return;
    for (size_t i = 0; i < count; i++) free_rule_contents(&list[i]);
    free(list);
}

int claw_event_dispatcher_update_rule(const claw_event_dispatcher_rule_t *rule)
{
    if (!rule || !rule->id[0] || !s_rt.ready) return RTK_ERR_BADARG;
    size_t acap = s_rt.cfg.max_actions_per_rule > 0 ? s_rt.cfg.max_actions_per_rule : 4;

    rtos_mutex_take(s_rt.lock, 0xFFFFFFFFUL);
    int idx = find_rule_index_locked(rule->id);
    if (idx < 0) {
        rtos_mutex_give(s_rt.lock);
        return RTK_FAIL;
    }
    claw_event_dispatcher_rule_t tmp;
    _memset(&tmp, 0, sizeof(tmp));
    if (copy_rule(&tmp, rule, acap) != RTK_SUCCESS) {
        rtos_mutex_give(s_rt.lock);
        return RTK_ERR_NOMEM;
    }
    free_rule_contents(&s_rt.rules[idx]);
    s_rt.rules[idx] = tmp;
    rtos_mutex_give(s_rt.lock);
    RTK_LOGI(TAG, "rule '%s' updated\n", rule->id);
    return RTK_SUCCESS;
}

int claw_event_dispatcher_delete_rule(const char *id)
{
    if (!id || !s_rt.ready) return RTK_ERR_BADARG;

    rtos_mutex_take(s_rt.lock, 0xFFFFFFFFUL);
    int idx = find_rule_index_locked(id);
    if (idx < 0) {
        rtos_mutex_give(s_rt.lock);
        return RTK_FAIL;
    }
    free_rule_contents(&s_rt.rules[idx]);
    /* Compact the array (preserve order). */
    for (size_t i = (size_t)idx; i + 1 < s_rt.nrules; i++) {
        s_rt.rules[i] = s_rt.rules[i + 1];
    }
    s_rt.nrules--;
    _memset(&s_rt.rules[s_rt.nrules], 0, sizeof(s_rt.rules[s_rt.nrules]));
    rtos_mutex_give(s_rt.lock);
    RTK_LOGI(TAG, "rule '%s' deleted (total=%u)\n", id, (unsigned)s_rt.nrules);
    return RTK_SUCCESS;
}

/* ---- Queue accessor used by claw_event_publisher ---- */

rtos_queue_t claw_event_dispatcher_get_queue(void)
{
    return s_rt.inbox;
}

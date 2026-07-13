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

#define MAX_EMIT_DEPTH        3
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

/* ---- Template: expand @{ev.xxx} placeholders (single-pass scan) ---- */

static char *expand_template(const char *tpl, const claw_event_t *ev)
{
    if (!tpl) return NULL;

    /* Placeholder table: {token, replacement_value} */
    struct { const char *token; const char *value; } subs[] = {
        { "@{ev.text}",    ev->text ? ev->text : "" },
        { "@{ev.chat}",    ev->chat_id },
        { "@{ev.sender}",  ev->sender_id },
        { "@{ev.channel}", ev->source_channel },
        { "@{ev.type}",    ev->event_type },
        { "@{ev.cap}",     ev->source_cap },
    };
    const int nsubs = (int)(sizeof(subs) / sizeof(subs[0]));

    /* Single-pass scan: build output character by character */
    size_t out_cap = strlen(tpl) + 256;
    char *out = rtos_mem_malloc(out_cap);
    if (!out) return NULL;
    size_t out_len = 0;

    const char *p = tpl;
    while (*p) {
        if (*p == '@') {
            /* Check whether any placeholder matches at this position */
            int matched = 0;
            for (int i = 0; i < nsubs; i++) {
                size_t tlen = strlen(subs[i].token);
                if (strncmp(p, subs[i].token, tlen) == 0) {
                    /* Append replacement value */
                    size_t vlen = strlen(subs[i].value);
                    if (out_len + vlen + 1 > out_cap) {
                        size_t new_cap = out_len + vlen + 256;
                        char *tmp = (char *)rtos_mem_malloc(new_cap);
                        if (!tmp) { rtos_mem_free(out); return NULL; }
                        _memcpy(tmp, out, out_len);
                        rtos_mem_free(out);
                        out = tmp;
                        out_cap = new_cap;
                    }
                    _memcpy(out + out_len, subs[i].value, vlen);
                    out_len += vlen;
                    p += tlen;
                    matched = 1;
                    break;
                }
            }
            if (!matched) {
                /* Bare '@' — copy literally */
                if (out_len + 1 >= out_cap) {
                    size_t new_cap = out_cap * 2;
                    char *tmp = (char *)rtos_mem_malloc(new_cap);
                    if (!tmp) { rtos_mem_free(out); return NULL; }
                    _memcpy(tmp, out, out_len);
                    rtos_mem_free(out);
                    out = tmp;
                    out_cap = new_cap;
                }
                out[out_len++] = *p++;
            }
        } else {
            if (out_len + 1 >= out_cap) {
                size_t new_cap = out_cap * 2;
                char *tmp = (char *)rtos_mem_malloc(new_cap);
                if (!tmp) { rtos_mem_free(out); return NULL; }
                _memcpy(tmp, out, out_len);
                rtos_mem_free(out);
                out = tmp;
                out_cap = new_cap;
            }
            out[out_len++] = *p++;
        }
    }
    out[out_len] = '\0';
    return out;
}

/* ---- Rule filtering ---- */

static bool filter_matches(const claw_event_dispatcher_rule_t *rule, const claw_event_t *ev)
{
    const claw_event_dispatcher_match_t *m = &rule->match;
    bool ok = rule->enabled;

    /* Accumulated-boolean: each non-empty criterion must be satisfied */
    ok &= (m->event_type[0]    == '\0' || strcmp(m->event_type,    ev->event_type)     == 0);
    ok &= (m->source_cap[0]    == '\0' || strcmp(m->source_cap,    ev->source_cap)     == 0);
    ok &= (m->channel[0]       == '\0' || strcmp(m->channel,       ev->source_channel) == 0);
    ok &= (m->chat_id[0]       == '\0' || strcmp(m->chat_id,       ev->chat_id)        == 0);
    if (ok && m->text_contains[0]) {
        ok = ev->text != NULL && strstr(ev->text, m->text_contains) != NULL;
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

/* ---- Action dispatch ---- */

static void dispatch_action(const claw_event_dispatcher_action_t *act,
                             const claw_event_t *ev,
                             int depth)
{
    switch (act->kind) {

    case CLAW_DISPATCHER_ACT_AGENT: {
        /* TRIGGER policy: "trigger:" (8) + source_cap (31) + ":" (1) + message_id (95) + NUL (1) = 136 bytes */
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

        /* Send instant acknowledgement unless the channel handles its own ACK. */
        if (req.source_channel && req.source_chat_id &&
                !claw_im_dispatch_channel_has_flag(req.source_channel,
                                                    CLAW_IM_CHANNEL_FLAG_NO_ACK)) {
            claw_im_dispatch_send(req.source_channel, req.source_chat_id,
                                  CLAW_IM_ACK_MSG);
        }

        /* If this session already has a request in-flight, cancel it so the
         * new message is processed immediately rather than waiting for up to
         * max_tool_iterations rounds to complete. */
        claw_agent_cancel_for_session(sid);

        if (claw_agent_submit(&req, s_rt.cfg.core_submit_timeout_ms) != RTK_SUCCESS) {
            RTK_LOGE(TAG, "RUN_AGENT submit failed (queue full) for %s\n", sid);
            if (req.source_channel && req.source_chat_id) {
                claw_im_dispatch_send(req.source_channel, req.source_chat_id,
                                      "I'm too busy right now, please try again.");
            }
        }
        break;
    }

    case CLAW_DISPATCHER_ACT_CAP: {
        char  *input  = expand_template(act->input_json, ev);
        char  *output = NULL;

        claw_cap_call_context_t ctx;
        _memset(&ctx, 0, sizeof(ctx));
        ctx.request_id = ++s_call_seq;
        ctx.channel    = ev->source_channel;
        ctx.chat_id    = ev->chat_id;
        ctx.source_cap = ev->source_cap[0] ? ev->source_cap : NULL;
        ctx.caller     = CLAW_CAP_CALLER_INTERNAL;

        int rc = claw_cap_call(act->cap, input ? input : "{}", &ctx, &output);
        if (rc != RTK_SUCCESS && !act->fail_open) {
            RTK_LOGE(TAG, "CALL_CAP '%s' failed (%d)\n", act->cap, rc);
        }
        rtos_mem_free(input);
        rtos_mem_free(output);
        break;
    }

    case CLAW_DISPATCHER_ACT_SEND: {
        /* Resolve target channel: explicit cap field, then ev->target_channel, then source. */
        const char *ch = act->cap[0]          ? act->cap :
                         ev->target_channel[0] ? ev->target_channel :
                                                  ev->source_channel;
        if (!ch[0]) {
            if (!act->fail_open) RTK_LOGE(TAG, "SEND_MESSAGE: no channel\n");
            break;
        }
        char *text = expand_template(act->input_json, ev);
        if (!text) {
            if (!act->fail_open) RTK_LOGW(TAG, "SEND_MESSAGE: empty template, dropped\n");
            break;
        }
        /* Spawn a background task so the dispatcher worker is not blocked by
         * the synchronous HTTP POST inside the IM send handler. */
        /* Concurrency cap — avoids exhausting SRAM under rule-triggered floods. */
        if (s_send_im_inflight >= MAX_SEND_IM_TASKS) {
            RTK_LOGW(TAG, "SEND_MESSAGE: max concurrent tasks reached, dropped\n");
            rtos_mem_free(text);
            break;
        }
        send_im_arg_t *sia = (send_im_arg_t *)rtos_mem_malloc(sizeof(send_im_arg_t));
        if (!sia) {
            /* Drop rather than block the dispatcher worker on a sync send. */
            RTK_LOGW(TAG, "SEND_MESSAGE: alloc failed, message dropped\n");
            rtos_mem_free(text);
            break;
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
        }
        break;
    }

    case CLAW_DISPATCHER_ACT_EMIT: {
        if (depth >= MAX_EMIT_DEPTH) {
            RTK_LOGW(TAG, "EMIT_EVENT: depth limit %d reached\n", MAX_EMIT_DEPTH);
            break;
        }
        claw_event_t *derived = rtos_mem_malloc(sizeof(claw_event_t));
        if (!derived) break;
        if (claw_event_clone(ev, derived) != RTK_SUCCESS) {
            rtos_mem_free(derived);
            break;
        }
        if (act->input_json) {
            char *rendered = expand_template(act->input_json, ev);
            if (rendered) {
                rtos_mem_free(derived->payload_json);
                derived->payload_json = rendered;
            }
        }
        if (rtos_queue_send(s_rt.inbox, &derived, 100) != RTK_SUCCESS) {
            RTK_LOGE(TAG, "EMIT_EVENT: queue full\n");
            claw_event_free(derived);
            rtos_mem_free(derived);
        }
        break;
    }

    case CLAW_DISPATCHER_ACT_DROP:
        break;

    case CLAW_DISPATCHER_ACT_SCRIPT:
        RTK_LOGW(TAG, "RUN_SCRIPT: not supported\n");
        break;

    default:
        RTK_LOGW(TAG, "Unknown action kind %d\n", (int)act->kind);
        break;
    }
}

/* ---- Event processing: two-phase (collect then execute) ---- */

static void handle_event(const claw_event_t *ev)
{
    /* Intercept slash commands before Phase 1 rule processing.
     * Guard: only intercept events from channels that have a registered IM send
     * handler — this prevents synthetic 'message' events emitted by internal cap
     * rules from being consumed when their text happens to start with '/'. */
    if (strcmp(ev->event_type, "message") == 0 &&
            ev->source_channel[0] &&
            claw_im_dispatch_has_channel(ev->source_channel) &&
            session_cmd_try_handle(ev)) {
        return;
    }

    /*
     * Phase 1: Hold the lock, walk the ruleset, record which rule indices
     * matched and whether the first consumer was found.  Copy action data
     * out so Phase 2 can run without the lock.
     */
    size_t   hit_idx[MAX_MATCH_PER_EVENT];
    size_t   hit_cnt  = 0;
    bool     consumed = false;

    rtos_mutex_take(s_rt.lock, 0xFFFFFFFFUL);
    for (size_t ri = 0; ri < s_rt.nrules && hit_cnt < MAX_MATCH_PER_EVENT; ri++) {
        if (!filter_matches(&s_rt.rules[ri], ev)) continue;
        RTK_LOGI(TAG, "event '%s' matched rule '%s'\n", ev->event_type, s_rt.rules[ri].id);
        hit_idx[hit_cnt++] = ri;
        if (s_rt.rules[ri].consume_on_match) { consumed = true; break; }
    }
    rtos_mutex_give(s_rt.lock);

    /*
     * Phase 2: Snapshot all matched rules' actions under a single lock
     * acquisition, then execute them lock-free.
     *
     * Previous design re-acquired the lock once per action (O(actions) lock
     * cycles, heap allocation while holding the lock).  The new design takes
     * the lock once, deep-copies every action for every matched rule into a
     * local stack array, releases the lock, then executes — O(1) lock cycles
     * regardless of how many rules matched or how many actions each has.
     */
#define MAX_SNAP_ACTIONS (MAX_MATCH_PER_EVENT * 4)   /* 4 = max actions per rule */
    claw_event_dispatcher_action_t snaps[MAX_SNAP_ACTIONS];
    char *snap_inputs[MAX_SNAP_ACTIONS];
    size_t snap_cnt = 0;

    rtos_mutex_take(s_rt.lock, 0xFFFFFFFFUL);
    for (size_t mi = 0; mi < hit_cnt && snap_cnt < MAX_SNAP_ACTIONS; mi++) {
        size_t ri = hit_idx[mi];
        if (ri >= s_rt.nrules) continue;
        claw_event_dispatcher_rule_t *rule = &s_rt.rules[ri];
        for (size_t ai = 0; ai < rule->action_count && snap_cnt < MAX_SNAP_ACTIONS; ai++) {
            snaps[snap_cnt]        = rule->actions[ai];
            snap_inputs[snap_cnt]  = rule->actions[ai].input_json
                                     ? claw_strdup(rule->actions[ai].input_json) : NULL;
            snaps[snap_cnt].input_json = snap_inputs[snap_cnt];
            snap_cnt++;
        }
    }
    rtos_mutex_give(s_rt.lock);

    for (size_t si = 0; si < snap_cnt; si++) {
        dispatch_action(&snaps[si], ev, 0);
        rtos_mem_free(snap_inputs[si]);
    }

    /* Default: forward unmatched messages to agent */
    if (hit_cnt == 0
        && !consumed
        && s_rt.cfg.default_route_messages_to_agent
        && strcmp(ev->event_type, "message") == 0) {

        claw_event_dispatcher_action_t dflt;
        _memset(&dflt, 0, sizeof(dflt));
        dflt.kind = CLAW_DISPATCHER_ACT_AGENT;
        dispatch_action(&dflt, ev, 0);
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

    /* Fill zero-value defaults */
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
    RTK_LOGI(TAG, "ready (rules=%u q=%u)\n",
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

    rtos_mutex_take(s_rt.lock, 0xFFFFFFFFUL);

    if (s_rt.nrules >= s_rt.cfg.max_rules) {
        rtos_mutex_give(s_rt.lock);
        RTK_LOGE(TAG, "ruleset full (%u)\n", (unsigned)s_rt.cfg.max_rules);
        return RTK_ERR_NOMEM;
    }

    claw_event_dispatcher_rule_t *slot = &s_rt.rules[s_rt.nrules];
    _memcpy(slot, rule, sizeof(*slot));
    slot->actions      = NULL;
    slot->action_count = 0;

    if (rule->action_count > 0 && rule->actions) {
        size_t cap = s_rt.cfg.max_actions_per_rule > 0 ? s_rt.cfg.max_actions_per_rule : 4;
        size_t cnt = rule->action_count < cap ? rule->action_count : cap;
        slot->actions = calloc(cnt, sizeof(claw_event_dispatcher_action_t));
        if (!slot->actions) {
            rtos_mutex_give(s_rt.lock);
            return RTK_ERR_NOMEM;
        }
        for (size_t i = 0; i < cnt; i++) {
            _memcpy(&slot->actions[i], &rule->actions[i], sizeof(claw_event_dispatcher_action_t));
            slot->actions[i].input_json = rule->actions[i].input_json
                                          ? claw_strdup(rule->actions[i].input_json) : NULL;
        }
        slot->action_count = cnt;
    }

    s_rt.nrules++;
    rtos_mutex_give(s_rt.lock);
    RTK_LOGI(TAG, "rule '%s' added (total=%u)\n", slot->id, (unsigned)s_rt.nrules);
    return RTK_SUCCESS;
}



void claw_event_dispatcher_free_rule(claw_event_dispatcher_rule_t *rule)
{
    if (!rule) return;
    if (rule->actions) {
        for (size_t i = 0; i < rule->action_count; i++) {
            rtos_mem_free(rule->actions[i].input_json);
            rule->actions[i].input_json = NULL;
        }
        free(rule->actions);  /* calloc'd — free with libc free */
        rule->actions = NULL;
    }
    rule->action_count = 0;
}

/* ---- Queue accessor used by claw_event_publisher ---- */

rtos_queue_t claw_event_dispatcher_get_queue(void)
{
    return s_rt.inbox;
}

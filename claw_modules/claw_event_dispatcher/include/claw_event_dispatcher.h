/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "claw_cap.h"
#include "claw_event.h"
#include "claw_compat.h"
#include "ameba_claw_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* session_builder callback — maps event to session_id string */
typedef size_t (*claw_event_dispatcher_session_builder_fn)(const claw_event_t *event,
                                                        char *buf, size_t buf_size,
                                                        void *user_ctx);

typedef enum {
    CLAW_DISPATCHER_ACT_AGENT    = 0,
    CLAW_DISPATCHER_ACT_CAP     = 1,
    CLAW_DISPATCHER_ACT_SCRIPT   = 2,
    CLAW_DISPATCHER_ACT_SEND = 3,
    CLAW_DISPATCHER_ACT_EMIT   = 4,
    CLAW_DISPATCHER_ACT_DROP         = 5,
} claw_event_dispatcher_action_kind_t;

/* On action failure: continue this rule's remaining actions (default), or
 * stop them. Replaces the old fail_open bool (which only gated logging and
 * never affected control flow) — "stop" is genuinely new behavior. */
typedef enum {
    CLAW_DISPATCHER_ON_ERROR_CONTINUE = 0,
    CLAW_DISPATCHER_ON_ERROR_STOP     = 1,
} claw_event_dispatcher_on_error_t;

/* Text match mode for match.text. NONE = ignore match.text (fall back to the
 * legacy text_contains substring test, which stays independent). */
typedef enum {
    CLAW_DISPATCHER_TEXT_MATCH_NONE   = 0,
    CLAW_DISPATCHER_TEXT_MATCH_EXACT  = 1,
    CLAW_DISPATCHER_TEXT_MATCH_PREFIX = 2,
} claw_event_dispatcher_text_match_t;

/* Comparison operator for a per-action only_if guard. NONE = no guard. */
typedef enum {
    CLAW_DISPATCHER_OP_NONE     = 0,
    CLAW_DISPATCHER_OP_EQ       = 1,
    CLAW_DISPATCHER_OP_NE       = 2,
    CLAW_DISPATCHER_OP_GT       = 3,
    CLAW_DISPATCHER_OP_LT       = 4,
    CLAW_DISPATCHER_OP_GE       = 5,
    CLAW_DISPATCHER_OP_LE       = 6,
    CLAW_DISPATCHER_OP_CONTAINS = 7,
    CLAW_DISPATCHER_OP_EXISTS   = 8,
} claw_event_dispatcher_cmp_op_t;

/* Single-comparison action guard (A6). left is a template (e.g.
 * "@{ev.payload.temp}"); it is rendered, then compared against the literal
 * right. Numeric comparison is used when both sides parse as numbers,
 * otherwise string comparison. Deliberately NOT a nested boolean tree —
 * anything more complex falls through to the agent. */
typedef struct {
    claw_event_dispatcher_cmp_op_t op;
    char left[CLAW_DISPATCHER_GUARD_LEFT_MAX];
    char right[CLAW_DISPATCHER_GUARD_RIGHT_MAX];
} claw_event_dispatcher_guard_t;

typedef struct {
    char event_type[96];
    char source_cap[96];
    char channel[96];
    char chat_id[96];
    char text_contains[96];   /* substring match; empty = wildcard */
    char event_key[96];       /* exact match vs event correlation_id; empty = wildcard */
    char text[CLAW_DISPATCHER_MATCH_TEXT_MAX];       /* EXACT/PREFIX pattern */
    claw_event_dispatcher_text_match_t text_match_rule;
} claw_event_dispatcher_match_t;

typedef struct {
    claw_event_dispatcher_action_kind_t kind;
    char cap[64];           /* SEND: target channel; CAP: capability id */
    char *input_json;       /* heap template. SEND: plain text; CAP/EMIT: JSON object */
    claw_event_dispatcher_on_error_t on_error;
    bool capture_output;    /* feed this action's output into @{last.output} */
    claw_event_dispatcher_guard_t only_if;  /* op==NONE → always run */
} claw_event_dispatcher_action_t;

typedef struct {
    bool enabled;
    bool consume_on_match;
    char id[64];
    char ack[CLAW_DISPATCHER_RULE_ACK_MAX];  /* instant-ack template; empty = default */
    char *vars_json;                         /* heap; rule-level vars object → @{vars.*} */
    uint32_t cooldown_ms;                    /* min interval between fires; 0 = none */
    claw_event_dispatcher_match_t match;
    claw_event_dispatcher_action_t *actions;
    size_t action_count;
    /* ---- runtime-only, not persisted ---- */
    uint32_t last_fired_ts;                  /* rtos ms tick of last fire (cooldown) */
    uint32_t fire_count;                     /* times this rule has fired */
} claw_event_dispatcher_rule_t;

/* Optional event-history sink (A7). Called once per event at the dispatch
 * funnel with the matched-rule count. NULL = disabled. Reserved hook for a
 * future bounded event-history store; no storage is implemented yet. */
typedef void (*claw_event_dispatcher_event_sink_fn)(const claw_event_t *event,
                                                     size_t matched_rule_count);

typedef struct {
    size_t max_rules;
    size_t max_actions_per_rule;
    uint32_t event_queue_len;
    uint32_t task_stack_size;
    uint32_t task_priority;
    uint32_t core_submit_timeout_ms;
    bool default_route_messages_to_agent;
    claw_event_dispatcher_session_builder_fn session_builder;
    void *session_builder_ctx;
    claw_event_dispatcher_event_sink_fn on_event_logged;  /* optional; NULL = off */
} claw_event_dispatcher_config_t;

int claw_event_dispatcher_init(const claw_event_dispatcher_config_t *config);
int claw_event_dispatcher_start(void);
int claw_event_dispatcher_stop(void);
int claw_event_dispatcher_add_rule(const claw_event_dispatcher_rule_t *rule);
void claw_event_dispatcher_free_rule(claw_event_dispatcher_rule_t *rule);

/* ---- Runtime rule management (batch B; hot, take effect without restart) ---- */
int claw_event_dispatcher_get_rule(const char *id, claw_event_dispatcher_rule_t *out);
int claw_event_dispatcher_list_rules(claw_event_dispatcher_rule_t **out, size_t *count);
int claw_event_dispatcher_update_rule(const claw_event_dispatcher_rule_t *rule);
int claw_event_dispatcher_delete_rule(const char *id);
void claw_event_dispatcher_free_rule_list(claw_event_dispatcher_rule_t *list, size_t count);

#ifdef __cplusplus
}
#endif

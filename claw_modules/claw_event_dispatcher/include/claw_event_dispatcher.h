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

typedef struct {
    char event_type[96];
    char source_cap[96];
    char channel[96];
    char chat_id[96];
    char text_contains[96];   /* substring match; empty = wildcard */
} claw_event_dispatcher_match_t;

typedef struct {
    claw_event_dispatcher_action_kind_t kind;
    char cap[64];           /* for CALL_CAP / SEND_MESSAGE */
    char *input_json;       /* heap-allocated template, @{ev.field} */
    bool fail_open;
} claw_event_dispatcher_action_t;

typedef struct {
    bool enabled;
    bool consume_on_match;
    char id[64];
    claw_event_dispatcher_match_t match;
    claw_event_dispatcher_action_t *actions;
    size_t action_count;
} claw_event_dispatcher_rule_t;

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
} claw_event_dispatcher_config_t;

int claw_event_dispatcher_init(const claw_event_dispatcher_config_t *config);
int claw_event_dispatcher_start(void);
int claw_event_dispatcher_stop(void);
int claw_event_dispatcher_add_rule(const claw_event_dispatcher_rule_t *rule);
void claw_event_dispatcher_free_rule(claw_event_dispatcher_rule_t *rule);

#ifdef __cplusplus
}
#endif

/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "claw_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLAW_EVENT_SESSION_POLICY_CHAT    = 0,
    CLAW_EVENT_SESSION_POLICY_TRIGGER = 1,
} claw_event_session_policy_t;

typedef struct {
    char event_id[48];
    char source_cap[32];
    char event_type[32];       /* "message" or custom trigger type */
    char source_channel[16];
    char target_channel[16];
    char source_endpoint[64];
    char chat_id[96];
    char sender_id[96];
    char message_id[96];
    char correlation_id[96];
    char content_type[24];
    int64_t timestamp_ms;
    claw_event_session_policy_t session_policy;
    char *text;           /* heap-allocated */
    char *payload_json;   /* heap-allocated */
    uint8_t emit_depth;   /* rtk_emit chain generation: 0 = original event, +1 per
                           * emit hop. Bounds emit chains via CLAW_DISPATCHER_MAX_EMIT_DEPTH.
                           * Carried by claw_event_clone (whole-struct memcpy). */
} claw_event_t;

int claw_event_clone(const claw_event_t *src, claw_event_t *dst);
void claw_event_free(claw_event_t *event);
size_t claw_event_build_session_id(const claw_event_t *event, char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

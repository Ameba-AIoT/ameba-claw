/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "claw_event.h"
#include "claw_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

int claw_event_dispatcher_publish_message(const char *source_cap,
                                             const char *channel,
                                             const char *chat_id,
                                             const char *text,
                                             const char *sender_id,
                                             const char *message_id);
int claw_event_dispatcher_publish_trigger(const char *source_cap,
                                             const char *event_type,
                                             const char *event_key,
                                             const char *payload_json);

/* Publish a fully-populated event. Unlike publish_trigger (which only carries
 * source_cap/event_type/key/payload), this deep-copies EVERY field the caller
 * filled in — text, source_channel, chat_id, sender_id, session_policy, etc. —
 * so the dispatcher's AGENT action can wake the agent in a specific session and
 * reply to the originating chat. The caller keeps ownership of *src (its heap
 * strings are strdup'd here). If src->event_id is empty a fresh one is assigned.
 * Used by: cap_scheduler.c (action=agent → sched_agent_wake). */
int claw_event_dispatcher_publish_event(const claw_event_t *src);

#ifdef __cplusplus
}
#endif

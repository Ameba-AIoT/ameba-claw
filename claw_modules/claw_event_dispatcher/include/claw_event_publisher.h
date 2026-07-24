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

#ifdef __cplusplus
}
#endif

/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ameba_soc.h"
#include "claw_event_publisher.h"
#include "claw_event_dispatcher.h"
#include "claw_event.h"
#include "claw_compat.h"

/* Queue accessor — thin wrapper defined in claw_event_router.c */
rtos_queue_t claw_event_dispatcher_get_queue(void);

static const char *TAG = "claw_evt_pub";

/* Sequential event counter (avoids tick-count collisions on fast callers) */
static volatile uint32_t s_evt_seq;

/* ---- Internal: push a heap-allocated event onto the router queue ---- */

static int push_to_queue(claw_event_t *evt)
{
    rtos_queue_t q = claw_event_dispatcher_get_queue();
    if (!q) {
        RTK_LOGE(TAG, "router not started\n");
        claw_event_free(evt);
        rtos_mem_free(evt);
        return RTK_FAIL;
    }

    if (rtos_queue_send(q, &evt, 100) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "queue full, event dropped\n");
        claw_event_free(evt);
        rtos_mem_free(evt);
        return RTK_FAIL;
    }
    return RTK_SUCCESS;
}

/* ---- Public API ---- */

int claw_event_dispatcher_publish(const claw_event_t *event)
{
    claw_event_t *copy;
    int rc;

    if (!event) {
        return RTK_ERR_BADARG;
    }

    copy = rtos_mem_malloc(sizeof(claw_event_t));
    if (!copy) {
        return RTK_ERR_NOMEM;
    }

    rc = claw_event_clone(event, copy);
    if (rc != RTK_SUCCESS) {
        rtos_mem_free(copy);
        return rc;
    }

    return push_to_queue(copy);
}

int claw_event_dispatcher_publish_message(const char *source_cap,
                                      const char *channel,
                                      const char *chat_id,
                                      const char *text,
                                      const char *sender_id,
                                      const char *message_id)
{
    claw_event_t *evt = rtos_mem_malloc(sizeof(claw_event_t));
    if (!evt) {
        return RTK_ERR_NOMEM;
    }
    _memset(evt, 0, sizeof(claw_event_t));

    /* Unique ID: sequence number + tick */
    DiagSnPrintf(evt->event_id, sizeof(evt->event_id),
             "msg-%lu-%lu",
             (unsigned long)(++s_evt_seq),
             (unsigned long)rtos_time_get_current_system_time_ms());

    /* Populate fixed-size fields — strlcpy is cleaner than DiagSnPrintf(,, "%s",) */
    if (source_cap)  strlcpy(evt->source_cap,     source_cap,  sizeof(evt->source_cap));
    if (channel)     strlcpy(evt->source_channel, channel,     sizeof(evt->source_channel));
    if (chat_id)     strlcpy(evt->chat_id,         chat_id,     sizeof(evt->chat_id));
    if (sender_id)   strlcpy(evt->sender_id,       sender_id,   sizeof(evt->sender_id));
    if (message_id)  strlcpy(evt->message_id,      message_id,  sizeof(evt->message_id));

    strlcpy(evt->event_type,   "message", sizeof(evt->event_type));
    strlcpy(evt->content_type, "text",    sizeof(evt->content_type));

    evt->timestamp_ms   = (int64_t)rtos_time_get_current_system_time_ms();
    evt->session_policy = CLAW_EVENT_SESSION_POLICY_CHAT;

    if (text) {
        evt->text = strdup(text);
        if (!evt->text) {
            rtos_mem_free(evt);
            return RTK_ERR_NOMEM;
        }
    }

    return push_to_queue(evt);
}

int claw_event_dispatcher_publish_trigger(const char *source_cap,
                                      const char *event_type,
                                      const char *event_key,
                                      const char *payload_json)
{
    claw_event_t *evt = rtos_mem_malloc(sizeof(claw_event_t));
    if (!evt) {
        return RTK_ERR_NOMEM;
    }
    _memset(evt, 0, sizeof(claw_event_t));

    DiagSnPrintf(evt->event_id, sizeof(evt->event_id),
             "trg-%lu-%lu",
             (unsigned long)(++s_evt_seq),
             (unsigned long)rtos_time_get_current_system_time_ms());

    if (source_cap)  strlcpy(evt->source_cap,      source_cap,  sizeof(evt->source_cap));
    if (event_type)  strlcpy(evt->event_type,      event_type,  sizeof(evt->event_type));
    if (event_key)   strlcpy(evt->correlation_id,  event_key,   sizeof(evt->correlation_id));

    evt->timestamp_ms   = (int64_t)rtos_time_get_current_system_time_ms();
    evt->session_policy = CLAW_EVENT_SESSION_POLICY_TRIGGER;

    if (payload_json) {
        evt->payload_json = strdup(payload_json);
        if (!evt->payload_json) {
            rtos_mem_free(evt);
            return RTK_ERR_NOMEM;
        }
    }

    return push_to_queue(evt);
}

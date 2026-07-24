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

/* ---- Internal: shared event skeleton ---- */
static claw_event_t *new_event(const char *id_prefix)
{
    claw_event_t *evt = rtos_mem_malloc(sizeof(claw_event_t));
    if (!evt) {
        return NULL;
    }
    _memset(evt, 0, sizeof(claw_event_t));

    uint32_t now = (uint32_t)rtos_time_get_current_system_time_ms();
    DiagSnPrintf(evt->event_id, sizeof(evt->event_id), "%s-%lu-%lu",
                 id_prefix, (unsigned long)(++s_evt_seq), (unsigned long)now);
    evt->timestamp_ms = (int64_t)now;
    return evt;
}

static bool event_set_heap(claw_event_t *evt, char **dst, const char *src)
{
    if (!src) {
        return true;
    }
    *dst = strdup(src);
    if (!*dst) {
        rtos_mem_free(evt);
        return false;
    }
    return true;
}

/* ---- Public API ---- */

int claw_event_dispatcher_publish_message(const char *source_cap,
                                      const char *channel,
                                      const char *chat_id,
                                      const char *text,
                                      const char *sender_id,
                                      const char *message_id)
{
    claw_event_t *evt = new_event("msg");
    if (!evt) {
        return RTK_ERR_NOMEM;
    }

    /* Populate fixed-size fields — strlcpy is cleaner than DiagSnPrintf(,, "%s",) */
    if (source_cap)  strlcpy(evt->source_cap,     source_cap,  sizeof(evt->source_cap));
    if (channel)     strlcpy(evt->source_channel, channel,     sizeof(evt->source_channel));
    if (chat_id)     strlcpy(evt->chat_id,         chat_id,     sizeof(evt->chat_id));
    if (sender_id)   strlcpy(evt->sender_id,       sender_id,   sizeof(evt->sender_id));
    if (message_id)  strlcpy(evt->message_id,      message_id,  sizeof(evt->message_id));

    strlcpy(evt->event_type,   "message", sizeof(evt->event_type));
    strlcpy(evt->content_type, "text",    sizeof(evt->content_type));
    evt->session_policy = CLAW_EVENT_SESSION_POLICY_CHAT;

    if (!event_set_heap(evt, &evt->text, text)) {
        return RTK_ERR_NOMEM;
    }
    return push_to_queue(evt);
}

int claw_event_dispatcher_publish_trigger(const char *source_cap,
                                      const char *event_type,
                                      const char *event_key,
                                      const char *payload_json)
{
    claw_event_t *evt = new_event("trg");
    if (!evt) {
        return RTK_ERR_NOMEM;
    }

    if (source_cap)  strlcpy(evt->source_cap,      source_cap,  sizeof(evt->source_cap));
    if (event_type)  strlcpy(evt->event_type,      event_type,  sizeof(evt->event_type));
    if (event_key)   strlcpy(evt->correlation_id,  event_key,   sizeof(evt->correlation_id));

    evt->session_policy = CLAW_EVENT_SESSION_POLICY_TRIGGER;

    if (!event_set_heap(evt, &evt->payload_json, payload_json)) {
        return RTK_ERR_NOMEM;
    }
    return push_to_queue(evt);
}

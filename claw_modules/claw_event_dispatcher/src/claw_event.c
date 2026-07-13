/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ameba_soc.h"
#include "claw_event.h"

/*
 * Clone an event by pre-allocating all heap fields first so that dst
 * is never left in a partially-written state on allocation failure.
 */
int claw_event_clone(const claw_event_t *src, claw_event_t *dst)
{
    char *txt_copy = NULL;
    char *pay_copy = NULL;

    if (!src || !dst) {
        return RTK_ERR_BADARG;
    }

    /* Pre-allocate heap strings before modifying dst */
    if (src->text) {
        txt_copy = strdup(src->text);
        if (!txt_copy) {
            return RTK_ERR_NOMEM;
        }
    }

    if (src->payload_json) {
        pay_copy = strdup(src->payload_json);
        if (!pay_copy) {
            free(txt_copy);
            return RTK_ERR_NOMEM;
        }
    }

    /* All allocations succeeded — safe to write dst */
    _memcpy(dst, src, sizeof(claw_event_t));
    dst->text         = txt_copy;
    dst->payload_json = pay_copy;

    return RTK_SUCCESS;
}

/*
 * Free heap fields in an event.  Saves the pointers, zeros the struct
 * (preventing use-after-free on any nested reference), then frees.
 */
void claw_event_free(claw_event_t *event)
{
    char *t, *p;

    if (!event) {
        return;
    }

    t = event->text;
    p = event->payload_json;
    _memset(event, 0, sizeof(*event));
    free(t);
    free(p);
}

size_t claw_event_build_session_id(const claw_event_t *event, char *buf, size_t buf_size)
{
    if (!event || !buf || buf_size == 0) {
        return 0;
    }

    switch (event->session_policy) {
    case CLAW_EVENT_SESSION_POLICY_TRIGGER:
        DiagSnPrintf(buf, buf_size, "trigger:%s:%s",
                     event->source_cap[0] ? event->source_cap : "system",
                     event->message_id[0] ? event->message_id : event->event_id);
        break;

    case CLAW_EVENT_SESSION_POLICY_CHAT:
    default:
        if (event->source_channel[0] == '\0' && event->chat_id[0] == '\0') {
            DiagSnPrintf(buf, buf_size, "global");
        } else {
            DiagSnPrintf(buf, buf_size, "%s:%s",
                         event->source_channel, event->chat_id);
        }
        break;
    }

    return strlen(buf);
}

/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "session_cmd.h"
#include "cap_session_mgr.h"
#include "claw_im_dispatch.h"
#include "claw_compat.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ses_cmd";

/* Trim leading whitespace; return pointer into s. */
static const char *ltrim(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Trim leading and trailing whitespace; writes result to dst. */
static void trim_copy(const char *src, char *dst, size_t dst_size)
{
    if (!src || dst_size == 0) {
        if (dst && dst_size > 0) dst[0] = '\0';
        return;
    }
    src = ltrim(src);
    size_t len = strlen(src);
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t' ||
                       src[len - 1] == '\r' || src[len - 1] == '\n')) {
        len--;
    }
    if (len >= dst_size) len = dst_size - 1;
    if (len > 0) memcpy(dst, src, len);
    dst[len] = '\0';
}

/* Maximum reply text length — /list output can be up to 512 bytes + prefix. */
#define REPLY_TEXT_MAX 560

/* Maximum concurrent one-shot reply tasks. Excess falls back to sync send. */
#define MAX_REPLY_TASKS 3
static volatile int s_reply_tasks_inflight = 0;

/* Payload for the one-shot reply task — avoids blocking the dispatcher worker. */
typedef struct {
    char channel[32];
    char chat_id[96];
    char text[REPLY_TEXT_MAX];
} reply_task_arg_t;

static void session_reply_task(void *p)
{
    reply_task_arg_t *arg = (reply_task_arg_t *)p;
    claw_im_dispatch_send(arg->channel, arg->chat_id, arg->text);
    s_reply_tasks_inflight--;  /* decrement before free so dispatcher sees freed slot */
    rtos_mem_free(arg);
    rtos_task_delete(NULL);
}

/* Spawn a one-shot task to send the reply so the dispatcher worker is not
 * blocked by the synchronous HTTP POST inside claw_im_dispatch_send.
 * Stack: 8192 bytes — IM sends use mbedTLS which needs 4-8 KB of stack.
 * If the concurrency cap (MAX_REPLY_TASKS) is hit or task creation fails,
 * we fall back to a synchronous send from the calling context. */
static void send_reply(const claw_event_t *ev, const char *text)
{
    if (!text) return;
    /* For the local web channel, message_id holds the originating session alias.
     * Pass it as chat_id so cap_im_local_send routes to that session instead of
     * get_current(), which may have changed (e.g. /new sets a new current). */
    const char *cid = (strcmp(ev->source_channel, "local") == 0 && ev->message_id[0])
                      ? ev->message_id : ev->chat_id;
    /* Concurrency limit — avoids exhausting SRAM under command floods. */
    if (s_reply_tasks_inflight >= MAX_REPLY_TASKS) {
        RTK_LOGW(TAG, "reply tasks full (%d), sending sync\n",
                 MAX_REPLY_TASKS);
        claw_im_dispatch_send(ev->source_channel, cid, text);
        return;
    }
    reply_task_arg_t *arg = (reply_task_arg_t *)rtos_mem_malloc(sizeof(reply_task_arg_t));
    if (!arg) {
        /* Heap exhausted — send synchronously rather than dropping the reply. */
        RTK_LOGW(TAG, "reply arg alloc failed, sending sync\n");
        claw_im_dispatch_send(ev->source_channel, cid, text);
        return;
    }
    strlcpy(arg->channel, ev->source_channel, sizeof(arg->channel));
    strlcpy(arg->chat_id, cid, sizeof(arg->chat_id));
    strlcpy(arg->text, text, sizeof(arg->text));
    s_reply_tasks_inflight++;
    if (rtos_task_create(NULL, "ses_rply", session_reply_task, arg,
                         8192, 1) != RTK_SUCCESS) {
        s_reply_tasks_inflight--;
        rtos_mem_free(arg);
        /* Task table full — send synchronously. */
        RTK_LOGW(TAG, "task create failed, sending sync\n");
        claw_im_dispatch_send(ev->source_channel, cid, text);
    }
}

int session_cmd_try_handle(const claw_event_t *ev)
{
    if (!ev || !ev->text) return 0;

    /* Apply ltrim BEFORE the '/' check so leading whitespace doesn't drop commands */
    const char *text = ltrim(ev->text);
    if (text[0] != '/') return 0;

    const char *ch  = ev->source_channel;
    const char *cid = ev->chat_id;

    /* ---- /new [name] ---- */
    if (strncmp(text, "/new", 4) == 0 &&
            (text[4] == '\0' || text[4] == ' ' || text[4] == '\t')) {
        char name[40] = {0};
        trim_copy(text + 4, name, sizeof(name));
        char actual[40] = {0};
        int rc = cap_session_mgr_new(ch, cid,
                                     name[0] ? name : NULL,
                                     actual, sizeof(actual));
        char reply[128];
        if (rc == RTK_SUCCESS) {
            snprintf(reply, sizeof(reply), "✓ New session '%s' created.", actual);
        } else if (rc == CAP_SESSION_ERR_CONFLICT) {
            snprintf(reply, sizeof(reply), "Session name '%s' already exists.", name);
        } else {
            snprintf(reply, sizeof(reply), "Failed to create session.");
        }
        send_reply(ev, reply);
        return 1;
    }

    /* ---- /list ---- */
    if (strcmp(text, "/list") == 0 ||
            (strncmp(text, "/list", 5) == 0 && (text[5] == ' ' || text[5] == '\t'))) {
        /* Use heap for large list buffer to avoid > 128 B locals on task stack */
        char *buf = (char *)rtos_mem_malloc(512);
        if (!buf) {
            send_reply(ev, "Out of memory.");
            return 1;
        }
        int n = cap_session_mgr_list(ch, cid, buf, 512);
        char *reply = (char *)rtos_mem_malloc(560);
        if (!reply) {
            rtos_mem_free(buf);
            send_reply(ev, "Out of memory.");
            return 1;
        }
        if (n > 0) {
            snprintf(reply, 560, "Sessions:\n%s", buf);
        } else {
            snprintf(reply, 560, "No sessions found.");
        }
        send_reply(ev, reply);
        rtos_mem_free(reply);
        rtos_mem_free(buf);
        return 1;
    }

    /* ---- /resume <name> ---- */
    if (strncmp(text, "/resume", 7) == 0 &&
            (text[7] == '\0' || text[7] == ' ' || text[7] == '\t')) {
        char name[40] = {0};
        trim_copy(text + 7, name, sizeof(name));
        char reply[128];
        if (!name[0]) {
            snprintf(reply, sizeof(reply), "Usage: /resume <name>");
        } else {
            int rc = cap_session_mgr_resume(ch, cid, name);
            if (rc == RTK_SUCCESS) {
                snprintf(reply, sizeof(reply), "✓ Switched to session '%s'.", name);
            } else if (rc == CAP_SESSION_ERR_NOT_FOUND) {
                snprintf(reply, sizeof(reply),
                         "Session '%s' not found. Use /list to see available sessions.", name);
            } else {
                snprintf(reply, sizeof(reply), "Failed to switch session.");
            }
        }
        send_reply(ev, reply);
        return 1;
    }

    /* ---- /rename <name> ---- */
    if (strncmp(text, "/rename", 7) == 0 &&
            (text[7] == '\0' || text[7] == ' ' || text[7] == '\t')) {
        char name[40] = {0};
        trim_copy(text + 7, name, sizeof(name));
        char reply[128];
        if (!name[0]) {
            snprintf(reply, sizeof(reply), "Usage: /rename <name>");
        } else {
            bool use_alias = ev->message_id[0] && strcmp(ch, "local") == 0;
            int rc = use_alias
                     ? cap_session_mgr_rename_alias(ch, cid, ev->message_id, name)
                     : cap_session_mgr_rename(ch, cid, name);
            if (rc == RTK_SUCCESS) {
                snprintf(reply, sizeof(reply), "✓ Session renamed to '%s'.", name);
            } else if (rc == CAP_SESSION_ERR_NOT_FOUND) {
                snprintf(reply, sizeof(reply), "Session not found.");
            } else if (rc == CAP_SESSION_ERR_CONFLICT) {
                snprintf(reply, sizeof(reply), "Session name '%s' already exists.", name);
            } else if (rc == RTK_ERR_BADARG) {
                snprintf(reply, sizeof(reply), "Invalid session name '%s'.", name);
            } else {
                snprintf(reply, sizeof(reply), "Failed to rename session.");
            }
        }
        send_reply(ev, reply);
        return 1;
    }

    /* ---- /delete <name> ---- */
    if (strncmp(text, "/delete", 7) == 0 &&
            (text[7] == '\0' || text[7] == ' ' || text[7] == '\t')) {
        char name[40] = {0};
        trim_copy(text + 7, name, sizeof(name));
        char reply[128];
        if (!name[0]) {
            snprintf(reply, sizeof(reply), "Usage: /delete <name>");
        } else {
            int rc = cap_session_mgr_delete(ch, cid, name);
            if (rc == RTK_SUCCESS) {
                snprintf(reply, sizeof(reply), "✓ Session '%s' deleted.", name);
            } else if (rc == CAP_SESSION_ERR_CURRENT) {
                snprintf(reply, sizeof(reply),
                         "Cannot delete the current session. Use /resume to switch first.");
            } else if (rc == CAP_SESSION_ERR_NOT_FOUND) {
                snprintf(reply, sizeof(reply),
                         "Session '%s' not found. Use /list to see available sessions.", name);
            } else {
                snprintf(reply, sizeof(reply), "Failed to delete session.");
            }
        }
        send_reply(ev, reply);
        return 1;
    }

    /* ---- /clear ---- */
    if (strcmp(text, "/clear") == 0 ||
            (strncmp(text, "/clear", 6) == 0 && (text[6] == ' ' || text[6] == '\t'))) {
        int rc = (ev->message_id[0] && strcmp(ch, "local") == 0)
                 ? cap_session_mgr_clear_chat_alias(ch, cid, ev->message_id)
                 : cap_session_mgr_clear_chat(ch, cid);
        send_reply(ev, rc == RTK_SUCCESS
                   ? "✓ Conversation cleared."
                   : "Failed to clear conversation.");
        return 1;
    }

    return 0;
}

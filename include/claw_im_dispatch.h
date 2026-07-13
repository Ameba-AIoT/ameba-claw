/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

/**
 * Channel capability flags — declared at registration time so callers
 * never need strcmp(channel, "serial") or strcmp(channel, "local").
 *
 * Each bit describes a behavioural difference that the framework (on_response,
 * on_tool_progress, dispatcher ACK) must respect.  New channels that behave
 * like a normal IM channel (Telegram, WeChat, Feishu, QQ) pass flags=0.
 */

/** Do not send per-tool progress messages to this channel.
 *  Use for channels that already display per-call output (serial AT console)
 *  or where progress noise degrades the UX. */
#define CLAW_IM_CHANNEL_FLAG_SILENT_PROGRESS  (1u << 0)

/** Do not append the tool_trace summary to the final reply.
 *  Use for channels where the trace is redundant (serial prints every call)
 *  or causes Markdown rendering noise (local WebUI). */
#define CLAW_IM_CHANNEL_FLAG_SILENT_TRACE     (1u << 1)

/** Do not send the instant ACK ("working on it...") from the event dispatcher.
 *  Use for channels that submit directly via claw_agent_submit() and handle
 *  their own acknowledgement (serial AT console). */
#define CLAW_IM_CHANNEL_FLAG_NO_ACK           (1u << 2)

/** The chat_id on this channel is an ephemeral session identifier that does
 *  not survive across message boundaries (e.g. local WebUI browser sessions).
 *  The LLM must NOT embed this chat_id in scheduler jobs or other persistent
 *  references — the session will be gone before the job fires.
 *  Checked by the im_conversation context provider to suppress IM context
 *  injection for channels where scheduler reminders make no sense. */
#define CLAW_IM_CHANNEL_FLAG_EPHEMERAL_SESSION (1u << 3)

/**
 * Function type for sending a message on a specific IM channel.
 */
typedef void (*claw_im_send_fn_t)(const char *chat_id, const char *text);

/**
 * Register an outbound send function for a named channel.
 * flags:        bitwise-OR of CLAW_IM_CHANNEL_FLAG_*
 * send_cap_name: LLM-visible capability name for sending text on this channel
 *               (e.g. "wechat_send_text", "telegram_send_text").  NULL or ""
 *               for channels that have no LLM-callable send cap (serial/local).
 *               Used by the im_conversation context provider so the LLM can
 *               reference the correct cap in scheduler jobs without hardcoding
 *               channel names.
 */
int claw_im_dispatch_register_with_flags(const char *channel,
                                          claw_im_send_fn_t fn,
                                          uint32_t flags,
                                          const char *send_cap_name);

/**
 * Convenience wrapper: register with flags=0, send_cap_name=NULL.
 * Callers that do not need a cap name (or set it later) use this.
 */
static inline int claw_im_dispatch_register(const char *channel,
                                             claw_im_send_fn_t fn)
{
    return claw_im_dispatch_register_with_flags(channel, fn, 0, NULL);
}

/**
 * Return the send-text cap name for a channel, or NULL if none registered.
 * e.g. claw_im_dispatch_send_cap("wechat") → "wechat_send_text"
 */
const char *claw_im_dispatch_send_cap(const char *channel);

/**
 * Send a message via the registered handler for the given channel.
 */
void claw_im_dispatch_send(const char *channel, const char *chat_id,
                            const char *text);

/**
 * Return non-zero if a send handler has been registered for the given channel.
 */
int claw_im_dispatch_has_channel(const char *channel);

/**
 * Query whether a channel has a given flag set.
 * Returns non-zero if the flag is set, 0 if not set or channel not registered.
 */
uint32_t claw_im_dispatch_channel_flags(const char *channel);

static inline int claw_im_dispatch_channel_has_flag(const char *channel,
                                                      uint32_t flag)
{
    return (claw_im_dispatch_channel_flags(channel) & flag) != 0;
}

/**
 * Function type for sending a media file on a specific IM channel.
 */
typedef int (*claw_im_send_media_fn_t)(const char *chat_id,
                                       const char *vfs_path,
                                       const char *caption,
                                       const char *media_kind);

/**
 * Register a media-send function for a named channel.
 */
int claw_im_dispatch_register_media(const char *channel,
                                     claw_im_send_media_fn_t fn);

/**
 * Send a media file via the registered handler for the given channel.
 */
int claw_im_dispatch_send_media(const char *channel, const char *chat_id,
                                 const char *vfs_path, const char *caption,
                                 const char *media_kind);

/**
 * Generic cap execute handler for "send text to IM channel" tools.
 * Eliminates copy-paste across cap_im_telegram/wechat/feishu/qq/local.
 */
int claw_im_cap_execute_send_text(const char *input_json,
                                   char **output,
                                   claw_im_send_fn_t send_fn);

/**
 * Progress-send function type.  Receives the same text as claw_im_send_fn_t
 * plus the request_id so the channel can apply per-request rate limiting
 * entirely within its own code.
 * Return value is ignored by the caller.
 */
typedef void (*claw_im_send_progress_fn_t)(const char *chat_id,
                                            const char *text,
                                            uint32_t    request_id);

/**
 * Register a progress-send function for a channel.
 * When registered, claw_im_dispatch_send_progress() calls this instead of
 * falling back to the generic send function.  The channel is responsible for
 * all rate-limiting, budgeting and user notification.
 */
int claw_im_dispatch_register_progress(const char *channel,
                                        claw_im_send_progress_fn_t fn);

/**
 * Send a progress message via the channel's registered progress handler.
 * Returns 0 if the channel had a progress handler (message delivered to it).
 * Returns -1 if no progress handler is registered for this channel — the
 * caller should fall back to the generic budget + claw_im_dispatch_send().
 */
int claw_im_dispatch_send_progress(const char *channel,
                                    const char *chat_id,
                                    const char *text,
                                    uint32_t    request_id);

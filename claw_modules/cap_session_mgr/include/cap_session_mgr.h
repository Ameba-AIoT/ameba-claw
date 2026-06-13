/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stddef.h>
#include "claw_event.h"
#include "claw_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the session manager.
 *
 * @param session_root_dir  Root directory for session storage (e.g. "/session").
 *                          The directory and a "chat_map" subdirectory will be
 *                          created if they do not already exist.
 * @return RTK_SUCCESS on success.
 */
int cap_session_mgr_init(const char *session_root_dir);

/**
 * Build a session ID string from an incoming event.
 *
 * Suitable for use as a claw_event_dispatcher_session_builder_fn callback.
 * Maps (source_channel, chat_id) → a stable, versioned session ID string of
 * the form "channel:chat_id:vN".  A lightweight JSON mapping file is stored in
 * {session_root}/chat_map/ so the version survives reboots.
 *
 * @param event     Incoming event (must not be NULL).
 * @param buf       Output buffer for the session ID string.
 * @param buf_size  Size of buf.
 * @param user_ctx  Unused; pass NULL.
 * @return Number of bytes written (excluding NUL terminator), like DiagSnPrintf.
 */
size_t cap_session_mgr_build_session_id(const claw_event_t *event,
                                         char *buf, size_t buf_size,
                                         void *user_ctx);

/**
 * Increment the persisted version number for (channel, chat_id), so that
 * subsequent build_session_id() calls return a session ID with a higher
 * "vN" suffix. Old session history files are kept on disk; they simply
 * stop matching the new session ID. This is the "soft reset" path:
 * preserve history but start a fresh conversation.
 *
 * @param channel  Source channel name (e.g. "telegram", "wechat", "serial").
 * @param chat_id  Chat identifier within that channel.
 * @return RTK_SUCCESS on success, RTK_ERR_BADARG / RTK_FAIL otherwise.
 *         When the (channel, chat_id) has no existing mapping file, one
 *         is created at version=2 (so the next session_id is "channel:chat_id:v2").
 */
int cap_session_mgr_bump_version(const char *channel, const char *chat_id);

#ifdef __cplusplus
}
#endif

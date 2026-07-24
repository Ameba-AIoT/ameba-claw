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

/* Error codes specific to session_mgr operations (not in rtk_status.h) */
#define CAP_SESSION_ERR_NOT_FOUND   (-10)  /* alias does not exist */
#define CAP_SESSION_ERR_CONFLICT    (-11)  /* alias already exists */
#define CAP_SESSION_ERR_CURRENT     (-12)  /* refused: target is current session */

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
 * Maps (source_channel, chat_id) to a stable session ID string of the form
 * "channel:chat_id:alias" where alias is the current session alias from the
 * chat_map file.  Creates the file with alias="default" if it does not exist.
 *
 * @param event     Incoming event (must not be NULL).
 * @param buf       Output buffer for the session ID string.
 * @param buf_size  Size of buf.
 * @param user_ctx  Unused; pass NULL.
 * @return Number of bytes written (excluding NUL terminator).
 */
size_t cap_session_mgr_build_session_id(const claw_event_t *event,
                                         char *buf, size_t buf_size,
                                         void *user_ctx);

/**
 * /new — Create a new named session for (channel, chat_id) and switch to it.
 *
 * alias is NULL or empty: auto-name by "MMDD-HHMM" (clock synced) or "s<N>".
 * out_alias/out_size: optional — receives the actual alias used (for echo).
 * Returns RTK_SUCCESS / RTK_ERR_BADARG / CAP_SESSION_ERR_CONFLICT / RTK_FAIL.
 */
int cap_session_mgr_new(const char *channel, const char *chat_id,
                        const char *alias,
                        char *out_alias, size_t out_size);

/**
 * /resume — Switch (channel, chat_id) current session to alias.
 *
 * Returns RTK_SUCCESS / CAP_SESSION_ERR_NOT_FOUND / RTK_FAIL.
 */
int cap_session_mgr_resume(const char *channel, const char *chat_id,
                           const char *alias);

/**
 * /list — Format all sessions for (channel, chat_id) into out_buf.
 *
 * Format: one line per session; current session has " (current)" appended.
 * Returns bytes written (excl. NUL); truncated to out_size if necessary.
 */
int cap_session_mgr_list(const char *channel, const char *chat_id,
                         char *out_buf, size_t out_size);

/**
 * /rename — Rename the current session of (channel, chat_id) to new_alias.
 *
 * Returns RTK_SUCCESS / RTK_ERR_BADARG / CAP_SESSION_ERR_CONFLICT / RTK_FAIL.
 */
int cap_session_mgr_rename(const char *channel, const char *chat_id,
                           const char *new_alias);

/**
 * Rename a specific session (old_alias) to new_alias without changing current.
 *
 * Unlike cap_session_mgr_rename, this targets old_alias explicitly and does not
 * modify the current pointer unless current == old_alias (keeps the map valid).
 * Returns RTK_SUCCESS / RTK_ERR_BADARG / CAP_SESSION_ERR_NOT_FOUND /
 *         CAP_SESSION_ERR_CONFLICT / RTK_FAIL.
 */
int cap_session_mgr_rename_alias(const char *channel, const char *chat_id,
                                  const char *old_alias, const char *new_alias);

/**
 * /delete — Delete the named session from (channel, chat_id).
 *
 * Deletes the named session from (channel, chat_id).  If alias is the current
 * session, current is auto-switched to the first remaining alias.
 * Calls claw_memory_clear_session on the session_id and removes the alias from
 * the sessions array.  Deletes the chat_map file when sessions becomes empty.
 * Returns RTK_SUCCESS / CAP_SESSION_ERR_NOT_FOUND / RTK_FAIL.
 */
int cap_session_mgr_delete(const char *channel, const char *chat_id,
                           const char *alias);

/**
 * /clear — Clear the conversation history of (channel, chat_id) current session.
 *
 * Does not delete or modify the chat_map.  Other sessions are unaffected.
 * Returns RTK_SUCCESS / RTK_FAIL.
 */
int cap_session_mgr_clear_chat(const char *channel, const char *chat_id);

/**
 * Clear the conversation history of a specific session alias without changing current.
 *
 * Returns RTK_SUCCESS / RTK_ERR_BADARG / RTK_FAIL.
 */
int cap_session_mgr_clear_chat_alias(const char *channel, const char *chat_id,
                                      const char *alias);

/**
 * Read the current session alias for (channel, chat_id) into out_buf.
 *
 * Read-only; does not modify state.
 * Returns RTK_SUCCESS / RTK_ERR_BADARG / RTK_FAIL.
 */
int cap_session_mgr_get_current(const char *channel, const char *chat_id,
                                char *out_buf, size_t out_size);

#ifdef __cplusplus
}
#endif

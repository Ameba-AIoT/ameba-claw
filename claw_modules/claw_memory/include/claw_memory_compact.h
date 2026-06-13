/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "claw_compat.h"
#include "claw_agent.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the asynchronous compaction subsystem.
 *
 *  - session_root:  directory holding s_*.json session files (e.g. "vfs:/session")
 *  - file_mutex:    the same mutex claw_memory uses to serialize session-file IO
 *  - char_threshold: total session char count above which compaction triggers;
 *                   0 disables compaction entirely (no task is spawned).
 *  - protect_last:  number of trailing turns kept verbatim, never compacted.
 *
 * Must be called after claw_memory_init() and before claw_agent_start().
 *
 * On startup this also scans `session_root` for any *.json.bak residue from a
 * crash and atomically rolls them back via rename(*.bak, *).
 */
int claw_memory_compact_init(const char *session_root,
                             rtos_mutex_t file_mutex,
                             uint16_t char_threshold,
                             uint8_t protect_last);

/* Enqueue a compaction job for the given session_id. Idempotent: if a job for
 * the same sid is already inflight or queued, the call is silently dropped.
 * Safe to call from any task; never blocks. */
void claw_memory_compact_enqueue(const char *session_id);

/* Serialize LLM calls (TLS handshakes) between mem_extract and mem_compact —
 * the device only has 1-2 TLS sockets and they must not race. Both modules
 * wrap their claw_agent_llm_chat_messages call with these. */
void claw_memory_summary_lock_take(void);
void claw_memory_summary_lock_give(void);

/* SYSTEM_PROMPT provider that injects the session's `compaction_summary`
 * field as "## Earlier Conversation Summary". Returns RTK_FAIL when there
 * is no summary yet, so providers earlier in the chain remain valid. */
extern claw_agent_context_provider_t claw_memory_compaction_summary_provider;

#ifdef __cplusplus
}
#endif

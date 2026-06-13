/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "claw_compat.h"
#include "claw_agent.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum size of a single session or memory file read into RAM.
 * All slurp helpers (claw_memory.c and claw_memory_compact.c) must use
 * this constant so the limits stay in sync.
 *
 * Raised 32K → 256K for the token-budget long-context model: a session must be
 * able to hold ~100K tokens of (mostly Chinese) history before the token-budget
 * compaction trigger fires. RAM note: slurp reads the whole file into one
 * rtos_mem_malloc and cJSON parses it, so peak transient is a few × this — keep
 * an eye on get_heap_info on byte-heavy (English/JSON) sessions; the append-time
 * soft-cap shed (claw_memory.c) guarantees the file never exceeds this, so it is
 * never slurp-rejected. See design_spec/long_context_compaction_strategy.md §7. */
#define CLAW_MEMORY_MAX_FILE_SIZE (256 * 1024)

/* ---- Structured long-term memory item ---- */

#define CLAW_MEMORY_CONTENT_MAX 256
#define CLAW_MEMORY_TAG_MAX     64
#define CLAW_MEMORY_SOURCE_MAX  64
#define CLAW_MEMORY_SUMMARY_MAX 64

typedef struct {
    uint32_t    id;                              /* auto-assigned unique id */
    char        source[CLAW_MEMORY_SOURCE_MAX];  /* origin: "user", "llm", "system" */
    char        content[CLAW_MEMORY_CONTENT_MAX];
    char        tags[CLAW_MEMORY_TAG_MAX];       /* comma-separated tag string */
    char        summary[CLAW_MEMORY_SUMMARY_MAX];/* ≤40-char label for index injection */
    uint32_t    access_count;
    uint32_t    created_at;                      /* unix timestamp */
} claw_memory_item_t;

/* ---- Config ---- */

typedef struct {
    const char *memory_root_dir;   /* e.g. "vfs:/memory" — for long_term_store.json and session data */
    const char *session_root_dir;  /* e.g. "vfs:/session" */
    const char *profile_root_dir;  /* for AGENTS/SOUL/IDENTITY/USER/MEMORY.md; falls back to memory_root_dir if NULL */
    size_t max_session_turns;      /* max turns per session file (ring buffer), default 20 */

    /* S2 compaction tuning. */
    uint8_t  compaction_protect_last;    /* default 4 when 0 */

    /* Token-budget compaction (preferred over char threshold). When the last
     * request's real prompt_tokens reaches compaction_token_threshold, older
     * turns are summarized. window_tokens is the hard model ceiling used for
     * synchronous trim. 0 keeps defaults. */
    uint32_t compaction_token_threshold; /* default 110000 when 0 */
    uint32_t context_window_tokens;      /* default 128000 when 0 */
} claw_memory_config_t;

int claw_memory_init(const claw_memory_config_t *config);

/* Return the configured session root directory (e.g. "vfs:/session").
 * Valid only after claw_memory_init(). Returns "" before init. */
const char *claw_memory_get_session_root(void);

/* Resolve the on-disk path for a session_id under session_root, using the
 * djb2+sanitize scheme that matches claw_memory's internal IO. Other modules
 * (e.g. claw_memory_compact) MUST call this — re-implementing the hash
 * elsewhere would silently desynchronize file paths. */
void claw_memory_session_file_path(const char *session_id,
                                   char *out, size_t out_size);

/* Copy at most `max_bytes` bytes of `src` into `dst`, but never split a UTF-8
 * multi-byte sequence in the middle. dst_size must be ≥ max_bytes + 1.
 * Returns the number of bytes written (excluding NUL). Used by callers that
 * truncate human-readable text into bounded labels. */
size_t claw_memory_utf8_safe_copy(char *dst, size_t dst_size,
                                  const char *src, size_t max_bytes);

/* Like claw_memory_utf8_safe_copy, but when `src` is longer than the limit and
 * has to be truncated, append a short UTF-8 marker ("…[截断]") so the reader —
 * and the LLM on recall — can tell the stored text was cut off. When `src`
 * fits, behaves identically to claw_memory_utf8_safe_copy (no marker added). */
size_t claw_memory_utf8_safe_copy_marked(char *dst, size_t dst_size,
                                         const char *src, size_t max_bytes);

/* append_session_turn callback — compatible with claw_agent_config_t.append_session_turn.
 * tool_msgs_json (optional): verbatim serialized tool round-trips for this turn,
 * in the wire format identified by `backend`; stored for byte-identical
 * cross-turn replay. Pass NULL/0 when no tools ran. */
int claw_memory_append_session_turn(const char *session_id,
                                           const char *user_text,
                                           const char *assistant_text,
                                           const char *tool_msgs_json,
                                           int backend,
                                           uint32_t prompt_tokens,
                                           void *user_ctx);

/* ---- Structured CRUD API ---- */

/* Store a new memory item. On success, item->id is filled in. */
int claw_memory_store(claw_memory_item_t *item);

/* Recall items whose content contains keyword. Returns JSON array string (caller frees). */
char *claw_memory_recall(const char *keyword, int max_results);

/* Update content of item with given id. */
int claw_memory_update(uint32_t id, const char *new_content);

/* Delete item with given id. */
int claw_memory_forget(uint32_t id);

/* List all items. Returns JSON array string (caller frees). */
char *claw_memory_list(int max_results);

/* Delete session history file for a given session_id (0 = success, -1 = not found/error) */
int claw_memory_clear_session(const char *session_id);

/* Delete ALL session history files under session_root_dir */
int claw_memory_clear_all_sessions(void);

/* Delete all long-term memories */
int claw_memory_clear_long_term(void);

/* claw_agent context providers */
extern claw_agent_context_provider_t claw_memory_profile_provider;
extern claw_agent_context_provider_t claw_memory_session_history_provider;
extern claw_agent_context_provider_t claw_memory_long_term_label_provider;

#ifdef __cplusplus
}
#endif

/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * claw_memory_compact.c — asynchronous session-history compaction.
 *
 * Pipeline:
 *   claw_memory_append_session_turn (engine_task)
 *     ↓ when total_chars > threshold
 *   claw_memory_compact_enqueue(sid)
 *     ↓ via FreeRTOS queue (depth 2, sid-deduped)
 *   compact_task (mem_compact, 12 KB stack, priority 1)
 *     ↓
 *   do_compact(sid):
 *       1) take file_mutex (held for the whole compaction; appends wait)
 *       2) read s_<sid>.json, decide split = len(turns) - protect_last
 *       3) if older < 2 → skip
 *       4) take summary_lock (serializes TLS handshakes vs mem_extract)
 *       5) call claw_agent_llm_chat_messages with a "summarize older turns"
 *          prompt; max_tokens=300
 *       6) release summary_lock
 *       7) merge old + new summary (cap at 800 chars; on overflow drop the
 *          older summary rather than synchronously calling LLM again — that
 *          recursive compaction is a future enhancement)
 *       8) write the new file via rename(*.json, *.json.bak) → write new
 *          → remove(*.bak); any failure rolls back via rename(*.bak, *)
 *       9) release file_mutex
 *
 * On any LLM failure we set compaction_pending=true so the next append turn
 * can re-trigger; we never lose history.
 */

#include "claw_memory_compact.h"
#include "claw_memory.h"
#include "claw_agent_llm.h"
#include "claw_config.h"
#include "cJSON.h"
#include "basic_types.h"
#include "os_wrapper.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "mem_compact"

#define COMPACT_QUEUE_LEN     2
#define COMPACT_STACK_SIZE    (12 * 1024)  /* peak 6.0 KB; 12 KB = 2x margin */
#define COMPACT_PATH_MAX      192
#define COMPACT_SID_MAX       64    /* per-slot session_id buffer */
/* Slot count: queue_len + 1 (the job currently being executed by the task
 * is no longer in the queue). With QUEUE_LEN=2 we need 3 slots to dedup
 * correctly across "1 running + 2 queued". A single-slot s_inflight_sid
 * was strcpy-clobbered by every new sid, so a different-sid enqueue could
 * silently re-admit a duplicate of the running sid. */
#define COMPACT_INFLIGHT_SLOTS (COMPACT_QUEUE_LEN + 1)
/* Combined old+new summary cap. Bumped 800 → 3072: with token-budget
 * compaction the batch being summarized can be ~100K tokens of a long task —
 * an 800-char summary would discard nearly all task state. */
#define COMPACT_SUMMARY_MAX   3072

/* Long-running-task oriented compaction: the summary is the agent's durable
 * working memory of everything it can no longer see verbatim, so it must keep
 * the operational state, not just chat gist. */
#define COMPACT_SYSTEM_PROMPT \
    "You are the long-term working-memory compactor for an embedded AI agent " \
    "running multi-step tasks. Rewrite the older conversation turns below into " \
    "a compact Chinese memo (≤500 characters) that lets the agent CONTINUE the " \
    "task without the original transcript. MUST preserve: open/unfinished " \
    "subtasks and next steps; decisions and their reasons; concrete facts the " \
    "agent discovered via tools (file paths, IDs, IPs, config values, numbers); " \
    "user constraints and stated preferences. DISCARD: greetings, acknowledgements, " \
    "and verbose phrasing. Output plain text only, no markdown, no preamble."

typedef struct {
    char *session_id;
} compact_job_t;

static QueueHandle_t  s_queue;
static rtos_mutex_t   s_file_mutex;          /* shared with claw_memory.c */
static rtos_mutex_t   s_summary_lock;        /* serializes LLM calls vs extract */
static rtos_mutex_t   s_dedup_lock;
static char           s_session_root[COMPACT_PATH_MAX];
/* Slots store the sid string of every queued-or-running job. Empty slot
 * = "". Lookup is O(N) but N is tiny (3). */
static char           s_inflight[COMPACT_INFLIGHT_SLOTS][COMPACT_SID_MAX];
static uint16_t       s_char_threshold;
static uint8_t        s_protect_last;
static int            s_initialized;

/* Helpers — caller MUST hold s_dedup_lock. */
static int inflight_find(const char *sid)
{
    for (int i = 0; i < COMPACT_INFLIGHT_SLOTS; i++) {
        if (s_inflight[i][0] && strcmp(s_inflight[i], sid) == 0) return i;
    }
    return -1;
}

static int inflight_reserve(const char *sid)
{
    for (int i = 0; i < COMPACT_INFLIGHT_SLOTS; i++) {
        if (!s_inflight[i][0]) {
            strncpy(s_inflight[i], sid, COMPACT_SID_MAX - 1);
            s_inflight[i][COMPACT_SID_MAX - 1] = '\0';
            return i;
        }
    }
    return -1;   /* no free slot — caller treats as "queue is at capacity" */
}

static void inflight_release(const char *sid)
{
    int idx = inflight_find(sid);
    if (idx >= 0) s_inflight[idx][0] = '\0';
}

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > CLAW_MEMORY_MAX_FILE_SIZE) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int write_file(const char *path, const char *data)
{
    FILE *f = fopen(path, "w");
    if (!f) return RTK_FAIL;
    size_t len = strlen(data);
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return (w == len) ? RTK_SUCCESS : RTK_FAIL;
}

/* Concatenate older turns into "User: ...\nAssistant: ...\n\n..." form. */
static char *render_older_turns(cJSON *turns, int older_count)
{
    size_t cap = 4096;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    size_t pos = 0;
    buf[0] = '\0';

    for (int i = 0; i < older_count; i++) {
        cJSON *t = cJSON_GetArrayItem(turns, i);
        if (!t) continue;
        cJSON *u = cJSON_GetObjectItem(t, "user");
        cJSON *a = cJSON_GetObjectItem(t, "assistant");
        const char *us = (u && cJSON_IsString(u)) ? u->valuestring : "";
        const char *as = (a && cJSON_IsString(a)) ? a->valuestring : "";

        size_t need = strlen(us) + strlen(as) + 32;
        if (pos + need >= cap) {
            size_t ncap = cap * 2;
            while (ncap < pos + need) ncap *= 2;
            char *nbuf = realloc(buf, ncap);
            if (!nbuf) { free(buf); return NULL; }
            buf = nbuf;
            cap = ncap;
        }
        pos += snprintf(buf + pos, cap - pos, "User: %s\nAssistant: %s\n\n",
                        us, as);
    }
    return buf;
}

/* Strip leading/trailing whitespace in place; returns the trimmed string. */
static char *trim(char *s)
{
    if (!s) return s;
    while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\n' ||
                     s[n-1] == '\r' || s[n-1] == '\t')) {
        s[--n] = '\0';
    }
    return s;
}

/* ------------------------------------------------------------------ */
/* startup .bak rollback                                               */
/* ------------------------------------------------------------------ */

static void rollback_stale_bak_files(const char *root)
{
    void *dir = opendir(root);
    if (!dir) return;

    struct dirent *ent;
    int rolled_back = 0, removed = 0;
    char bak_path[COMPACT_PATH_MAX];
    char tgt_path[COMPACT_PATH_MAX];
    while ((ent = readdir(dir)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen <= 4) continue;
        if (strcmp(ent->d_name + nlen - 4, ".bak") != 0) continue;

        snprintf(bak_path, sizeof(bak_path), "%s/%s", root, ent->d_name);
        snprintf(tgt_path, sizeof(tgt_path), "%s/%.*s",
                 root, (int)(nlen - 4), ent->d_name);

        /* A stale .bak can come from two distinct crash points:
         *  1) Crashed AFTER rename(.json,.bak) but BEFORE write_file. The
         *     new .json doesn't exist yet → rollback by renaming back.
         *  2) Crashed AFTER write_file but BEFORE remove(.bak). The new
         *     .json exists and is the good copy → just delete the .bak.
         *     This case is critical on FatFs, where rename-over-existing
         *     returns FR_EXIST instead of overwriting; without this
         *     branch every subsequent compact would fail until manual
         *     cleanup.
         */
        FILE *probe = fopen(tgt_path, "r");
        if (probe) {
            fclose(probe);
            if (remove(bak_path) == 0) {
                RTK_LOGI(TAG, "removed stale %s (new file is intact)\n",
                         ent->d_name);
                removed++;
            } else {
                RTK_LOGE(TAG, "could not remove stale %s\n", ent->d_name);
            }
        } else {
            if (rename(bak_path, tgt_path) == 0) {
                RTK_LOGW(TAG, "rolled back %s\n", ent->d_name);
                rolled_back++;
            } else {
                RTK_LOGE(TAG, "rollback failed for %s\n", ent->d_name);
            }
        }
    }
    closedir(dir);
    if (rolled_back || removed) {
        RTK_LOGI(TAG, "startup recovery: %d rolled back, %d cleaned\n",
                 rolled_back, removed);
    }
}

/* ------------------------------------------------------------------ */
/* core compaction routine                                             */
/* ------------------------------------------------------------------ */

static int call_summary_llm(const char *prev_summary,
                            const char *older_text,
                            char **out_summary)
{
    *out_summary = NULL;

    cJSON *messages = cJSON_CreateArray();
    if (!messages) return RTK_ERR_NOMEM;

    /* Build a single user message with prior summary (if any) + older turns. */
    size_t need = strlen(older_text) + (prev_summary ? strlen(prev_summary) : 0) + 256;
    char *prompt = malloc(need);
    if (!prompt) { cJSON_Delete(messages); return RTK_ERR_NOMEM; }
    if (prev_summary && prev_summary[0]) {
        snprintf(prompt, need,
                 "Existing summary of even older turns:\n%s\n\n"
                 "Newer-but-still-old turns to fold in:\n%s",
                 prev_summary, older_text);
    } else {
        snprintf(prompt, need,
                 "Older turns to summarize:\n%s", older_text);
    }

    cJSON *msg = cJSON_CreateObject();
    if (!msg) { free(prompt); cJSON_Delete(messages); return RTK_ERR_NOMEM; }
    cJSON_AddStringToObject(msg, "role",    "user");
    cJSON_AddStringToObject(msg, "content", prompt);
    cJSON_AddItemToArray(messages, msg);
    free(prompt);

    llm_resp_t resp = {0};
    char *err = NULL;

    claw_memory_summary_lock_take();
    int rc = claw_agent_llm_chat_messages(COMPACT_SYSTEM_PROMPT, messages,
                                          NULL, &resp, &err);
    claw_memory_summary_lock_give();

    cJSON_Delete(messages);

    if (rc != RTK_SUCCESS || !resp.reply || !resp.reply[0]) {
        RTK_LOGW(TAG, "summary LLM failed: %s\n", err ? err : "(no detail)");
        free(err);
        claw_agent_llm_response_free(&resp);
        return RTK_FAIL;
    }

    char *out = strdup(resp.reply);
    free(err);
    claw_agent_llm_response_free(&resp);
    if (!out) return RTK_ERR_NOMEM;
    trim(out);
    *out_summary = out;
    return RTK_SUCCESS;
}

/* === do_compact — 3-window pipeline ==================================
 *
 *   Window A (file_mutex held, ~30 ms)
 *      slurp + parse session file, compute split, render older_text,
 *      capture boundary fingerprint (the LAST older turn's text prefix)
 *      so window C can locate the correct cut point even after the
 *      ring buffer shifts indices, and copy prev_summary. Release lock.
 *
 *   Window B (no file_mutex, ~5–10 s)
 *      LLM summarization. summary_lock serializes TLS handshakes
 *      against mem_extract.
 *
 *   Window C (file_mutex held, ~50 ms)
 *      Re-read the file (which may have been appended to and ring-
 *      trimmed during window B), find the boundary by positional check
 *      (turn at new_total - protect_last - 1) with fingerprint
 *      verification, fall back to linear search, cap at max_deletable.
 *      Atomic .bak write.
 *
 * On any error after window B starts, summary is silently discarded;
 * the next append turn will re-trigger because total_chars is still
 * above the threshold. NEVER do an unsafe write_file just to mark
 * pending — that could lose the entire session on power loss.
 */
static void do_compact(const char *sid)
{
    char fpath[COMPACT_PATH_MAX];
    char bak_path[COMPACT_PATH_MAX];
    char *raw = NULL;
    cJSON *root = NULL;
    char *new_summary = NULL;
    char *older_text = NULL;
    char *prev_summary_copy = NULL;
    char *boundary_fp = NULL;
    char  fp_origin = 'A';   /* 'A'=assistant text, 'U'=user text */
    int   older_count_at_start = 0;
    char *combined = NULL;
    char *serialized = NULL;
    int compacted_n = 0;

    claw_memory_session_file_path(sid, fpath, sizeof(fpath));
    snprintf(bak_path, sizeof(bak_path), "%s.bak", fpath);

    /* === Window A — read snapshot under file_mutex, brief hold === */
    rtos_mutex_take(s_file_mutex, 0xFFFFFFFFUL);

    raw = slurp(fpath);
    if (!raw) {
        rtos_mutex_give(s_file_mutex);
        RTK_LOGI(TAG, "sid=%s no/empty/oversize file, skip\n", sid);
        return;
    }
    root = cJSON_Parse(raw);
    free(raw); raw = NULL;
    if (!root) {
        rtos_mutex_give(s_file_mutex);
        RTK_LOGW(TAG, "sid=%s bad JSON, skip\n", sid);
        return;
    }

    cJSON *turns = cJSON_GetObjectItem(root, "turns");
    if (!turns || !cJSON_IsArray(turns)) {
        rtos_mutex_give(s_file_mutex);
        cJSON_Delete(root);
        RTK_LOGW(TAG, "sid=%s no turns array, skip\n", sid);
        return;
    }
    int total_turns = cJSON_GetArraySize(turns);
    older_count_at_start = total_turns - (int)s_protect_last;
    if (older_count_at_start < 2) {
        rtos_mutex_give(s_file_mutex);
        cJSON_Delete(root);
        RTK_LOGI(TAG, "sid=%s only %d compactable turn(s), skip\n",
                 sid, older_count_at_start);
        return;
    }

    older_text = render_older_turns(turns, older_count_at_start);
    if (!older_text) {
        rtos_mutex_give(s_file_mutex);
        cJSON_Delete(root);
        RTK_LOGE(TAG, "sid=%s OOM rendering older turns\n", sid);
        return;
    }

    /* Capture fingerprint of the LAST older turn so window C can locate
     * the cut point even if the ring buffer evicted some of our older
     * turns during the unlocked LLM call. Prefer assistant text (longer,
     * more unique); fall back to user. If neither exists, leave fp NULL
     * and window C will fall back to count-based delete. */
    cJSON *boundary = cJSON_GetArrayItem(turns, older_count_at_start - 1);
    if (boundary) {
        cJSON *bA = cJSON_GetObjectItem(boundary, "assistant");
        cJSON *bU = cJSON_GetObjectItem(boundary, "user");
        const char *src = NULL;
        if (bA && cJSON_IsString(bA) && bA->valuestring[0]) {
            src = bA->valuestring; fp_origin = 'A';
        } else if (bU && cJSON_IsString(bU) && bU->valuestring[0]) {
            src = bU->valuestring; fp_origin = 'U';
        }
        if (src) {
            size_t slen = strlen(src);
            size_t flen = (slen > 64) ? 64 : slen;
            boundary_fp = malloc(flen + 1);
            if (boundary_fp) {
                memcpy(boundary_fp, src, flen);
                boundary_fp[flen] = '\0';
            }
        }
    }

    cJSON *prev = cJSON_GetObjectItem(root, "compaction_summary");
    if (prev && cJSON_IsString(prev) && prev->valuestring[0]) {
        prev_summary_copy = strdup(prev->valuestring);
    } else {
        prev_summary_copy = strdup("");
    }

    cJSON_Delete(root); root = NULL;
    rtos_mutex_give(s_file_mutex);

    if (!prev_summary_copy) {
        RTK_LOGE(TAG, "sid=%s OOM copying prev summary\n", sid);
        goto cleanup_no_lock;
    }

    /* === Window B — LLM call without holding file_mutex === */
    if (call_summary_llm(prev_summary_copy, older_text, &new_summary) != RTK_SUCCESS
            || !new_summary || !new_summary[0]) {
        RTK_LOGW(TAG, "sid=%s compact failed at LLM step, will retry on next append\n", sid);
        goto cleanup_no_lock;
    }

    /* Combine prev + new summary, cap at COMPACT_SUMMARY_MAX. */
    {
        size_t old_len = strlen(prev_summary_copy);
        size_t new_len = strlen(new_summary);
        if (old_len == 0) {
            combined = strdup(new_summary);
        } else if (old_len + 2 + new_len <= COMPACT_SUMMARY_MAX) {
            combined = malloc(old_len + 2 + new_len + 1);
            if (combined) {
                snprintf(combined, old_len + 2 + new_len + 1, "%s\n\n%s",
                         prev_summary_copy, new_summary);
            }
        } else {
            combined = strdup(new_summary);
            RTK_LOGI(TAG, "sid=%s combined summary > %d chars, kept new only\n",
                     sid, COMPACT_SUMMARY_MAX);
        }
    }
    if (!combined) {
        RTK_LOGE(TAG, "sid=%s OOM combining summaries\n", sid);
        goto cleanup_no_lock;
    }

    /* === Window C — re-take lock, locate cut, write atomically === */
    rtos_mutex_take(s_file_mutex, 0xFFFFFFFFUL);

    raw = slurp(fpath);
    if (!raw) {
        rtos_mutex_give(s_file_mutex);
        RTK_LOGW(TAG, "sid=%s file vanished after LLM, dropping summary\n", sid);
        goto cleanup_no_lock;
    }
    root = cJSON_Parse(raw);
    free(raw); raw = NULL;
    if (!root) {
        rtos_mutex_give(s_file_mutex);
        RTK_LOGW(TAG, "sid=%s bad JSON in window C, dropping summary\n", sid);
        goto cleanup_no_lock;
    }
    turns = cJSON_GetObjectItem(root, "turns");
    if (!turns || !cJSON_IsArray(turns)) {
        rtos_mutex_give(s_file_mutex);
        RTK_LOGW(TAG, "sid=%s no turns array in window C\n", sid);
        cJSON_Delete(root); root = NULL;
        goto cleanup_no_lock;
    }

    int new_total = cJSON_GetArraySize(turns);

    /* Decide how many head turns to delete. The LAST `protect_last` turns
     * are NEVER touched, so the maximum cut is new_total - protect_last.
     *
     * Strategy:
     *   1) Position the boundary at index (new_total - protect_last - 1),
     *      which by the protect_last invariant is "newest non-protected".
     *      Verify with fingerprint — if it matches, take that count.
     *   2) Otherwise scan [0, max_deletable) for the fingerprint
     *      (handles the rare case of cross-turn collision or ring shift).
     *   3) If still no match, the original boundary turn was evicted by
     *      the ring buffer during window B; delete_count=0 and we just
     *      persist the summary (covering content the user can't see
     *      anyway). */
    int max_deletable = new_total - (int)s_protect_last;
    if (max_deletable < 0) max_deletable = 0;
    int delete_count = 0;

    if (boundary_fp && boundary_fp[0] && max_deletable > 0) {
        size_t fp_len = strlen(boundary_fp);
        const char *fp_field = (fp_origin == 'U') ? "user" : "assistant";

        cJSON *expected = cJSON_GetArrayItem(turns, max_deletable - 1);
        cJSON *fe = expected ? cJSON_GetObjectItem(expected, fp_field) : NULL;
        if (fe && cJSON_IsString(fe) && fe->valuestring[0] &&
            strncmp(fe->valuestring, boundary_fp, fp_len) == 0) {
            delete_count = max_deletable;   /* fast path */
        } else {
            for (int i = 0; i < max_deletable; i++) {
                cJSON *t = cJSON_GetArrayItem(turns, i);
                if (!t) continue;
                cJSON *tf = cJSON_GetObjectItem(t, fp_field);
                if (tf && cJSON_IsString(tf) && tf->valuestring[0] &&
                    strncmp(tf->valuestring, boundary_fp, fp_len) == 0) {
                    delete_count = i + 1;
                    break;
                }
            }
        }
    } else if (max_deletable > 0) {
        /* No fingerprint captured (OOM during window A). Best effort. */
        delete_count = (older_count_at_start < max_deletable)
                       ? older_count_at_start : max_deletable;
    }

    if (delete_count == 0) {
        RTK_LOGI(TAG, "sid=%s ring buffer already evicted compacted turns; "
                      "writing summary only (new_total=%d)\n", sid, new_total);
    } else {
        for (int i = 0; i < delete_count; i++) {
            cJSON_DeleteItemFromArray(turns, 0);
        }
    }
    compacted_n = delete_count;

    /* Update summary fields. compacted_turn_count tracks turns the LLM
     * has summarized over (older_count_at_start), not turns deleted in
     * this round (delete_count) — they may differ if the ring buffer
     * already removed some during window B. */
    cJSON_DeleteItemFromObject(root, "compaction_summary");
    cJSON_AddStringToObject(root, "compaction_summary", combined);

    cJSON_DeleteItemFromObject(root, "compaction_summary_chars");
    cJSON_AddNumberToObject(root, "compaction_summary_chars",
                            (double)strlen(combined));

    cJSON *jcount_prev = cJSON_GetObjectItem(root, "compacted_turn_count");
    int prev_count = (jcount_prev && cJSON_IsNumber(jcount_prev))
                     ? (int)jcount_prev->valuedouble : 0;
    cJSON_DeleteItemFromObject(root, "compacted_turn_count");
    cJSON_AddNumberToObject(root, "compacted_turn_count",
                            (double)(prev_count + older_count_at_start));

    cJSON_DeleteItemFromObject(root, "compaction_pending");
    cJSON_AddBoolToObject(root, "compaction_pending", 0);

    /* Bump the compaction generation so the next request's context provider
     * can tell the LLM "older turns were just summarized" (P4 after-banner). */
    {
        cJSON *jgen = cJSON_GetObjectItem(root, "compaction_generation");
        int gen = (jgen && cJSON_IsNumber(jgen)) ? (int)jgen->valuedouble : 0;
        cJSON_DeleteItemFromObject(root, "compaction_generation");
        cJSON_AddNumberToObject(root, "compaction_generation", (double)(gen + 1));
    }

    serialized = cJSON_PrintUnformatted(root);
    if (!serialized) {
        rtos_mutex_give(s_file_mutex);
        RTK_LOGE(TAG, "sid=%s serialize OOM\n", sid);
        goto cleanup_no_lock;
    }

    /* Atomic write via .bak.
     *  - Pre-clean any stale .bak: on FatFs, rename-over-existing returns
     *    FR_EXIST and the rename below would otherwise fail forever after
     *    a prior crashed compact.
     *  - rename(.json, .bak) → write_file(.json, new) → remove(.bak).
     *  - On write failure: rename(.bak, .json) rollback.
     */
    (void)remove(bak_path);
    if (rename(fpath, bak_path) != 0) {
        rtos_mutex_give(s_file_mutex);
        RTK_LOGE(TAG, "sid=%s rename to .bak failed\n", sid);
        goto cleanup_no_lock;
    }
    if (write_file(fpath, serialized) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "sid=%s write new file failed, rolling back\n", sid);
        if (rename(bak_path, fpath) != 0) {
            RTK_LOGE(TAG, "sid=%s rollback rename failed too! "
                          "data state inconsistent\n", sid);
        }
        rtos_mutex_give(s_file_mutex);
        goto cleanup_no_lock;
    }
    if (remove(bak_path) != 0) {
        RTK_LOGW(TAG, "sid=%s could not remove .bak (cosmetic — "
                      "startup recovery will clean it up)\n", sid);
    }

    RTK_LOGI(TAG, "sid=%s compacted %d→%d turns "
                  "(summarized=%d, deleted=%d), summary=%uB\n",
             sid, new_total, new_total - compacted_n,
             older_count_at_start, compacted_n,
             (unsigned)strlen(combined));

    rtos_mutex_give(s_file_mutex);

cleanup_no_lock:
    free(older_text);
    free(new_summary);
    free(prev_summary_copy);
    free(boundary_fp);
    free(combined);
    free(serialized);
    if (root) cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
/* task / queue                                                        */
/* ------------------------------------------------------------------ */

static void compact_task(void *arg)
{
    (void)arg;
    compact_job_t job;
    while (1) {
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        do_compact(job.session_id);

        /* Release this sid's slot. Multiple slots may match if a duplicate
         * enqueue ever slipped through (it cannot under the current dedup
         * — we look up across ALL slots — but inflight_release is
         * idempotent and releases the first match only, which is fine). */
        rtos_mutex_take(s_dedup_lock, 0xFFFFFFFFUL);
        inflight_release(job.session_id);
        rtos_mutex_give(s_dedup_lock);

        free(job.session_id);
    }
}

void claw_memory_compact_enqueue(const char *session_id)
{
    if (!s_initialized || !session_id || !session_id[0]) return;
    if (s_char_threshold == 0) return;

    /* Reject sids longer than our slot — they could not be tracked
     * accurately for dedup. djb2-hashed session_ids are bounded so
     * this is defensive only. */
    if (strlen(session_id) >= COMPACT_SID_MAX) {
        RTK_LOGW(TAG, "session_id too long, dropping\n");
        return;
    }

    /* Allocate sid copy outside the lock to keep the critical section short. */
    char *sid_copy = strdup(session_id);
    if (!sid_copy) return;

    /* Hold dedup_lock across BOTH the inflight check AND the enqueue:
     * splitting them (as the original single-slot version did) leaves a
     * window where a parallel enqueue can be deduplicated against a
     * slot that was reserved but never made it onto the queue. The
     * multi-slot table closes a separate hole — single-slot tracking
     * was clobbered by every new sid, so distinct-sid interleaving
     * could re-admit a duplicate of the running sid. */
    rtos_mutex_take(s_dedup_lock, 0xFFFFFFFFUL);

    if (inflight_find(session_id) >= 0) {
        rtos_mutex_give(s_dedup_lock);
        free(sid_copy);
        return;
    }

    if (inflight_reserve(session_id) < 0) {
        /* All slots busy — this means QUEUE_LEN+1 different sids are
         * already in flight or queued. Treat as queue-full. */
        rtos_mutex_give(s_dedup_lock);
        RTK_LOGW(TAG, "all inflight slots busy, dropping sid=%s\n", session_id);
        free(sid_copy);
        return;
    }

    compact_job_t job = { .session_id = sid_copy };
    if (xQueueSend(s_queue, &job, 0) != pdTRUE) {
        /* xQueueSend should not fail with available slot since we sized
         * COMPACT_INFLIGHT_SLOTS = QUEUE_LEN + 1, but be defensive. */
        inflight_release(session_id);
        rtos_mutex_give(s_dedup_lock);
        RTK_LOGW(TAG, "queue full, dropping sid=%s\n", session_id);
        free(sid_copy);
        return;
    }

    rtos_mutex_give(s_dedup_lock);
}

void claw_memory_summary_lock_take(void)
{
    if (!s_initialized) return;
    rtos_mutex_take(s_summary_lock, 0xFFFFFFFFUL);
}

void claw_memory_summary_lock_give(void)
{
    if (!s_initialized) return;
    rtos_mutex_give(s_summary_lock);
}

/* ------------------------------------------------------------------ */
/* compaction_summary provider                                         */
/* ------------------------------------------------------------------ */

static int collect_compaction_summary(const claw_agent_request_t *request,
                                      claw_agent_context_t *out_context,
                                      void *user_ctx)
{
    (void)user_ctx;
    if (!s_initialized || !request || !request->session_id) return RTK_FAIL;

    char fpath[COMPACT_PATH_MAX];
    claw_memory_session_file_path(request->session_id, fpath, sizeof(fpath));

    rtos_mutex_take(s_file_mutex, 0xFFFFFFFFUL);
    char *raw = slurp(fpath);
    rtos_mutex_give(s_file_mutex);
    if (!raw) return RTK_FAIL;

    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) return RTK_FAIL;

    /* P4: notify the LLM around compaction.
     *  - BEFORE banner: when the last request's real prompt_tokens reached ≥85%
     *    of the compaction threshold, warn that older turns are about to be
     *    summarized so the agent can persist critical state proactively. This
     *    fires even before the first compaction (no summary yet).
     *  - AFTER banner: whenever a lossy summary exists, tell the agent the early
     *    transcript was condensed and how to retrieve specifics. */
    uint32_t compact_tokens = claw_config_get()->llm.compact_tokens;
    cJSON *jlpt = cJSON_GetObjectItem(root, "last_prompt_tokens");
    uint32_t lpt = (jlpt && cJSON_IsNumber(jlpt)) ? (uint32_t)jlpt->valuedouble : 0;
    int warn_before = (compact_tokens > 0 && lpt > 0 &&
                       lpt >= (compact_tokens / 100 * 85));

    cJSON *jsum = cJSON_GetObjectItem(root, "compaction_summary");
    int have_summary = (jsum && cJSON_IsString(jsum) && jsum->valuestring[0]);

    if (!warn_before && !have_summary) {
        cJSON_Delete(root);
        return RTK_FAIL;
    }

    size_t sum_len = have_summary ? strlen(jsum->valuestring) : 0;
    size_t buf_size = sum_len + 768;
    char *buf = malloc(buf_size);
    if (!buf) { cJSON_Delete(root); return RTK_FAIL; }

    size_t pos = 0;
    if (warn_before) {
        pos += snprintf(buf + pos, buf_size - pos,
            "## Context Budget Warning\n"
            "当前上下文约 %u tokens，已接近上限，较早的对话即将被有损压缩为摘要。"
            "若后续仍需要早期细节，请立即用 memory_store 落盘关键信息，"
            "或在本轮回复中显式记录。\n\n",
            (unsigned)lpt);
    }
    if (have_summary) {
        pos += snprintf(buf + pos, buf_size - pos,
            "## Earlier Conversation Summary\n"
            "较早的对话已被压缩为以下有损摘要；需要精确的早期信息请用 memory_recall "
            "或请用户重述。\n%s\n", jsum->valuestring);
    }
    cJSON_Delete(root);

    out_context->kind    = CLAW_AGENT_CONTEXT_KIND_SYSTEM_PROMPT;
    out_context->content = buf;
    return RTK_SUCCESS;
}

claw_agent_context_provider_t claw_memory_compaction_summary_provider = {
    .name       = "memory_compaction_summary",
    .collect    = collect_compaction_summary,
    .user_ctx   = NULL,
    .quiet_skip = true,  /* skips until a compaction summary / budget warning exists — expected */
};

/* ------------------------------------------------------------------ */
/* init                                                                */
/* ------------------------------------------------------------------ */

int claw_memory_compact_init(const char *session_root,
                             rtos_mutex_t file_mutex,
                             uint16_t char_threshold,
                             uint8_t protect_last)
{
    if (s_initialized) return RTK_SUCCESS;
    if (!session_root || !session_root[0]) return RTK_ERR_BADARG;

    strncpy(s_session_root, session_root, sizeof(s_session_root) - 1);
    s_session_root[sizeof(s_session_root) - 1] = '\0';
    s_file_mutex      = file_mutex;
    s_char_threshold  = char_threshold;
    s_protect_last    = (protect_last > 0) ? protect_last : 4;
    memset(s_inflight, 0, sizeof(s_inflight));
    s_summary_lock    = NULL;
    s_dedup_lock      = NULL;
    s_queue           = NULL;

    if (rtos_mutex_create(&s_summary_lock) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "summary_lock create failed\n");
        s_summary_lock = NULL;
        return RTK_ERR_NOMEM;
    }
    if (rtos_mutex_create(&s_dedup_lock) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "dedup_lock create failed\n");
        rtos_mutex_delete(s_summary_lock);
        s_summary_lock = NULL;
        s_dedup_lock   = NULL;
        return RTK_ERR_NOMEM;
    }

    /* Recover any *.json.bak left from a crashed prior compact run. */
    rollback_stale_bak_files(s_session_root);

    /* If compaction is disabled we still expose summary_lock + provider, but
     * we never spawn the worker task — saves the 32 KB stack. */
    if (s_char_threshold == 0) {
        s_initialized = 1;
        RTK_LOGI(TAG, "initialized (compaction disabled, threshold=0)\n");
        return RTK_SUCCESS;
    }

    s_queue = xQueueCreate(COMPACT_QUEUE_LEN, sizeof(compact_job_t));
    if (!s_queue) {
        RTK_LOGE(TAG, "queue create failed\n");
        rtos_mutex_delete(s_summary_lock);
        rtos_mutex_delete(s_dedup_lock);
        s_summary_lock = NULL;
        s_dedup_lock   = NULL;
        return RTK_ERR_NOMEM;
    }
    if (rtos_task_create(NULL, "mem_compact", compact_task, NULL,
                         COMPACT_STACK_SIZE, 1) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "task create failed\n");
        vQueueDelete(s_queue);
        rtos_mutex_delete(s_summary_lock);
        rtos_mutex_delete(s_dedup_lock);
        s_queue        = NULL;
        s_summary_lock = NULL;
        s_dedup_lock   = NULL;
        return RTK_FAIL;
    }

    s_initialized = 1;
    RTK_LOGI(TAG, "initialized (root=%s threshold=%uB protect_last=%u)\n",
             s_session_root, (unsigned)s_char_threshold,
             (unsigned)s_protect_last);
    return RTK_SUCCESS;
}

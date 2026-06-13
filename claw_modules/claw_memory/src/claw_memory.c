/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "claw_memory.h"
#include "claw_memory_compact.h"
#include "ameba_claw_defs.h"
#include "claw_cap.h"
#include "claw_compat.h"
#include "claw_config.h"
#include <cJSON.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include "vfs.h"

#define TAG "claw_memory"
#define CLAW_MEMORY_MAX_PATH 192

/* Per-turn cap on the serialized tool round-trip blob (CLAW_MEMORY_TOOLMSGS_PER_TURN_MAX)
 * is defined in ameba_claw_defs.h, sized relative to TOOL_RESULT_MAX_BYTES so a
 * single full-size tool result survives cross-turn replay. */

/* Soft ceiling for the whole serialized session file. Kept below
 * CLAW_MEMORY_MAX_FILE_SIZE (256 KB) — slurp_file refuses to read anything
 * larger, which would silently drop the session. If a write would cross this,
 * tool_msgs are shed from the oldest turns (oldest cache value, first to be
 * compacted away anyway) until it fits. */
#define CLAW_MEMORY_SESSION_SOFT_MAX (CLAW_MEMORY_MAX_FILE_SIZE - 2048)

/* Forward declaration — defined at bottom of file after cap descriptors */
static void claw_memory_register_caps(void);

/* Forward declarations for long-term store helpers used by S1 label provider
 * (which is positioned above the CRUD section). */
static cJSON *lt_load(void);
static void   lt_rollback_stale_bak(void);

/* ---- State ---- */

typedef struct {
    char memory_root[CLAW_MEMORY_MAX_PATH];
    char session_root[CLAW_MEMORY_MAX_PATH];
    char profile_root[CLAW_MEMORY_MAX_PATH];  /* base dir for AGENTS/SOUL/IDENTITY/USER/MEMORY.md */
    size_t max_session_turns;
    uint8_t  compaction_protect_last;     /* S2 — # trailing turns kept verbatim */
    uint32_t compaction_token_threshold;  /* token-budget trigger (preferred) */
    uint32_t context_window_tokens;       /* hard ceiling for synchronous trim */
    rtos_mutex_t mutex;      /* protects in-memory data structures only (no I/O held) */
    rtos_mutex_t file_mutex; /* serialises session-file I/O; shared with compact task */
    bool initialized;
} claw_memory_state_t;

static claw_memory_state_t s_mem = {0};

/* ---- Utility: read file into malloc'd buffer ---- */

static char *slurp_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > CLAW_MEMORY_MAX_FILE_SIZE) { fclose(f); return NULL; }
    char *buf = rtos_mem_malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* ---- Utility: ensure file exists, create with default content if not ---- */

static void create_if_missing(const char *path, const char *default_content)
{
    FILE *f = fopen(path, "r");
    if (f) { fclose(f); return; }
    f = fopen(path, "w");
    if (f) {
        if (default_content && default_content[0]) {
            size_t len = strlen(default_content);
            fwrite(default_content, 1, len, f);
            fflush(f);
        }
        fclose(f);
    }
}

/* ---- Utility: mkdir, ignore EEXIST ---- */
/* VFS mkdir returns LFS error codes directly (not POSIX errno).
 * LFS_ERR_EXIST = -17.  Check the return value, not errno. */
#define MKDIR_ERR_EXIST  (-17)

static void mkdir_if_needed(const char *path)
{
    int ret = mkdir(path, 0755);
    if (ret != 0 && ret != MKDIR_ERR_EXIST) {
        RTK_LOGW(TAG, "mkdir(%s) failed: %d\n", path, ret);
    }
}

/* ---- Utility: session file path ---- */

static void session_file_path(const char *session_id, char *out, size_t out_size)
{
    char sanitized[33];
    size_t i;
    const char *src = session_id ? session_id : "default";
    size_t src_len = strlen(src);

    /* Copy up to 32 chars, replacing ':' and '/' with '_' */
    for (i = 0; i < 32 && i < src_len; i++) {
        char c = src[i];
        if (c == ':' || c == '/') c = '_';
        sanitized[i] = c;
    }
    sanitized[i] = '\0';

    /* djb2 hash of original session_id */
    uint32_t hash = 5381;
    const unsigned char *p = (const unsigned char *)src;
    while (*p) {
        hash = ((hash << 5) + hash) ^ (uint32_t)(*p);
        p++;
    }

    DiagSnPrintf(out, out_size, "%s/s_%s_%08lx.json", s_mem.session_root, sanitized, (unsigned long)hash);
}

/* Public wrapper so claw_memory_compact (and any future module) can resolve
 * paths without re-implementing the djb2+sanitize hash. */
void claw_memory_session_file_path(const char *session_id,
                                   char *out, size_t out_size)
{
    session_file_path(session_id, out, out_size);
}

/* ---- UTF-8 safe truncation ----
 *
 * Truncating a Chinese/emoji-bearing string at a fixed byte count can split a
 * multi-byte sequence and produce a leading byte with no continuation, which
 * downstream renderers display as a replacement character or garbage. This
 * helper walks src one UTF-8 codepoint at a time and stops at a clean
 * boundary ≤ max_bytes. Invalid lead bytes are treated as 1-byte (resync).
 */
size_t claw_memory_utf8_safe_copy(char *dst, size_t dst_size,
                                  const char *src, size_t max_bytes)
{
    if (!dst || dst_size == 0) return 0;
    if (!src) { dst[0] = '\0'; return 0; }

    size_t limit = (max_bytes < dst_size - 1) ? max_bytes : dst_size - 1;
    size_t out = 0;
    while (src[out] != '\0' && out < limit) {
        unsigned char c = (unsigned char)src[out];
        int seq_len;
        if      ((c & 0x80) == 0x00) seq_len = 1;
        else if ((c & 0xE0) == 0xC0) seq_len = 2;
        else if ((c & 0xF0) == 0xE0) seq_len = 3;
        else if ((c & 0xF8) == 0xF0) seq_len = 4;
        else                          seq_len = 1;

        if (out + (size_t)seq_len > limit) break;

        int k;
        for (k = 0; k < seq_len; k++) {
            if (src[out + k] == '\0') {
                dst[out] = '\0';
                return out;
            }
            dst[out + k] = src[out + k];
        }
        out += (size_t)seq_len;
    }
    dst[out] = '\0';
    return out;
}

size_t claw_memory_utf8_safe_copy_marked(char *dst, size_t dst_size,
                                         const char *src, size_t max_bytes)
{
    /* "…[截断]" — U+2026 (3B) + "[截断]" (8B) = 11 bytes, NOT counting NUL. */
    static const char MARK[] = "\xE2\x80\xA6[\xE6\x88\xAA\xE6\x96\xAD]";
    const size_t mark_len = sizeof(MARK) - 1;

    if (!dst || dst_size == 0) return 0;
    if (!src) { dst[0] = '\0'; return 0; }

    size_t limit = (max_bytes < dst_size - 1) ? max_bytes : dst_size - 1;

    /* Fits within the limit — no truncation, no marker. */
    if (strlen(src) <= limit)
        return claw_memory_utf8_safe_copy(dst, dst_size, src, limit);

    /* Truncating: reserve room for the marker, then append it. The marker is
     * pure ASCII-bracketed CJK and never split, so the result stays valid
     * UTF-8 regardless of where the source boundary landed. */
    size_t budget = (limit > mark_len) ? (limit - mark_len) : 0;
    size_t out = claw_memory_utf8_safe_copy(dst, dst_size, src, budget);
    memcpy(dst + out, MARK, mark_len);   /* out + mark_len <= limit <= dst_size-1 */
    dst[out + mark_len] = '\0';
    return out + mark_len;
}

/* ---- claw_memory_init ---- */

int claw_memory_init(const claw_memory_config_t *config)
{
    char path[CLAW_MEMORY_MAX_PATH];

    if (s_mem.initialized) return RTK_SUCCESS;
    if (!config || !config->memory_root_dir || !config->session_root_dir) {
        return RTK_ERR_BADARG;
    }

    /* Save paths */
    strlcpy(s_mem.memory_root, config->memory_root_dir, sizeof(s_mem.memory_root));
    strlcpy(s_mem.session_root, config->session_root_dir, sizeof(s_mem.session_root));
    strlcpy(s_mem.profile_root,
            (config->profile_root_dir && config->profile_root_dir[0])
                ? config->profile_root_dir : config->memory_root_dir,
            sizeof(s_mem.profile_root));
    s_mem.max_session_turns = (config->max_session_turns > 0) ? config->max_session_turns : 20;

    /* S2 compaction tuning. */
    s_mem.compaction_protect_last   = (config->compaction_protect_last > 0)
                                      ? config->compaction_protect_last : 4;
    s_mem.compaction_token_threshold = (config->compaction_token_threshold > 0)
                                      ? config->compaction_token_threshold : 110000;
    s_mem.context_window_tokens     = (config->context_window_tokens > 0)
                                      ? config->context_window_tokens : 128000;

    /* Create directories */
    mkdir_if_needed(s_mem.memory_root);
    mkdir_if_needed(s_mem.session_root);

    /* Ensure profile files exist (all uppercase, at profile_root) */
    DiagSnPrintf(path, sizeof(path), "%s/AGENTS.md", s_mem.profile_root);
    create_if_missing(path, "You are an intelligent embedded AI assistant running on RTL8721F.\n");

    DiagSnPrintf(path, sizeof(path), "%s/SOUL.md", s_mem.profile_root);
    create_if_missing(path, "");

    DiagSnPrintf(path, sizeof(path), "%s/IDENTITY.md", s_mem.profile_root);
    create_if_missing(path, "");

    DiagSnPrintf(path, sizeof(path), "%s/USER.md", s_mem.profile_root);
    create_if_missing(path, "");

    DiagSnPrintf(path, sizeof(path), "%s/MEMORY.md", s_mem.profile_root);
    create_if_missing(path, "");

    /* Create mutexes:
     *   mutex      — protects in-memory state only; never held across I/O.
     *   file_mutex — serialises session-file read/write; shared with compact
     *                task so append and compaction never race on the same file. */
    int err = rtos_mutex_create(&s_mem.mutex);
    if (err != RTK_SUCCESS) {
        RTK_LOGE(TAG, "Failed to create mutex\n");
        return RTK_ERR_NOMEM;
    }
    err = rtos_mutex_create(&s_mem.file_mutex);
    if (err != RTK_SUCCESS) {
        RTK_LOGE(TAG, "Failed to create file_mutex\n");
        rtos_mutex_delete(s_mem.mutex);
        return RTK_ERR_NOMEM;
    }

    s_mem.initialized = true;
    claw_memory_register_caps();

    /* Roll back any LT store *.bak left from a crashed prior write. */
    lt_rollback_stale_bak();

    /* Spin up async compaction; pass file_mutex so compact and append
     * coordinate session-file access through the same lock. */
    (void)claw_memory_compact_init(s_mem.session_root,
                                   s_mem.file_mutex,
                                   1,
                                   s_mem.compaction_protect_last);

    RTK_LOGI(TAG, "Initialized (memory=%s session=%s)\n", s_mem.memory_root, s_mem.session_root);
    return RTK_SUCCESS;
}

const char *claw_memory_get_session_root(void)
{
    return s_mem.initialized ? s_mem.session_root : "";
}

/* ---- claw_memory_clear_session ---- */

int claw_memory_clear_session(const char *session_id)
{
    if (!s_mem.initialized) return -1;
    char *fpath = (char *)rtos_mem_malloc(CLAW_MEMORY_MAX_PATH);
    if (!fpath) return -1;
    session_file_path(session_id, fpath, CLAW_MEMORY_MAX_PATH);
    int rc = remove(fpath);
    if (rc == 0) {
        RTK_LOGI(TAG, "session cleared: %s\n", fpath);
    }
    rtos_mem_free(fpath);
    return rc;
}

/* ---- claw_memory_clear_all_sessions ---- */

int claw_memory_clear_all_sessions(void)
{
    if (!s_mem.initialized) return -1;
    void *dir = opendir(s_mem.session_root);
    if (!dir) return -1;

    char *fpath = (char *)rtos_mem_malloc(CLAW_MEMORY_MAX_PATH);
    if (!fpath) { closedir(dir); return -1; }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        /* Only delete session history files (s_*.json). Skip:
         *  - chat_map/ subdirectory owned by cap_session_mgr
         *  - .bak files from a crashed compact write (startup rollback repairs them)
         */
        if (ent->d_name[0] != 's' || ent->d_name[1] != '_') continue;
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6 || strcmp(ent->d_name + nlen - 5, ".json") != 0) continue;

        DiagSnPrintf(fpath, CLAW_MEMORY_MAX_PATH, "%s/%s", s_mem.session_root, ent->d_name);
        if (remove(fpath) == 0) count++;
    }
    closedir(dir);
    rtos_mem_free(fpath);
    RTK_LOGI(TAG, "cleared %d session file(s)\n", count);
    return count;
}

/* ---- claw_memory_clear_long_term ---- */

int claw_memory_clear_long_term(void)
{
    if (!s_mem.initialized) return -1;
    char *fpath = (char *)rtos_mem_malloc(CLAW_MEMORY_MAX_PATH);
    if (!fpath) return -1;
    DiagSnPrintf(fpath, CLAW_MEMORY_MAX_PATH, "%s/long_term_store.json", s_mem.memory_root);
    int rc = remove(fpath);
    rtos_mem_free(fpath);
    if (rc == 0) RTK_LOGI(TAG, "long-term store cleared\n");
    return rc;
}

/* ---- S2: count chars across compaction_summary + every turn ---- */

static size_t session_total_chars(cJSON *root)
{
    size_t total = 0;
    if (!root) return 0;

    cJSON *jsum = cJSON_GetObjectItem(root, "compaction_summary");
    if (jsum && cJSON_IsString(jsum)) total += strlen(jsum->valuestring);

    cJSON *turns = cJSON_GetObjectItem(root, "turns");
    if (turns && cJSON_IsArray(turns)) {
        cJSON *t;
        cJSON_ArrayForEach(t, turns) {
            cJSON *u = cJSON_GetObjectItem(t, "user");
            cJSON *a = cJSON_GetObjectItem(t, "assistant");
            cJSON *tm = cJSON_GetObjectItem(t, "tool_msgs");
            if (u && cJSON_IsString(u)) total += strlen(u->valuestring);
            if (a && cJSON_IsString(a)) total += strlen(a->valuestring);
            /* Tool round-trips count toward the compaction threshold so a
             * tool-heavy session compacts before the file nears 32 KB. */
            if (tm && cJSON_IsArray(tm)) {
                char *s = cJSON_PrintUnformatted(tm);
                if (s) { total += strlen(s); cJSON_free(s); }
            }
        }
    }
    return total;
}

/* ---- claw_memory_append_session_turn ---- */

int claw_memory_append_session_turn(const char *session_id,
                                           const char *user_text,
                                           const char *assistant_text,
                                           const char *tool_msgs_json,
                                           int backend,
                                           uint32_t prompt_tokens,
                                           void *user_ctx)
{
    char fpath[CLAW_MEMORY_MAX_PATH];
    cJSON *root = NULL;
    cJSON *turns = NULL;
    char *existing = NULL;
    char *serialized = NULL;
    int ret = RTK_SUCCESS;

    (void)user_ctx;

    if (!s_mem.initialized) return RTK_FAIL;

    session_file_path(session_id, fpath, sizeof(fpath));

    /* Phase 1 — read session file under file_mutex (brief I/O hold) */
    rtos_mutex_take(s_mem.file_mutex, 0xFFFFFFFFUL);
    existing = slurp_file(fpath);
    rtos_mutex_give(s_mem.file_mutex);

    /* Parse outside any lock — pure CPU, no shared state */
    if (existing) {
        root = cJSON_Parse(existing);
        rtos_mem_free(existing);
        existing = NULL;
    }

    /* Phase 2 — build and serialize the new turn under mem mutex (no I/O) */
    rtos_mutex_take(s_mem.mutex, 0xFFFFFFFFUL);

    /* If parse failed or no file, create fresh root */
    if (!root) {
        root = cJSON_CreateObject();
        if (!root) { ret = RTK_ERR_NOMEM; goto done; }
        turns = cJSON_CreateArray();
        if (!turns) { cJSON_Delete(root); root = NULL; ret = RTK_ERR_NOMEM; goto done; }
        cJSON_AddItemToObject(root, "turns", turns);
    } else {
        turns = cJSON_GetObjectItem(root, "turns");
        if (!turns || !cJSON_IsArray(turns)) {
            /* Malformed — recreate */
            cJSON_Delete(root);
            root = cJSON_CreateObject();
            if (!root) { ret = RTK_ERR_NOMEM; goto done; }
            turns = cJSON_CreateArray();
            if (!turns) { cJSON_Delete(root); root = NULL; ret = RTK_ERR_NOMEM; goto done; }
            cJSON_AddItemToObject(root, "turns", turns);
        }
    }

    /* Enforce ring buffer: remove oldest turn if over limit */
    while (cJSON_GetArraySize(turns) >= (int)s_mem.max_session_turns) {
        cJSON_DeleteItemFromArray(turns, 0);
    }

    /* Append new turn — sanitize both fields through utf8_safe_copy so a
     * truncated multi-byte sequence at a UART/AT buffer boundary never
     * reaches the JSON file and causes downstream API parse errors. */
    {
        cJSON *turn = cJSON_CreateObject();
        if (!turn) { ret = RTK_ERR_NOMEM; goto done; }

        const char *u = user_text      ? user_text      : "";
        const char *a = assistant_text ? assistant_text : "";
        size_t u_len = strlen(u);
        size_t a_len = strlen(a);
        char *u_san = (char *)rtos_mem_malloc(u_len + 1);
        char *a_san = (char *)rtos_mem_malloc(a_len + 1);
        if (!u_san || !a_san) {
            rtos_mem_free(u_san);
            rtos_mem_free(a_san);
            cJSON_Delete(turn);
            ret = RTK_ERR_NOMEM;
            goto done;
        }
        claw_memory_utf8_safe_copy(u_san, u_len + 1, u, u_len);
        claw_memory_utf8_safe_copy(a_san, a_len + 1, a, a_len);
        cJSON_AddStringToObject(turn, "user",      u_san);
        cJSON_AddStringToObject(turn, "assistant", a_san);
        rtos_mem_free(u_san);
        rtos_mem_free(a_san);

        /* Attach this turn's verbatim tool round-trips for byte-identical
         * cross-turn replay (tool visibility + prompt-cache prefix continuity).
         * Tagged with the backend whose wire format they were built in; the
         * history provider skips them on a backend mismatch. A turn whose
         * tool blob exceeds the per-turn cap is stored as text only. */
        if (tool_msgs_json && tool_msgs_json[0] &&
                strlen(tool_msgs_json) <= CLAW_MEMORY_TOOLMSGS_PER_TURN_MAX) {
            cJSON *tm = cJSON_Parse(tool_msgs_json);
            if (tm && cJSON_IsArray(tm) && cJSON_GetArraySize(tm) > 0) {
                cJSON_AddItemToObject(turn, "tool_msgs", tm);
                cJSON_AddNumberToObject(turn, "tool_backend", (double)backend);
            } else {
                cJSON_Delete(tm);
            }
        }
        cJSON_AddItemToArray(turns, turn);
    }

    /* Effective context size of the last request: real prompt_tokens when the
     * endpoint reported usage, else a conservative char→token estimate
     * (session_total_chars/2 overestimates tokens + fixed overhead for the
     * system prompt & tools that are not stored in the session file). This one
     * value drives BOTH the compaction trigger and the provider's "context
     * approaching limit" warning, so the estimate path behaves like the
     * real-usage path. Stored on the session root for the provider to read. */
    uint32_t eff_tokens = (prompt_tokens > 0)
                          ? prompt_tokens
                          : (uint32_t)(session_total_chars(root) / 2 + 6000);
    cJSON_DeleteItemFromObject(root, "last_prompt_tokens");
    cJSON_AddNumberToObject(root, "last_prompt_tokens", (double)eff_tokens);

    /* Write back. Guard the whole-file size below slurp_file's limit so the
     * session is never slurp-rejected (which would lose all history):
     *   stage 1 — shed tool_msgs from the oldest turns (lowest cache value,
     *             first to be compacted away anyway); text is preserved;
     *   stage 2 — if still over (pure text too large, e.g. async compaction
     *             hasn't caught up near the window ceiling), drop the oldest
     *             WHOLE turns synchronously. This is the storage-level
     *             synchronous trim backstop. */
    serialized = cJSON_PrintUnformatted(root);
    if (!serialized) { ret = RTK_ERR_NOMEM; goto done; }
    if (strlen(serialized) > CLAW_MEMORY_SESSION_SOFT_MAX) {
        int tn = cJSON_GetArraySize(turns);
        int i;
        for (i = 0; i < tn && strlen(serialized) > CLAW_MEMORY_SESSION_SOFT_MAX; i++) {
            cJSON *t = cJSON_GetArrayItem(turns, i);
            if (!t || !cJSON_GetObjectItem(t, "tool_msgs")) continue;
            cJSON_DeleteItemFromObject(t, "tool_msgs");
            cJSON_DeleteItemFromObject(t, "tool_backend");
            rtos_mem_free(serialized);
            serialized = cJSON_PrintUnformatted(root);
            if (!serialized) { ret = RTK_ERR_NOMEM; goto done; }
        }
        /* stage 2: drop oldest whole turns, but never the protected tail. */
        while (cJSON_GetArraySize(turns) > (int)s_mem.compaction_protect_last &&
               strlen(serialized) > CLAW_MEMORY_SESSION_SOFT_MAX) {
            cJSON_DeleteItemFromArray(turns, 0);
            rtos_mem_free(serialized);
            serialized = cJSON_PrintUnformatted(root);
            if (!serialized) { ret = RTK_ERR_NOMEM; goto done; }
        }
        RTK_LOGI(TAG, "session over soft cap, shed to %uB (%d turns left)\n",
                 (unsigned)strlen(serialized), cJSON_GetArraySize(turns));
    }

    /* Compaction trigger decision — done while still holding mem mutex
     * (reads root which is only valid here). */
    bool do_compact = false;
    if (ret == RTK_SUCCESS && s_mem.compaction_token_threshold > 0) {
        cJSON *jpending = cJSON_GetObjectItem(root, "compaction_pending");
        bool pending = (jpending && cJSON_IsBool(jpending) && cJSON_IsTrue(jpending));
        if (pending || eff_tokens >= s_mem.compaction_token_threshold) {
            RTK_LOGI(TAG, "compact trigger: eff_tokens=%u thr=%u (prompt_tokens=%u)\n",
                     (unsigned)eff_tokens, (unsigned)s_mem.compaction_token_threshold,
                     (unsigned)prompt_tokens);
            do_compact = true;
        }
    }

done:
    /* Release mem mutex before any I/O — JSON tree no longer needed. */
    if (root) cJSON_Delete(root);
    rtos_mutex_give(s_mem.mutex);

    /* Phase 3 — write serialized JSON under file_mutex (brief I/O hold).
     * file_mutex is shared with compact task; they never write the same
     * file simultaneously. */
    if (ret == RTK_SUCCESS && serialized) {
        rtos_mutex_take(s_mem.file_mutex, 0xFFFFFFFFUL);
        FILE *f = fopen(fpath, "w");
        if (!f) {
            RTK_LOGE(TAG, "Cannot open session file for write: %s\n", fpath);
            ret = RTK_FAIL;
        } else {
            size_t slen = strlen(serialized);
            size_t written = fwrite(serialized, 1, slen, f);
            fclose(f);
            if (written != slen) {
                RTK_LOGE(TAG, "session write incomplete (%u/%u)\n",
                         (unsigned)written, (unsigned)slen);
                ret = RTK_FAIL;
            }
        }
        rtos_mutex_give(s_mem.file_mutex);
    }
    rtos_mem_free(serialized);

    if (ret == RTK_SUCCESS && do_compact)
        claw_memory_compact_enqueue(session_id);

    return ret;
}

/* ---- Profile context provider ---- */

static int collect_profile(const claw_agent_request_t *request,
                                  claw_agent_context_t *out_context,
                                  void *user_ctx)
{
    char path[CLAW_MEMORY_MAX_PATH];
    const char *filenames[] = { "AGENTS.md", "SOUL.md", "IDENTITY.md", "USER.md" };
    const size_t nfiles = sizeof(filenames) / sizeof(filenames[0]);
    char *parts[sizeof(filenames) / sizeof(filenames[0])];
    size_t total = 0;
    size_t i;
    char *combined = NULL;
    char *p;
    int ret = RTK_SUCCESS;

    (void)request;
    (void)user_ctx;

    memset(parts, 0, sizeof(parts));

    if (!s_mem.initialized) return RTK_FAIL;

    for (i = 0; i < nfiles; i++) {
        DiagSnPrintf(path, sizeof(path), "%s/%s", s_mem.profile_root, filenames[i]);
        parts[i] = slurp_file(path);
        if (parts[i]) {
            /* Trim trailing whitespace to detect truly empty */
            size_t len = strlen(parts[i]);
            while (len > 0 && (parts[i][len-1] == '\n' || parts[i][len-1] == '\r' ||
                               parts[i][len-1] == ' '  || parts[i][len-1] == '\t')) {
                parts[i][--len] = '\0';
            }
            if (len == 0) {
                rtos_mem_free(parts[i]);
                parts[i] = NULL;
            } else {
                total += len + 2; /* "\n\n" separator */
            }
        }
    }

    if (total == 0) {
        ret = RTK_FAIL;
        goto cleanup;
    }

    /* claw_agent frees out_context->content with libc free(), so use libc
     * malloc here — NOT rtos_mem_malloc, which sits on a separate heap and
     * would silently corrupt heap metadata when libc free() touched it. */
    combined = malloc(total + 1);
    if (!combined) {
        ret = RTK_ERR_NOMEM;
        goto cleanup;
    }
    p = combined;
    *p = '\0';

    for (i = 0; i < nfiles; i++) {
        if (!parts[i]) continue;
        if (p != combined) {
            _memcpy(p, "\n\n", 2);
            p += 2;
        }
        size_t len = strlen(parts[i]);
        _memcpy(p, parts[i], len);
        p += len;
    }
    *p = '\0';

    out_context->kind    = CLAW_AGENT_CONTEXT_KIND_SYSTEM_PROMPT;
    out_context->content = combined;

cleanup:
    for (i = 0; i < nfiles; i++) {
        rtos_mem_free(parts[i]);
    }
    return ret;
}

claw_agent_context_provider_t claw_memory_profile_provider = {
    .name     = "memory_profile",
    .collect  = collect_profile,
    .user_ctx = NULL,
};

/* ---- Session history context provider ---- */

static int collect_session_history(const claw_agent_request_t *request,
                                          claw_agent_context_t *out_context,
                                          void *user_ctx)
{
    char fpath[CLAW_MEMORY_MAX_PATH];
    char *file_content = NULL;
    cJSON *root = NULL;
    cJSON *turns = NULL;
    cJSON *arr = NULL;
    char *result = NULL;
    int ret = RTK_SUCCESS;
    int i, n;

    (void)user_ctx;

    if (!s_mem.initialized) return RTK_FAIL;

    session_file_path(request ? request->session_id : NULL, fpath, sizeof(fpath));

    /* Read file under file_mutex (brief I/O hold), then parse outside any lock */
    rtos_mutex_take(s_mem.file_mutex, 0xFFFFFFFFUL);
    file_content = slurp_file(fpath);
    rtos_mutex_give(s_mem.file_mutex);

    if (!file_content) return RTK_FAIL;

    root = cJSON_Parse(file_content);
    rtos_mem_free(file_content);

    /* Assemble messages array under mem mutex (pure in-memory) */
    rtos_mutex_take(s_mem.mutex, 0xFFFFFFFFUL);

    if (!root) {
        rtos_mutex_give(s_mem.mutex);
        return RTK_FAIL;
    }

    turns = cJSON_GetObjectItem(root, "turns");
    if (!turns || !cJSON_IsArray(turns)) {
        cJSON_Delete(root);
        rtos_mutex_give(s_mem.mutex);
        return RTK_FAIL;
    }

    n = cJSON_GetArraySize(turns);
    if (n == 0) {
        cJSON_Delete(root);
        rtos_mutex_give(s_mem.mutex);
        return RTK_FAIL;
    }

    /* Replay ALL stored turns — the file size is already bounded by the
     * token-budget compaction trigger + ring + soft-cap, so "fill the window"
     * is the goal. (Previously capped at the last 5 turns, which threw away
     * most of a long-running task's context.) */
    int start = 0;

    arr = cJSON_CreateArray();
    if (!arr) {
        cJSON_Delete(root);
        rtos_mutex_give(s_mem.mutex);
        return RTK_ERR_NOMEM;
    }

    for (i = start; i < n; i++) {
        cJSON *turn = cJSON_GetArrayItem(turns, i);
        if (!turn) continue;

        cJSON *user_item = cJSON_GetObjectItem(turn, "user");
        cJSON *asst_item = cJSON_GetObjectItem(turn, "assistant");
        const char *user_str = (user_item && cJSON_IsString(user_item)) ? user_item->valuestring : "";
        const char *asst_str = (asst_item && cJSON_IsString(asst_item)) ? asst_item->valuestring : "";

        cJSON *user_msg = cJSON_CreateObject();
        cJSON *asst_msg = cJSON_CreateObject();
        if (!user_msg || !asst_msg) {
            cJSON_Delete(user_msg);
            cJSON_Delete(asst_msg);
            ret = RTK_ERR_NOMEM;
            goto done_history;
        }
        cJSON_AddStringToObject(user_msg, "role",    "user");
        cJSON_AddStringToObject(user_msg, "content", user_str);
        cJSON_AddStringToObject(asst_msg, "role",    "assistant");
        cJSON_AddStringToObject(asst_msg, "content", asst_str);
        cJSON_AddItemToArray(arr, user_msg);

        /* Replay this turn's tool round-trips verbatim, BETWEEN the user
         * message and the assistant reply — reproducing the exact message
         * sequence the original request sent. This restores cross-turn tool
         * visibility and keeps the LLM prompt-cache prefix byte-identical.
         * Only when the stored wire format matches the active backend;
         * otherwise the blocks would be malformed for the current API, so we
         * fall back to the plain user/assistant text pair. */
        {
            cJSON *tm = cJSON_GetObjectItem(turn, "tool_msgs");
            cJSON *tb = cJSON_GetObjectItem(turn, "tool_backend");
            if (tm && cJSON_IsArray(tm) && tb && cJSON_IsNumber(tb) &&
                    (int)tb->valuedouble == (int)claw_config_get()->llm.backend) {
                cJSON *blk;
                cJSON_ArrayForEach(blk, tm) {
                    cJSON *dup = cJSON_Duplicate(blk, true);
                    if (dup) cJSON_AddItemToArray(arr, dup);
                }
            }
        }

        cJSON_AddItemToArray(arr, asst_msg);
    }

    result = cJSON_PrintUnformatted(arr);
    if (!result) {
        ret = RTK_ERR_NOMEM;
        goto done_history;
    }

    out_context->kind    = CLAW_AGENT_CONTEXT_KIND_MESSAGES;
    out_context->content = result;

done_history:
    cJSON_Delete(arr);
    cJSON_Delete(root);
    rtos_mutex_give(s_mem.mutex);
    return ret;
}

claw_agent_context_provider_t claw_memory_session_history_provider = {
    .name     = "memory_session_history",
    .collect  = collect_session_history,
    .user_ctx = NULL,
};

/* ---- Long-term label index provider (S1) ----
 *
 * Injects only summary labels (≤40-char each) instead of full memory bodies.
 * Pairs with the existing memory_recall tool: the model sees the catalog of
 * what it remembers and calls memory_recall by label/keyword to fetch detail.
 * Caps total entries at LT_MAX_ITEMS (64), so prompt cost stays O(N × ~64B).
 */

static int collect_long_term_label(const claw_agent_request_t *request,
                                          claw_agent_context_t *out_context,
                                          void *user_ctx)
{
    (void)request;
    (void)user_ctx;

    if (!s_mem.initialized) return RTK_FAIL;

    /* LT store read: file I/O under file_mutex, then process in mem mutex */
    rtos_mutex_take(s_mem.file_mutex, 0xFFFFFFFFUL);
    cJSON *arr = lt_load();
    rtos_mutex_give(s_mem.file_mutex);

    int count = arr ? cJSON_GetArraySize(arr) : 0;
    if (count == 0) {
        cJSON_Delete(arr);
        return RTK_FAIL;
    }

    rtos_mutex_take(s_mem.mutex, 0xFFFFFFFFUL);

    /* Header (~600B) + per-entry (label up to 64 + "- \n" framing = ~70B). */
    size_t buf_size = 1024 + (size_t)count * (CLAW_MEMORY_SUMMARY_MAX + 8);
    char *buf = malloc(buf_size);
    if (!buf) {
        cJSON_Delete(arr);
        rtos_mutex_give(s_mem.mutex);
        return RTK_FAIL;
    }

    size_t pos = 0;
    pos += DiagSnPrintf(buf + pos, buf_size - pos,
        "## Long-term Memory Index\n"
        "The list below contains only summary labels of stored memories, not full bodies.\n"
        "Use 'memory_recall' with a label substring or keyword to fetch the full content.\n"
        "Labels must be copied verbatim from this catalog. Do not invent labels not listed.\n"
        "If the user asks what you remember about them, recall before answering whenever a relevant label exists.\n");

    int emitted = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        cJSON *jsummary = cJSON_GetObjectItem(it, "summary");
        cJSON *jcontent = cJSON_GetObjectItem(it, "content");
        const char *label = NULL;
        char fallback[64];
        if (jsummary && cJSON_IsString(jsummary) && jsummary->valuestring[0]) {
            label = jsummary->valuestring;
        } else if (jcontent && cJSON_IsString(jcontent) && jcontent->valuestring[0]) {
            /* Legacy items without summary: take ≤40 bytes of content as a
             * transient label. UTF-8 safe — never splits a multi-byte char. */
            claw_memory_utf8_safe_copy(fallback, sizeof(fallback),
                                       jcontent->valuestring, 40);
            label = fallback;
        } else {
            continue;
        }
        if (pos >= buf_size - 8) break;
        pos += DiagSnPrintf(buf + pos, buf_size - pos, "- %s\n", label);
        emitted++;
    }
    cJSON_Delete(arr);
    rtos_mutex_give(s_mem.mutex);

    if (emitted == 0) {
        free(buf);
        return RTK_FAIL;
    }

    RTK_LOGD("mem_lt_label", "injecting %d label(s) (%uB)\n", emitted, (unsigned)pos);
    out_context->kind    = CLAW_AGENT_CONTEXT_KIND_SYSTEM_PROMPT;
    out_context->content = buf;
    return RTK_SUCCESS;
}

claw_agent_context_provider_t claw_memory_long_term_label_provider = {
    .name     = "memory_long_term_label",
    .collect  = collect_long_term_label,
    .user_ctx = NULL,
};

/* ================================================================
 * Structured long-term memory (CRUD)
 * Items stored in <memory_root>/long_term_store.json
 * ================================================================ */

#define LT_STORE_FILENAME "long_term_store.json"
#define LT_MAX_ITEMS      64

static void lt_store_path(char *buf, size_t n)
{
    DiagSnPrintf(buf, n, "%s/%s", s_mem.memory_root, LT_STORE_FILENAME);
}

/* Load JSON array from store file; creates empty array if absent. On parse
 * failure the corrupted file is renamed to long_term_store.json.corrupt-<ts>
 * so the user can inspect it later — silently overwriting it with an empty
 * array would erase forensic evidence of why memory was lost. */
static cJSON *lt_load(void)
{
    char path[CLAW_MEMORY_MAX_PATH];
    lt_store_path(path, sizeof(path));
    char *raw = slurp_file(path);
    if (!raw) return cJSON_CreateArray();
    cJSON *arr = cJSON_Parse(raw);
    rtos_mem_free(raw);
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        char corrupt_path[CLAW_MEMORY_MAX_PATH];
        snprintf(corrupt_path, sizeof(corrupt_path),
                 "%s.corrupt-%lu", path, (unsigned long)time(NULL));
        if (rename(path, corrupt_path) == 0) {
            RTK_LOGE(TAG, "lt_load: store unparseable, preserved at %s\n",
                     corrupt_path);
        } else {
            RTK_LOGE(TAG, "lt_load: store unparseable; rename for forensics failed\n");
        }
        return cJSON_CreateArray();
    }
    return arr;
}

/* Atomic write of the long-term store: rename current → .bak, write new
 * file, remove .bak. Any failure between rename and remove leaves a .bak
 * that lt_rollback_stale_bak() will recover at next boot. The previous
 * truncate-then-write approach lost the entire store on any power glitch
 * during fwrite, which mattered far more here than for sessions because
 * this file is the single source of truth for long-term memory. */
static void lt_save(const cJSON *arr)
{
    char path[CLAW_MEMORY_MAX_PATH];
    char bak_path[CLAW_MEMORY_MAX_PATH + 8];
    lt_store_path(path, sizeof(path));
    snprintf(bak_path, sizeof(bak_path), "%s.bak", path);

    char *s = cJSON_PrintUnformatted(arr);
    if (!s) {
        RTK_LOGE(TAG, "lt_save: cJSON_PrintUnformatted OOM\n");
        return;
    }
    size_t slen = strlen(s);

    /* Pre-clean any stale .bak — on FatFs, rename-over-existing returns
     * FR_EXIST, which would otherwise wedge every subsequent save until
     * manual cleanup. (Same trap as in claw_memory_compact.) */
    (void)remove(bak_path);

    /* Step 1: rename current to .bak. If the original file does not exist
     * yet (first save), rename fails — that's fine, we just write the new
     * file directly. */
    bool had_old = (rename(path, bak_path) == 0);

    /* Step 2: write new file. */
    FILE *f = fopen(path, "w");
    if (!f) {
        RTK_LOGE(TAG, "lt_save: fopen(%s) failed\n", path);
        if (had_old) (void)rename(bak_path, path);   /* roll back */
        free(s);
        return;
    }
    size_t written = fwrite(s, 1, slen, f);
    fclose(f);
    if (written != slen) {
        RTK_LOGE(TAG, "lt_save: fwrite incomplete (%u/%u), rolling back\n",
                 (unsigned)written, (unsigned)slen);
        (void)remove(path);
        if (had_old) (void)rename(bak_path, path);
        free(s);
        return;
    }

    /* Step 3: drop the .bak. Failure is cosmetic — startup recovery will
     * handle leftover .bak as "new file is good" and clean up. */
    if (had_old && remove(bak_path) != 0) {
        RTK_LOGW(TAG, "lt_save: could not remove .bak (cosmetic)\n");
    }
    RTK_LOGI(TAG, "lt_save: wrote %u bytes to %s\n", (unsigned)slen, path);
    free(s);
}

/* Boot-time recovery of any long_term_store.json.bak left from a crashed
 * write. Two crash points produce a stale .bak — same protocol as
 * claw_memory_compact's session rollback:
 *   1) crashed AFTER rename(.json,.bak) but BEFORE write_file: the new
 *      .json doesn't exist → rollback by renaming .bak back.
 *   2) crashed AFTER write_file but BEFORE remove(.bak): the new .json
 *      is the good copy → just delete the stale .bak. (Critical on
 *      FatFs where rename-over-existing returns FR_EXIST.) */
static void lt_rollback_stale_bak(void)
{
    char path[CLAW_MEMORY_MAX_PATH];
    char bak_path[CLAW_MEMORY_MAX_PATH + 8];
    lt_store_path(path, sizeof(path));
    snprintf(bak_path, sizeof(bak_path), "%s.bak", path);

    FILE *bak = fopen(bak_path, "r");
    if (!bak) return;          /* no .bak — clean state */
    fclose(bak);

    FILE *cur = fopen(path, "r");
    if (cur) {
        /* New file exists → it's the good one. Drop the stale .bak. */
        fclose(cur);
        if (remove(bak_path) == 0) {
            RTK_LOGI(TAG, "lt: removed stale .bak (new file intact)\n");
        } else {
            RTK_LOGE(TAG, "lt: could not remove stale .bak\n");
        }
    } else {
        /* New file missing → rollback. */
        if (rename(bak_path, path) == 0) {
            RTK_LOGW(TAG, "lt: rolled back from .bak\n");
        } else {
            RTK_LOGE(TAG, "lt: rollback rename failed\n");
        }
    }
}

static uint32_t lt_next_id(const cJSON *arr)
{
    uint32_t max_id = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        cJSON *jid = cJSON_GetObjectItem(it, "id");
        if (jid && cJSON_IsNumber(jid)) {
            uint32_t v = (uint32_t)jid->valuedouble;
            if (v > max_id) max_id = v;
        }
    }
    return max_id + 1;
}

int claw_memory_store(claw_memory_item_t *item)
{
    if (!s_mem.initialized || !item) return RTK_ERR_BADARG;

    /* Read LT store (I/O) under file_mutex */
    rtos_mutex_take(s_mem.file_mutex, 0xFFFFFFFFUL);
    cJSON *arr = lt_load();
    rtos_mutex_give(s_mem.file_mutex);

    /* Modify in-memory array under mem mutex (no I/O) */
    rtos_mutex_take(s_mem.mutex, 0xFFFFFFFFUL);

    /* Evict oldest if at capacity */
    while (cJSON_GetArraySize(arr) >= LT_MAX_ITEMS) {
        cJSON_DeleteItemFromArray(arr, 0);
    }

    item->id         = lt_next_id(arr);
    item->created_at = (uint32_t)time(NULL);
    item->access_count = 0;

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "id",           (double)item->id);
    cJSON_AddStringToObject(obj, "source",       item->source);
    cJSON_AddStringToObject(obj, "content",      item->content);
    cJSON_AddStringToObject(obj, "tags",         item->tags);
    cJSON_AddStringToObject(obj, "summary",      item->summary);
    cJSON_AddNumberToObject(obj, "access_count", 0);
    cJSON_AddNumberToObject(obj, "created_at",   (double)item->created_at);
    cJSON_AddItemToArray(arr, obj);

    rtos_mutex_give(s_mem.mutex);

    /* Write back (I/O) under file_mutex */
    rtos_mutex_take(s_mem.file_mutex, 0xFFFFFFFFUL);
    lt_save(arr);
    rtos_mutex_give(s_mem.file_mutex);

    cJSON_Delete(arr);
    return RTK_SUCCESS;
}

char *claw_memory_recall(const char *keyword, int max_results)
{
    if (!s_mem.initialized) return NULL;
    if (max_results <= 0) max_results = 10;

    rtos_mutex_take(s_mem.file_mutex, 0xFFFFFFFFUL);
    cJSON *arr = lt_load();
    rtos_mutex_give(s_mem.file_mutex);

    rtos_mutex_take(s_mem.mutex, 0xFFFFFFFFUL);
    cJSON *results = cJSON_CreateArray();
    int found = 0;
    bool dirty = false;   /* only persist when access_count actually changed */

    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (found >= max_results) break;
        cJSON *jcontent = cJSON_GetObjectItem(it, "content");
        cJSON *jtags    = cJSON_GetObjectItem(it, "tags");
        cJSON *jsummary = cJSON_GetObjectItem(it, "summary");
        if (!jcontent || !cJSON_IsString(jcontent)) continue;
        int match = 0;
        if (!keyword || keyword[0] == '\0') {
            match = 1;
        } else {
            if (strstr(jcontent->valuestring, keyword)) match = 1;
            else if (jtags && cJSON_IsString(jtags) && strstr(jtags->valuestring, keyword)) match = 1;
            else if (jsummary && cJSON_IsString(jsummary) && strstr(jsummary->valuestring, keyword)) match = 1;
        }
        if (match) {
            cJSON *jacnt = cJSON_GetObjectItem(it, "access_count");
            if (jacnt && cJSON_IsNumber(jacnt)) {
                jacnt->valuedouble += 1;
                dirty = true;
            }
            cJSON_AddItemToArray(results, cJSON_Duplicate(it, 1));
            found++;
        }
    }
    rtos_mutex_give(s_mem.mutex);

    if (dirty) {
        rtos_mutex_take(s_mem.file_mutex, 0xFFFFFFFFUL);
        lt_save(arr);
        rtos_mutex_give(s_mem.file_mutex);
    }
    cJSON_Delete(arr);

    char *s = cJSON_PrintUnformatted(results);
    cJSON_Delete(results);
    return s;
}

int claw_memory_update(uint32_t id, const char *new_content)
{
    if (!s_mem.initialized || !new_content) return RTK_ERR_BADARG;

    rtos_mutex_take(s_mem.file_mutex, 0xFFFFFFFFUL);
    cJSON *arr = lt_load();
    rtos_mutex_give(s_mem.file_mutex);

    rtos_mutex_take(s_mem.mutex, 0xFFFFFFFFUL);
    int ret = RTK_FAIL;
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        cJSON *jid = cJSON_GetObjectItem(it, "id");
        if (jid && cJSON_IsNumber(jid) && (uint32_t)jid->valuedouble == id) {
            cJSON *jcontent = cJSON_GetObjectItem(it, "content");
            if (jcontent && cJSON_IsString(jcontent)) {
                if (cJSON_SetValuestring(jcontent, new_content) != NULL)
                    ret = RTK_SUCCESS;
            }
            break;
        }
    }
    rtos_mutex_give(s_mem.mutex);

    if (ret == RTK_SUCCESS) {
        rtos_mutex_take(s_mem.file_mutex, 0xFFFFFFFFUL);
        lt_save(arr);
        rtos_mutex_give(s_mem.file_mutex);
    }
    cJSON_Delete(arr);
    return ret;
}

int claw_memory_forget(uint32_t id)
{
    if (!s_mem.initialized) return RTK_ERR_BADARG;

    rtos_mutex_take(s_mem.file_mutex, 0xFFFFFFFFUL);
    cJSON *arr = lt_load();
    rtos_mutex_give(s_mem.file_mutex);

    rtos_mutex_take(s_mem.mutex, 0xFFFFFFFFUL);
    int idx = 0, ret = RTK_FAIL;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        cJSON *jid = cJSON_GetObjectItem(it, "id");
        if (jid && cJSON_IsNumber(jid) && (uint32_t)jid->valuedouble == id) {
            cJSON_DeleteItemFromArray(arr, idx);
            ret = RTK_SUCCESS;
            break;
        }
        idx++;
    }
    rtos_mutex_give(s_mem.mutex);

    if (ret == RTK_SUCCESS) {
        rtos_mutex_take(s_mem.file_mutex, 0xFFFFFFFFUL);
        lt_save(arr);
        rtos_mutex_give(s_mem.file_mutex);
    }
    cJSON_Delete(arr);
    return ret;
}

char *claw_memory_list(int max_results)
{
    return claw_memory_recall(NULL, max_results);
}

/* ---- LLM-callable tool wrappers ---- */

#define set_output_fmt claw_cap_set_output

static int cap_memory_store(const char *input_json,
                            const claw_cap_call_context_t *ctx,
                            char **output)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(input_json);
    if (!root) { return set_output_fmt(output, "{\"error\":\"invalid json\"}"); }

    cJSON *jcontent = cJSON_GetObjectItem(root, "content");
    cJSON *jsource  = cJSON_GetObjectItem(root, "source");
    cJSON *jtags    = cJSON_GetObjectItem(root, "tags");

    if (!jcontent || !cJSON_IsString(jcontent)) {
        cJSON_Delete(root);
        return set_output_fmt(output, "{\"error\":\"missing content\"}");
    }

    claw_memory_item_t item = {0};
    /* UTF-8 boundary-safe copy: a raw strncpy would split a multi-byte (CJK)
     * character at the byte limit, leaving a dangling lead byte. That invalid
     * UTF-8 is later serialized verbatim into the LLM request and rejected by
     * the server ("Invalid UTF-8 middle byte"). */
    claw_memory_utf8_safe_copy_marked(item.content, sizeof(item.content),
                                      jcontent->valuestring, CLAW_MEMORY_CONTENT_MAX - 1);
    if (jsource && cJSON_IsString(jsource))
        claw_memory_utf8_safe_copy(item.source, sizeof(item.source),
                                   jsource->valuestring, CLAW_MEMORY_SOURCE_MAX - 1);
    else
        strncpy(item.source, "llm", CLAW_MEMORY_SOURCE_MAX - 1);
    if (jtags && cJSON_IsString(jtags))
        claw_memory_utf8_safe_copy(item.tags, sizeof(item.tags),
                                   jtags->valuestring, CLAW_MEMORY_TAG_MAX - 1);

    claw_memory_store(&item);
    int rc = set_output_fmt(output, "{\"status\":\"stored\",\"id\":%lu}",
                            (unsigned long)item.id);
    cJSON_Delete(root);
    return rc;
}

static int cap_memory_recall(const char *input_json,
                             const claw_cap_call_context_t *ctx,
                             char **output)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(input_json);
    const char *kw = NULL;
    int max = 5;
    if (root) {
        cJSON *jkw  = cJSON_GetObjectItem(root, "keyword");
        cJSON *jmax = cJSON_GetObjectItem(root, "max_results");
        if (jkw  && cJSON_IsString(jkw))  kw  = jkw->valuestring;
        if (jmax && cJSON_IsNumber(jmax))  max = (int)jmax->valuedouble;
    }
    char *result = claw_memory_recall(kw, max);
    int rc;
    if (result) {
        rc = set_output_fmt(output, "{\"items\":%s}", result);
        free(result);
    } else {
        rc = set_output_fmt(output, "{\"items\":[]}");
    }
    if (root) cJSON_Delete(root);
    return rc;
}

static int cap_memory_update(const char *input_json,
                             const claw_cap_call_context_t *ctx,
                             char **output)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(input_json);
    if (!root) { return set_output_fmt(output, "{\"error\":\"invalid json\"}"); }
    cJSON *jid      = cJSON_GetObjectItem(root, "id");
    cJSON *jcontent = cJSON_GetObjectItem(root, "content");
    if (!jid || !cJSON_IsNumber(jid) || !jcontent || !cJSON_IsString(jcontent)) {
        cJSON_Delete(root);
        return set_output_fmt(output, "{\"error\":\"missing id or content\"}");
    }
    int ret = claw_memory_update((uint32_t)jid->valuedouble, jcontent->valuestring);
    int rc = set_output_fmt(output, "{\"status\":\"%s\"}",
                            ret == RTK_SUCCESS ? "updated" : "not_found");
    cJSON_Delete(root);
    return rc;
}

static int cap_memory_forget(const char *input_json,
                             const claw_cap_call_context_t *ctx,
                             char **output)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(input_json);
    if (!root) { return set_output_fmt(output, "{\"error\":\"invalid json\"}"); }
    cJSON *jid = cJSON_GetObjectItem(root, "id");
    if (!jid || !cJSON_IsNumber(jid)) {
        cJSON_Delete(root);
        return set_output_fmt(output, "{\"error\":\"missing id\"}");
    }
    int ret = claw_memory_forget((uint32_t)jid->valuedouble);
    int rc = set_output_fmt(output, "{\"status\":\"%s\"}",
                            ret == RTK_SUCCESS ? "forgotten" : "not_found");
    cJSON_Delete(root);
    return rc;
}

static int cap_memory_list(const char *input_json,
                           const claw_cap_call_context_t *ctx,
                           char **output)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(input_json);
    int max = 20;
    if (root) {
        cJSON *jmax = cJSON_GetObjectItem(root, "max_results");
        if (jmax && cJSON_IsNumber(jmax)) max = (int)jmax->valuedouble;
        cJSON_Delete(root);
    }
    char *result = claw_memory_list(max);
    int rc;
    if (result) {
        rc = set_output_fmt(output, "{\"items\":%s}", result);
        free(result);
    } else {
        rc = set_output_fmt(output, "{\"items\":[]}");
    }
    return rc;
}

static const claw_cap_descriptor_t s_mem_caps[] = {
    {
        .id = "memory_store", .name = "memory_store", .family = "memory",
        .description = "Store a long-term memory. Args: content(string), source(string, optional), tags(string, optional, comma-separated).",
        .kind = CLAW_CAP_KIND_INVOKE, .cap_flags = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"content\":{\"type\":\"string\"},"
            "\"source\":{\"type\":\"string\"},"
            "\"tags\":{\"type\":\"string\"}"
            "},\"required\":[\"content\"]}",
        .execute = cap_memory_store,
    },
    {
        .id = "memory_recall", .name = "memory_recall", .family = "memory",
        .description = "Search long-term memories by keyword. Args: keyword(string, optional), max_results(int, optional).",
        .kind = CLAW_CAP_KIND_INVOKE, .cap_flags = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"keyword\":{\"type\":\"string\"},"
            "\"max_results\":{\"type\":\"integer\"}"
            "}}",
        .execute = cap_memory_recall,
    },
    {
        .id = "memory_update", .name = "memory_update", .family = "memory",
        .description = "Update a long-term memory by id. Args: id(int), content(string).",
        .kind = CLAW_CAP_KIND_INVOKE, .cap_flags = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"id\":{\"type\":\"integer\"},"
            "\"content\":{\"type\":\"string\"}"
            "},\"required\":[\"id\",\"content\"]}",
        .execute = cap_memory_update,
    },
    {
        .id = "memory_forget", .name = "memory_forget", .family = "memory",
        .description = "Delete a long-term memory by id. Args: id(int).",
        .kind = CLAW_CAP_KIND_INVOKE, .cap_flags = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"id\":{\"type\":\"integer\"}"
            "},\"required\":[\"id\"]}",
        .execute = cap_memory_forget,
    },
    {
        .id = "memory_list", .name = "memory_list", .family = "memory",
        .description = "List all long-term memories. Args: max_results(int, optional, default 20).",
        .kind = CLAW_CAP_KIND_INVOKE, .cap_flags = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"max_results\":{\"type\":\"integer\"}"
            "}}",
        .execute = cap_memory_list,
    },
};

static const claw_cap_group_t s_mem_group = {
    .group_id         = "memory",
    .plugin_name      = "claw_memory",
    .version          = "1",
    .descriptors      = s_mem_caps,
    .descriptor_count = 5,
};

static void claw_memory_register_caps(void)
{
    claw_cap_register_group(&s_mem_group);
}

/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_session_mgr.h"
#include "claw_memory.h"
#include "claw_compat.h"
#include <cJSON.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>

#define TAG "cap_session_mgr"

/* ------------------------------------------------------------------ */
/* Module state                                                         */
/* ------------------------------------------------------------------ */

static char              s_session_root[128];
static char              s_mapping_dir[160];
static rtos_mutex_t      s_mutex;
static bool              s_initialized;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* djb2 hash */
static uint32_t hash_str(const char *s)
{
    uint32_t h = 5381;
    int c;
    while ((c = (unsigned char)*s++)) {
        h = ((h << 5) + h) + (uint32_t)c;
    }
    return h;
}

/* Sanitize a string so it is safe as part of a filename. */
static void sanitize_filename(const char *src, char *dst, size_t max_len)
{
    size_t i = 0;
    const char *p = src;
    while (*p && i < max_len) {
        unsigned char c = (unsigned char)*p++;
        bool ok = (c >= 'A' && c <= 'Z') ||
                  (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-';
        dst[i++] = ok ? (char)c : '_';
    }
    dst[i] = '\0';
}

/* Sanitize channel or chat_id for embedding inside session_id. */
static void sanitize_session_field(const char *src, char *dst, size_t dst_size)
{
    if (dst_size == 0) return;
    size_t i = 0, limit = dst_size - 1;
    if (src) {
        while (src[i] != '\0' && i < limit) {
            unsigned char c = (unsigned char)src[i];
            bool ok = (c >= 'A' && c <= 'Z') ||
                      (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') ||
                      c == '.' || c == '_' || c == '-';
            dst[i] = ok ? (char)c : '_';
            i++;
        }
    }
    dst[i] = '\0';
}

/* Validate alias: characters [A-Za-z0-9._-], length 1-32. */
static bool is_valid_alias(const char *alias)
{
    if (!alias || !alias[0]) return false;
    size_t len = 0;
    for (const char *p = alias; *p; p++, len++) {
        unsigned char c = (unsigned char)*p;
        bool ok = (c >= 'A' && c <= 'Z') ||
                  (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return (len >= 1 && len <= 32);
}

/* Check whether a cJSON string array contains a given string. */
static bool array_has_string(const cJSON *arr, const char *s)
{
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (cJSON_IsString(item) && strcmp(item->valuestring, s) == 0) {
            return true;
        }
    }
    return false;
}

/* Resolve the chat_map JSON path for a (channel, chat_id) pair. */
static void chat_map_path_for(const char *channel, const char *chat_id,
                              char *out, size_t out_size)
{
    char chat_key[200];
    char sanitized[33];
    snprintf(chat_key, sizeof(chat_key), "%s:%s",
             channel ? channel : "", chat_id ? chat_id : "");
    sanitize_filename(chat_key, sanitized, sizeof(sanitized) - 1);
    uint32_t h = hash_str(chat_key);
    snprintf(out, out_size, "%s/s_%s_%08x.json",
             s_mapping_dir, sanitized, (unsigned)h);
}

/* VFS mkdir: LFS returns -17 for "already exists". */
#define MKDIR_ERR_EXIST  (-17)

static void try_mkdir(const char *path)
{
    int ret = mkdir(path, 0755);
    if (ret != 0 && ret != MKDIR_ERR_EXIST) {
        RTK_LOGW(TAG, "mkdir('%s') failed ret=%d\n", path, ret);
    }
}

/* Read chat_map JSON file; fills map_path.
 * Returns heap-allocated cJSON (caller must cJSON_Delete) or NULL on failure. */
/* Read chat_map from a pre-computed path — no mutex, no path resolution.
 * Used by all operations to avoid holding the mutex during flash I/O. */
static cJSON *read_chat_map_at(const char *map_path)
{
    FILE *f = fopen(map_path, "r");
    if (!f) return NULL;
    char *fbuf = (char *)rtos_mem_malloc(1024);
    if (!fbuf) { fclose(f); return NULL; }
    size_t nread = fread(fbuf, 1, 1023, f);
    fclose(f);
    fbuf[nread] = '\0';
    cJSON *root = cJSON_Parse(fbuf);
    rtos_mem_free(fbuf);
    return root;
}

static cJSON *read_chat_map(const char *channel, const char *chat_id,
                            char *map_path, size_t map_path_size)
{
    chat_map_path_for(channel, chat_id, map_path, map_path_size);
    return read_chat_map_at(map_path);
}

/* Write serialized JSON string to map_path atomically: write to .tmp then rename.
 * LittleFS rename is atomic — a power loss can never leave a zero-byte file. */
static int write_chat_map_str(const char *map_path, const char *json_str)
{
    size_t plen = strlen(map_path);
    char *tmp_path = (char *)rtos_mem_malloc(plen + 5);
    if (!tmp_path) return RTK_FAIL;
    memcpy(tmp_path, map_path, plen);
    memcpy(tmp_path + plen, ".tmp", 5);   /* includes NUL */

    FILE *wf = fopen(tmp_path, "w");
    if (!wf) {
        RTK_LOGW(TAG, "write_chat_map: cannot open %s\n", tmp_path);
        rtos_mem_free(tmp_path);
        return RTK_FAIL;
    }
    size_t slen = strlen(json_str);
    size_t w = fwrite(json_str, 1, slen, wf);
    fclose(wf);
    if (w != slen) {
        remove(tmp_path);
        rtos_mem_free(tmp_path);
        return RTK_FAIL;
    }
    int r = rename(tmp_path, map_path);
    rtos_mem_free(tmp_path);
    return (r == 0) ? RTK_SUCCESS : RTK_FAIL;
}

/* Serialize root to JSON string using cJSON's allocator. Caller must cJSON_free. */
static char *serialize_chat_map(cJSON *root)
{
    return cJSON_PrintUnformatted(root);
}

/* Build a new chat_map object for (channel, chat_id) with "default" session. */
static cJSON *create_default_chat_map(const char *channel, const char *chat_id)
{
    char chat_key[200];
    snprintf(chat_key, sizeof(chat_key), "%s:%s",
             channel ? channel : "", chat_id ? chat_id : "");
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "chat_key", chat_key);
    cJSON_AddStringToObject(root, "current", "default");
    cJSON *arr = cJSON_AddArrayToObject(root, "sessions");
    if (!arr) { cJSON_Delete(root); return NULL; }
    cJSON_AddItemToArray(arr, cJSON_CreateString("default"));
    return root;
}

/* Generate an automatic session alias.
 * Clock synced (time > 1700000000): "MMDD-HHMM", dedup with "-2"/"-3".
 * Clock not synced: "s<N>", N = array length + 1, deduplicated. */
static void auto_alias(const cJSON *sessions_arr, char *out, size_t out_size)
{
    char base[24];
    time_t t = time(NULL);
    if (t > 1700000000) {
        /* Use gmtime_r for thread safety — gmtime() returns a global static buffer */
        struct tm tm_buf;
        struct tm *tm_info = gmtime_r(&t, &tm_buf);
        if (tm_info) {
            snprintf(base, sizeof(base), "%02d%02d-%02d%02d",
                     tm_info->tm_mon + 1, tm_info->tm_mday,
                     tm_info->tm_hour, tm_info->tm_min);
        } else {
            snprintf(base, sizeof(base), "s1");
        }
    } else {
        int n = cJSON_GetArraySize(sessions_arr) + 1;
        snprintf(base, sizeof(base), "s%d", n);
    }

    char candidate[40];
    strlcpy(candidate, base, sizeof(candidate));
    int suffix = 2;
    while (array_has_string(sessions_arr, candidate)) {
        snprintf(candidate, sizeof(candidate), "%s-%d", base, suffix++);
        if (suffix > 99) break;
    }
    strlcpy(out, candidate, out_size);
}

/* Build "channel:chat_id:alias" session_id from sanitized fields. */
static void build_sid(const char *channel, const char *chat_id, const char *alias,
                      char *out, size_t out_size)
{
    char ch_safe[64];
    char id_safe[128];
    sanitize_session_field(channel, ch_safe, sizeof(ch_safe));
    sanitize_session_field(chat_id, id_safe, sizeof(id_safe));
    snprintf(out, out_size, "%s:%s:%s", ch_safe, id_safe, alias);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int cap_session_mgr_init(const char *session_root_dir)
{
    if (!session_root_dir || !session_root_dir[0]) {
        return RTK_ERR_BADARG;
    }

    strlcpy(s_session_root, session_root_dir, sizeof(s_session_root));
    snprintf(s_mapping_dir, sizeof(s_mapping_dir), "%s/chat_map", s_session_root);

    try_mkdir(s_session_root);
    try_mkdir(s_mapping_dir);

    int err = rtos_mutex_create(&s_mutex);
    if (err != RTK_SUCCESS) {
        RTK_LOGE(TAG, "Failed to create mutex\n");
        return RTK_ERR_NOMEM;
    }

    s_initialized = true;
    RTK_LOGI(TAG, "Initialized (root=%s, map=%s)\n", s_session_root, s_mapping_dir);
    return RTK_SUCCESS;
}

size_t cap_session_mgr_build_session_id(const claw_event_t *event,
                                         char *buf, size_t buf_size,
                                         void *user_ctx)
{
    (void)user_ctx;

    if (!event || !buf || buf_size == 0) return 0;

    /* For TRIGGER policy the alias-map is irrelevant; delegate to base. */
    if (event->session_policy != CLAW_EVENT_SESSION_POLICY_CHAT) {
        return claw_event_build_session_id(event, buf, buf_size);
    }

    const char *channel = event->source_channel[0] ? event->source_channel : NULL;
    const char *chat_id = event->chat_id[0]        ? event->chat_id        : NULL;

    if (!channel && !chat_id) {
        size_t n = (size_t)snprintf(buf, buf_size, "global");
        return n < buf_size ? n : buf_size - 1;
    }

    /* WebUI multi-tab routing: caller embeds target alias in message_id.
     * Skip the chat_map lookup and use it directly, but ensure the alias
     * exists in the chat_map so GET /api/session can list it. */
    if (event->message_id[0] && is_valid_alias(event->message_id)) {
        int nr = cap_session_mgr_new(channel, chat_id, event->message_id, NULL, 0);
        if (nr != RTK_SUCCESS && nr != CAP_SESSION_ERR_CONFLICT) {
            RTK_LOGW(TAG, "build_session_id: failed to register alias '%s' (%d)\n",
                     event->message_id, nr);
        }
        build_sid(channel, chat_id, event->message_id, buf, buf_size);
        return strlen(buf);
    }

    char *map_path = (char *)rtos_mem_malloc(320);
    if (!map_path) {
        /* fallback: write default session ID directly into caller's buffer */
        build_sid(channel, chat_id, "default", buf, buf_size);
        size_t n = strlen(buf);
        return n < buf_size ? n : buf_size - 1;
    }
    char current_alias[40] = "default";
    char *new_map_json = NULL;  /* serialized default map to write, if needed */

    /* Read outside mutex — avoids holding lock during flash I/O.  */
    cJSON *root = read_chat_map(channel, chat_id, map_path, 320);
    if (root) {
        cJSON *cur = cJSON_GetObjectItem(root, "current");
        if (cur && cJSON_IsString(cur) && cur->valuestring[0]) {
            strlcpy(current_alias, cur->valuestring, sizeof(current_alias));
        }
        cJSON_Delete(root);
    } else {
        /* No file — build default and serialize under mutex, write outside */
        cJSON *new_root = create_default_chat_map(channel, chat_id);
        if (new_root) {
            new_map_json = serialize_chat_map(new_root);
            cJSON_Delete(new_root);
        }
    }



    /* File write outside the mutex — avoids holding mutex during flash I/O */
    if (new_map_json) {
        write_chat_map_str(map_path, new_map_json);
        cJSON_free(new_map_json);
        RTK_LOGI(TAG, "Created chat_map for %s:%s with default\n",
                 channel ? channel : "", chat_id ? chat_id : "");
    }

    rtos_mem_free(map_path);
    build_sid(channel, chat_id, current_alias, buf, buf_size);
    size_t n = strlen(buf);
    return n < buf_size ? n : buf_size - 1;
}

int cap_session_mgr_new(const char *channel, const char *chat_id,
                        const char *alias,
                        char *out_alias, size_t out_size)
{
    if (!s_initialized) return RTK_FAIL;
    if (!channel || !channel[0] || !chat_id || !chat_id[0]) return RTK_ERR_BADARG;
    if (alias && alias[0] && !is_valid_alias(alias)) return RTK_ERR_BADARG;

    char *map_path = (char *)rtos_mem_malloc(320);
    if (!map_path) return RTK_FAIL;
    char actual[40];
    char *json_str = NULL;
    int rc = RTK_FAIL;

    /* Read outside mutex — avoids holding lock during flash I/O.  */
    cJSON *root = read_chat_map(channel, chat_id, map_path, 320);
    if (!root) {
        root = create_default_chat_map(channel, chat_id);
    }
    if (!root) {
        rtos_mem_free(map_path);
        return RTK_FAIL;
    }

    rtos_mutex_take(s_mutex, 0xFFFFFFFFUL);

    cJSON *sessions = cJSON_GetObjectItem(root, "sessions");
    if (!sessions || !cJSON_IsArray(sessions)) {
        /* Repair: reconstruct sessions array from existing 'current' field */
        cJSON_DeleteItemFromObject(root, "sessions");
        sessions = cJSON_AddArrayToObject(root, "sessions");
        if (!sessions) {
            cJSON_Delete(root);
            rtos_mutex_give(s_mutex);
            return RTK_FAIL;
        }
        cJSON *cur = cJSON_GetObjectItem(root, "current");
        if (cur && cJSON_IsString(cur)) {
            cJSON_AddItemToArray(sessions, cJSON_CreateString(cur->valuestring));
        }
    }

    if (alias && alias[0]) {
        strlcpy(actual, alias, sizeof(actual));
    } else {
        auto_alias(sessions, actual, sizeof(actual));
    }

    if (array_has_string(sessions, actual)) {
        cJSON_Delete(root);
        rtos_mutex_give(s_mutex);
        rtos_mem_free(map_path);
        return CAP_SESSION_ERR_CONFLICT;
    }

    cJSON_AddItemToArray(sessions, cJSON_CreateString(actual));
    cJSON_DeleteItemFromObject(root, "current");
    cJSON_AddStringToObject(root, "current", actual);

    json_str = serialize_chat_map(root);  /* serialize under mutex */
    cJSON_Delete(root);
    rtos_mutex_give(s_mutex);             /* release before file I/O */

    if (json_str) {
        rc = write_chat_map_str(map_path, json_str);
        cJSON_free(json_str);
    }

    if (rc == RTK_SUCCESS) {
        RTK_LOGI(TAG, "new '%s' for %s:%s\n", actual, channel, chat_id);
        if (out_alias && out_size > 0) {
            strlcpy(out_alias, actual, out_size);
        }
    }
    rtos_mem_free(map_path);
    return rc;
}

int cap_session_mgr_resume(const char *channel, const char *chat_id,
                           const char *alias)
{
    if (!s_initialized) return RTK_FAIL;
    if (!channel || !channel[0] || !chat_id || !chat_id[0]) return RTK_ERR_BADARG;
    if (!alias || !alias[0]) return RTK_ERR_BADARG;

    char *map_path = (char *)rtos_mem_malloc(320);
    if (!map_path) return RTK_FAIL;
    char *json_str = NULL;

    /* Read outside mutex — avoids holding lock during flash I/O.  */
    cJSON *root = read_chat_map(channel, chat_id, map_path, 320);
    if (!root) {
        rtos_mem_free(map_path);
        return CAP_SESSION_ERR_NOT_FOUND;
    }

    cJSON *sessions = cJSON_GetObjectItem(root, "sessions");
    if (!sessions || !array_has_string(sessions, alias)) {
        cJSON_Delete(root);
        rtos_mem_free(map_path);
        return CAP_SESSION_ERR_NOT_FOUND;
    }

    rtos_mutex_take(s_mutex, 0xFFFFFFFFUL);

    cJSON_DeleteItemFromObject(root, "current");
    cJSON_AddStringToObject(root, "current", alias);

    json_str = serialize_chat_map(root);
    cJSON_Delete(root);
    rtos_mutex_give(s_mutex);

    int rc = RTK_FAIL;
    if (json_str) {
        rc = write_chat_map_str(map_path, json_str);
        cJSON_free(json_str);
    }

    if (rc == RTK_SUCCESS) {
        RTK_LOGI(TAG, "resumed '%s' for %s:%s\n", alias, channel, chat_id);
    }
    rtos_mem_free(map_path);
    return rc;
}

int cap_session_mgr_list(const char *channel, const char *chat_id,
                         char *out_buf, size_t out_size)
{
    if (!out_buf || out_size == 0) return 0;
    out_buf[0] = '\0';
    if (!s_initialized || !channel || !channel[0] || !chat_id || !chat_id[0]) return 0;

    /* Heap-allocate path to stay within 128-byte stack-local limit. */
    char *map_path = (char *)rtos_mem_malloc(320);
    if (!map_path) return 0;
    chat_map_path_for(channel, chat_id, map_path, 320);

    /* Read outside the mutex — list is read-only; cJSON_Parse handles a partial
     * read gracefully by returning NULL, which we already handle below. */
    cJSON *root = read_chat_map_at(map_path);
    rtos_mem_free(map_path);
    if (!root) {
        /* No chat map file yet — implicit default session always exists. */
        static const char s[] = "\xe2\x80\xa2 default (current)";
        size_t n = sizeof(s) - 1;
        if (n < out_size) {
            memcpy(out_buf, s, n + 1);
            return (int)n;
        }
        return 0;
    }

    const char *current_alias = "default";
    cJSON *cur = cJSON_GetObjectItem(root, "current");
    if (cur && cJSON_IsString(cur) && cur->valuestring[0]) {
        current_alias = cur->valuestring;
    }

    cJSON *sessions = cJSON_GetObjectItem(root, "sessions");
    size_t written = 0;
    if (sessions && cJSON_IsArray(sessions)) {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, sessions) {
            if (!cJSON_IsString(item)) continue;
            const char *name = item->valuestring;
            bool is_cur = (strcmp(name, current_alias) == 0);
            int n = snprintf(out_buf + written, out_size - written,
                             "\xe2\x80\xa2 %s%s\n", name, is_cur ? " (current)" : "");
            if (n <= 0 || (size_t)n >= out_size - written) {
                written = out_size - 1;
                break;
            }
            written += (size_t)n;
        }
    }

    /* Strip trailing newline */
    if (written > 0 && out_buf[written - 1] == '\n') {
        out_buf[--written] = '\0';
    }

    /* Repair: if sessions array was empty or absent, show the current alias so
     * that /list never returns "No sessions found" when there IS an active session. */
    if (written == 0 && current_alias[0]) {
        int n = snprintf(out_buf, out_size,
                         "\xe2\x80\xa2 %s (current)", current_alias);
        if (n > 0 && (size_t)n < out_size)
            written = (size_t)n;
    }

    cJSON_Delete(root);
    return (int)written;
}

int cap_session_mgr_rename(const char *channel, const char *chat_id,
                           const char *new_alias)
{
    if (!s_initialized) return RTK_FAIL;
    if (!channel || !channel[0] || !chat_id || !chat_id[0]) return RTK_ERR_BADARG;
    if (!is_valid_alias(new_alias)) return RTK_ERR_BADARG;

    char *map_path = (char *)rtos_mem_malloc(320);
    if (!map_path) return RTK_FAIL;
    char old_alias[40];
    char *json_str = NULL;

    /* Read outside mutex — avoids holding lock during flash I/O.  */
    cJSON *root = read_chat_map(channel, chat_id, map_path, 320);
    if (!root) {
        rtos_mem_free(map_path);
        return RTK_FAIL;
    }

    cJSON *sessions = cJSON_GetObjectItem(root, "sessions");
    if (!sessions || !cJSON_IsArray(sessions)) {
        cJSON_Delete(root);
        rtos_mem_free(map_path);
        return RTK_FAIL;
    }

    if (array_has_string(sessions, new_alias)) {
        cJSON_Delete(root);
        rtos_mem_free(map_path);
        return CAP_SESSION_ERR_CONFLICT;
    }

    cJSON *cur_item = cJSON_GetObjectItem(root, "current");
    if (!cur_item || !cJSON_IsString(cur_item)) {
        cJSON_Delete(root);
        rtos_mem_free(map_path);
        return RTK_FAIL;
    }

    rtos_mutex_take(s_mutex, 0xFFFFFFFFUL);
    strlcpy(old_alias, cur_item->valuestring, sizeof(old_alias));

    /* Rebuild sessions array with new_alias replacing old_alias */
    cJSON *new_sessions = cJSON_CreateArray();
    if (!new_sessions) {
        cJSON_Delete(root);
        rtos_mutex_give(s_mutex);
        rtos_mem_free(map_path);
        return RTK_FAIL;
    }
    bool replaced = false;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, sessions) {
        if (!cJSON_IsString(item)) continue;
        const char *entry;
        if (strcmp(item->valuestring, old_alias) == 0) {
            entry = new_alias;
            replaced = true;
        } else {
            entry = item->valuestring;
        }
        cJSON_AddItemToArray(new_sessions, cJSON_CreateString(entry));
    }

    if (!replaced) {
        cJSON_Delete(new_sessions);
        cJSON_Delete(root);
        rtos_mutex_give(s_mutex);
        rtos_mem_free(map_path);
        return RTK_FAIL;
    }

    cJSON_DeleteItemFromObject(root, "sessions");
    cJSON_AddItemToObject(root, "sessions", new_sessions);
    cJSON_DeleteItemFromObject(root, "current");
    cJSON_AddStringToObject(root, "current", new_alias);

    json_str = serialize_chat_map(root);
    cJSON_Delete(root);
    rtos_mutex_give(s_mutex);

    int rc = RTK_FAIL;
    if (json_str) {
        rc = write_chat_map_str(map_path, json_str);
        cJSON_free(json_str);
    }

    if (rc == RTK_SUCCESS) {
        RTK_LOGI(TAG, "renamed '%s' to '%s' for %s:%s\n",
                 old_alias, new_alias, channel, chat_id);
    }
    rtos_mem_free(map_path);
    return rc;
}

int cap_session_mgr_delete(const char *channel, const char *chat_id,
                           const char *alias)
{
    if (!s_initialized) return RTK_FAIL;
    if (!channel || !channel[0] || !chat_id || !chat_id[0]) return RTK_ERR_BADARG;
    if (!alias || !alias[0]) return RTK_ERR_BADARG;

    char *map_path = (char *)rtos_mem_malloc(320);
    if (!map_path) return RTK_FAIL;
    /* Allocate sid buffer early so a malloc failure causes a clean early exit
     * rather than a partial success (alias removed from map but history kept). */
    char *sid = (char *)rtos_mem_malloc(256);
    if (!sid) { rtos_mem_free(map_path); return RTK_FAIL; }
    char *json_str = NULL;
    bool delete_file = false;

    /* Read outside mutex — avoids holding lock during flash I/O.  */
    cJSON *root = read_chat_map(channel, chat_id, map_path, 320);
    if (!root) {
        rtos_mem_free(sid);
        rtos_mem_free(map_path);
        return CAP_SESSION_ERR_NOT_FOUND;
    }

    cJSON *sessions = cJSON_GetObjectItem(root, "sessions");
    if (!sessions || !array_has_string(sessions, alias)) {
        cJSON_Delete(root);
        rtos_mem_free(sid);
        rtos_mem_free(map_path);
        return CAP_SESSION_ERR_NOT_FOUND;
    }

    cJSON *cur = cJSON_GetObjectItem(root, "current");
    bool is_current = (cur && cJSON_IsString(cur) && strcmp(cur->valuestring, alias) == 0);

    rtos_mutex_take(s_mutex, 0xFFFFFFFFUL);

    /* Remove alias from sessions array */
    int arr_size = cJSON_GetArraySize(sessions);
    for (int i = 0; i < arr_size; i++) {
        cJSON *it = cJSON_GetArrayItem(sessions, i);
        if (it && cJSON_IsString(it) && strcmp(it->valuestring, alias) == 0) {
            cJSON_DeleteItemFromArray(sessions, i);
            break;
        }
    }

    if (cJSON_GetArraySize(sessions) == 0) {
        delete_file = true;
        cJSON_Delete(root);
    } else {
        /* If the deleted session was current, switch to the first remaining alias */
        if (is_current) {
            cJSON *first = cJSON_GetArrayItem(sessions, 0);
            if (first && cJSON_IsString(first)) {
                cJSON_DeleteItemFromObject(root, "current");
                cJSON_AddStringToObject(root, "current", first->valuestring);
            }
        }
        json_str = serialize_chat_map(root);
        cJSON_Delete(root);
    }

    rtos_mutex_give(s_mutex);

    /* File operations outside the mutex */
    int rc = RTK_FAIL;
    if (delete_file) {
        remove(map_path);
        rc = RTK_SUCCESS;
    } else if (json_str) {
        rc = write_chat_map_str(map_path, json_str);
        cJSON_free(json_str);
    }

    if (rc == RTK_SUCCESS) {
        build_sid(channel, chat_id, alias, sid, 256);
        claw_memory_clear_session(sid);
        RTK_LOGI(TAG, "deleted '%s' for %s:%s\n", alias, channel, chat_id);
    }
    rtos_mem_free(sid);
    rtos_mem_free(map_path);
    return rc;
}

int cap_session_mgr_clear_chat(const char *channel, const char *chat_id)
{
    if (!s_initialized) return RTK_FAIL;
    if (!channel || !channel[0] || !chat_id || !chat_id[0]) return RTK_ERR_BADARG;

    char *map_path = (char *)rtos_mem_malloc(320);
    if (!map_path) return RTK_FAIL;
    /* Allocate sid early so a malloc failure causes a clean early exit. */
    char *sid = (char *)rtos_mem_malloc(256);
    if (!sid) { rtos_mem_free(map_path); return RTK_FAIL; }
    char current_alias[40] = "default";

    /* Read outside mutex — avoids holding lock during flash I/O.  */
    cJSON *root = read_chat_map(channel, chat_id, map_path, 320);
    rtos_mem_free(map_path);
    if (root) {
        cJSON *cur = cJSON_GetObjectItem(root, "current");
        if (cur && cJSON_IsString(cur) && cur->valuestring[0]) {
            strlcpy(current_alias, cur->valuestring, sizeof(current_alias));
        }
        cJSON_Delete(root);
    }

    build_sid(channel, chat_id, current_alias, sid, 256);
    claw_memory_clear_session(sid);
    rtos_mem_free(sid);
    RTK_LOGI(TAG, "cleared '%s' for %s:%s\n", current_alias, channel, chat_id);
    return RTK_SUCCESS;
}

int cap_session_mgr_get_current(const char *channel, const char *chat_id,
                                char *out_buf, size_t out_size)
{
    if (!out_buf || out_size == 0) return RTK_ERR_BADARG;
    if (!s_initialized) return RTK_FAIL;
    if (!channel || !channel[0] || !chat_id || !chat_id[0]) return RTK_ERR_BADARG;

    char *map_path = (char *)rtos_mem_malloc(320);
    if (!map_path) return RTK_FAIL;
    /* Read outside mutex — avoids holding lock during flash I/O.  */
    cJSON *root = read_chat_map(channel, chat_id, map_path, 320);
    rtos_mem_free(map_path);

    if (!root) return RTK_FAIL;

    int rc = RTK_FAIL;
    cJSON *cur = cJSON_GetObjectItem(root, "current");
    if (cur && cJSON_IsString(cur) && cur->valuestring[0]) {
        strlcpy(out_buf, cur->valuestring, out_size);
        rc = RTK_SUCCESS;
    }
    cJSON_Delete(root);
    return rc;
}

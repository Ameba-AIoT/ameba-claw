/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_session_mgr.h"
#include "claw_compat.h"
#include "diag.h"
#include <cJSON.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* Module tag                                                           */
/* ------------------------------------------------------------------ */

#define TAG "cap_session_mgr"

/* ------------------------------------------------------------------ */
/* Module state                                                         */
/* ------------------------------------------------------------------ */

static char              s_session_root[128];
static char              s_mapping_dir[160];   /* {session_root}/chat_map */
static rtos_mutex_t s_mutex;
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

/* Sanitize a string so it is safe as part of a filename. Replaces any
 * character that is not [A-Za-z0-9._-] with '_'. This is a strict
 * superset of the rules used by claw_memory.c::session_file_path (which
 * only replaces '/' and ':'), so a session_id built here always survives
 * that downstream path build without further mangling. Mirror this set
 * if either side ever extends — the two layers must stay aligned. */
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

/* Sanitize chat_id for embedding inside session_id — strips chars that
 * would be unsafe in a VFS filename (whitespace, backslash, control bytes). */
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

/* Resolve the chat_map JSON path for a (channel, chat_id) pair. */
static void chat_map_path_for(const char *channel, const char *chat_id,
                              char *out, size_t out_size)
{
    char chat_key[200];
    char sanitized[33];
    DiagSnPrintf(chat_key, sizeof(chat_key), "%s:%s",
                 channel ? channel : "", chat_id ? chat_id : "");
    sanitize_filename(chat_key, sanitized, sizeof(sanitized) - 1);
    uint32_t h = hash_str(chat_key);
    DiagSnPrintf(out, out_size, "%s/s_%s_%08x.json",
                 s_mapping_dir, sanitized, (unsigned)h);
}

/* VFS mkdir returns LFS error codes (-17 = already exists), not POSIX errno. */
#define MKDIR_ERR_EXIST  (-17)

/* Attempt to create a single directory; ignore EEXIST. */
static void try_mkdir(const char *path)
{
    int ret = mkdir(path, 0755);
    if (ret != 0 && ret != MKDIR_ERR_EXIST) {
        DiagPrintf("[W][%s] mkdir('%s') failed ret=%d\n", TAG, path, ret);
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int cap_session_mgr_init(const char *session_root_dir)
{
    if (!session_root_dir || !session_root_dir[0]) {
        return RTK_ERR_BADARG;
    }

    /* Store root path (truncate safely) */
    strlcpy(s_session_root, session_root_dir, sizeof(s_session_root));

    /* Derive mapping dir */
    DiagSnPrintf(s_mapping_dir, sizeof(s_mapping_dir), "%s/chat_map", s_session_root);

    /* Create directories (errors are non-fatal — directory may already exist) */
    try_mkdir(s_session_root);
    try_mkdir(s_mapping_dir);

    /* Create mutex */
    int err = rtos_mutex_create(&s_mutex);
    if (err != RTK_SUCCESS) {
        DiagPrintf("[E][%s] Failed to create mutex\n", TAG);
        return RTK_ERR_NOMEM;
    }

    s_initialized = true;
    DiagPrintf("[%s] Initialized (root=%s, map=%s)\n", TAG, s_session_root, s_mapping_dir);
    return RTK_SUCCESS;
}

size_t cap_session_mgr_build_session_id(const claw_event_t *event,
                                         char *buf, size_t buf_size,
                                         void *user_ctx)
{
    const char *channel;
    const char *chat_id;
    char        chat_key[200];
    char        map_path[320];
    int         version = 1;

    (void)user_ctx;

    if (!event || !buf || buf_size == 0) {
        return 0;
    }

    channel = event->source_channel[0] ? event->source_channel : NULL;
    chat_id = event->chat_id[0]        ? event->chat_id        : NULL;

    /* ---- Fall back to "global" when no chat context ---- */
    if (!channel && !chat_id) {
        size_t n = (size_t)DiagSnPrintf(buf, buf_size, "global");
        return n < buf_size ? n : buf_size - 1;
    }

    /* ---- Build chat_key + resolve mapping file path ---- */
    DiagSnPrintf(chat_key, sizeof(chat_key), "%s:%s",
                 channel ? channel : "", chat_id ? chat_id : "");
    chat_map_path_for(channel, chat_id, map_path, sizeof(map_path));

    /* ---- Read or create mapping file (mutex-protected) ---- */
    if (s_initialized && s_mutex) {
        rtos_mutex_take(s_mutex, 0xFFFFFFFFUL);
    }

    {
        FILE *f = fopen(map_path, "r");
        if (f) {
            /* File exists: parse version */
            char   fbuf[128];
            size_t nread = fread(fbuf, 1, sizeof(fbuf) - 1, f);
            fclose(f);
            fbuf[nread] = '\0';

            cJSON *root = cJSON_Parse(fbuf);
            if (root) {
                cJSON *ver_item = cJSON_GetObjectItem(root, "version");
                if (ver_item && cJSON_IsNumber(ver_item)) {
                    version = ver_item->valueint;
                }
                cJSON_Delete(root);
            }
        } else {
            /* File does not exist: create it with version=1 */
            FILE *wf = fopen(map_path, "w");
            if (wf) {
                cJSON *root = cJSON_CreateObject();
                if (root) {
                    cJSON_AddStringToObject(root, "chat_key", chat_key);
                    cJSON_AddNumberToObject(root, "version", 1);
                    char *json_str = cJSON_PrintUnformatted(root);
                    if (json_str) {
                        fwrite(json_str, 1, strlen(json_str), wf);
                        free(json_str);
                    }
                    cJSON_Delete(root);
                }
                fclose(wf);
                DiagPrintf("[%s] Created mapping '%s' v1\n", TAG, chat_key);
            } else {
                DiagPrintf("[W][%s] Cannot create map file: %s\n", TAG, map_path);
            }
            version = 1;
        }
    }

    if (s_initialized && s_mutex) {
        rtos_mutex_give(s_mutex);
    }

    /* ---- Format session ID with sanitized fields. Downstream (claw_memory
     * session_file_path) only handles ':' and '/'; we strip everything else
     * here so the same string can be safely round-tripped to a filename. */
    {
        char ch_safe[64];
        char id_safe[128];
        sanitize_session_field(channel, ch_safe, sizeof(ch_safe));
        sanitize_session_field(chat_id, id_safe, sizeof(id_safe));
        size_t n = (size_t)DiagSnPrintf(buf, buf_size, "%s:%s:v%d",
                                        ch_safe, id_safe, version);
        return n < buf_size ? n : buf_size - 1;
    }
}

int cap_session_mgr_bump_version(const char *channel, const char *chat_id)
{
    if (!s_initialized) return RTK_FAIL;
    if ((!channel || !channel[0]) && (!chat_id || !chat_id[0])) {
        /* "global" session has no mapping file — nothing to bump. */
        return RTK_ERR_BADARG;
    }

    char chat_key[200];
    char map_path[320];
    snprintf(chat_key, sizeof(chat_key), "%s:%s",
             channel ? channel : "",
             chat_id ? chat_id : "");
    chat_map_path_for(channel, chat_id, map_path, sizeof(map_path));

    int rc = RTK_FAIL;
    int old_version = 0, new_version = 0;

    if (s_mutex) rtos_mutex_take(s_mutex, 0xFFFFFFFFUL);

    /* Read current version (default 1 — first bump produces v2). */
    cJSON *root = NULL;
    FILE *f = fopen(map_path, "r");
    if (f) {
        char fbuf[128];
        size_t nread = fread(fbuf, 1, sizeof(fbuf) - 1, f);
        fclose(f);
        fbuf[nread] = '\0';
        root = cJSON_Parse(fbuf);
    }
    if (!root) {
        /* No existing mapping: build one from scratch. We deliberately do
         * NOT add a "version" field here — the unconditional add below
         * sets it. cJSON_AddNumberToObject does NOT replace duplicate
         * keys; it appends, and `cJSON_GetObjectItem` returns the first
         * match. Adding "version=1" here AND "version=new" later would
         * persist two entries and pin the on-disk version forever. */
        root = cJSON_CreateObject();
        if (root) {
            cJSON_AddStringToObject(root, "chat_key", chat_key);
        }
        old_version = 1;   /* logical default for "no prior file" */
    } else {
        cJSON *ver_item = cJSON_GetObjectItem(root, "version");
        if (ver_item && cJSON_IsNumber(ver_item)) {
            old_version = ver_item->valueint;
        } else {
            old_version = 1;
        }
        /* Strip ALL "version" entries — files written by the buggy
         * earlier revision can have duplicates accumulated, and a
         * single DeleteItemFromObject only removes the first match.
         * Loop until none remains so the final add lands as the
         * unique authoritative entry. */
        while (cJSON_GetObjectItem(root, "version")) {
            cJSON_DeleteItemFromObject(root, "version");
        }
        if (!cJSON_GetObjectItem(root, "chat_key")) {
            cJSON_AddStringToObject(root, "chat_key", chat_key);
        }
    }
    if (!root) goto out;

    new_version = old_version + 1;
    cJSON_AddNumberToObject(root, "version", new_version);

    /* Write back. The chat_map files are tiny (<128 bytes) and a power
     * loss here just means the next build_session_id falls back to v1
     * for this chat — at worst the user sees one extra "fresh session".
     * Atomic .bak protocol would be overkill for this size class. */
    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) goto out;
    FILE *wf = fopen(map_path, "w");
    if (wf) {
        size_t slen = strlen(json_str);
        size_t w = fwrite(json_str, 1, slen, wf);
        fclose(wf);
        rc = (w == slen) ? RTK_SUCCESS : RTK_FAIL;
    } else {
        DiagPrintf("[W][%s] bump_version: cannot open %s\n", TAG, map_path);
    }
    free(json_str);

out:
    if (root) cJSON_Delete(root);
    if (s_mutex) rtos_mutex_give(s_mutex);

    if (rc == RTK_SUCCESS) {
        DiagPrintf("[%s] bump_version '%s' v%d → v%d\n",
                   TAG, chat_key, old_version, new_version);
    }
    return rc;
}

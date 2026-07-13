/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_files.h"
#include "claw_cap.h"
#include <cJSON.h>
#include "platform_stdlib.h"
#include "vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "ameba_claw_defs.h"
#include "claw_utf8.h"
#include "cap_lua.h"

#define TAG "cap_files"

/* Stringify the read limit so the LLM-facing description tracks the macro and
 * can never drift from the actual byte cap (see CAP_FILES_MAX_READ_KB). */
#define CAP_FILES_STR2(x) #x
#define CAP_FILES_STR(x)  CAP_FILES_STR2(x)

static size_t s_max_read = CAP_FILES_MAX_READ_SIZE;

static int path_is_safe(const char *path)
{
    if (!path || path[0] == '\0') return 0;
    if (strstr(path, "..")) return 0;

    /* Block LLM access to sensitive system files.
     * The LLM has dedicated capabilities (memory_store/recall) for memory and
     * session operations — direct file access is unnecessary and risks leaking
     * API keys, WiFi credentials, and conversation history. */
    if (strncmp(path, "vfs:", 4) == 0) {
        const char *vp = path + 4;
        while (*vp == '/') vp++;  /* normalize: skip leading / */

        /* vfs:claw_config.json — API keys, WiFi credentials, etc. */
        if (strcmp(vp, "claw_config.json") == 0) return 0;

        /* vfs:wechat_token — WeChat bot OAuth token */
        if (strcmp(vp, "wechat_token") == 0) return 0;

        /* vfs:/session/ — conversation history */
        if (strncmp(vp, "session", 7) == 0 &&
            (vp[7] == '\0' || vp[7] == '/')) return 0;

        /* vfs:/memory/ — long-term memory stores and session data */
        if (strncmp(vp, "memory", 6) == 0 &&
            (vp[6] == '\0' || vp[6] == '/')) return 0;

        /* vfs:/ profile files — managed exclusively via memory capabilities */
        if (strcmp(vp, "AGENTS.md")   == 0) return 0;
        if (strcmp(vp, "SOUL.md")     == 0) return 0;
        if (strcmp(vp, "IDENTITY.md") == 0) return 0;
        if (strcmp(vp, "USER.md")     == 0) return 0;
        if (strcmp(vp, "MEMORY.md")   == 0) return 0;
    }

    return 1;
}

/* Returns 1 if the path is under vfs:/inbox/ (attachment download area).
 * These files may be stat'd but not read — binary media files would produce
 * garbled output and waste context window. */
static int path_is_inbox(const char *path)
{
    if (!path) return 0;
    if (strncmp(path, "vfs:", 4) != 0) return 0;
    const char *vp = path + 4;
    while (*vp == '/') vp++;
    return (strncmp(vp, "inbox", 5) == 0 &&
            (vp[5] == '\0' || vp[5] == '/'));
}

/* Write red line (12_skill_lua_separation.md §D.5/§E.1): the built-in content
 * mounted at rolfs:/ is a physically read-only littlefs image, so the LLM must
 * never be able to write/delete/move/copy-into it. The ROLFS adapter already
 * rejects prog/erase, but we reject the path at policy level too so the LLM
 * gets a clear error instead of a low-level open failure. read_file on rolfs:/
 * stays allowed (built-in skills/libs are meant to be read). */
static int path_is_writable(const char *path)
{
    if (!path_is_safe(path)) return 0;
    if (strncmp(path, "rolfs:", 6) == 0) return 0;
    return 1;
}

static int execute_read_file(const char *input_json,
                             const claw_cap_call_context_t *ctx,
                             char **output)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        *output = strdup("{\"error\":\"invalid json\"}");
        return RTK_SUCCESS;
    }

    cJSON *path_item = cJSON_GetObjectItem(root, "path");
    const char *path = (path_item && cJSON_IsString(path_item)) ? path_item->valuestring : NULL;

    if (!path || !path_is_safe(path)) {
        *output = strdup("{\"error\":\"invalid path\"}");
        cJSON_Delete(root);
        return RTK_SUCCESS;
    }

    /* Attachments in vfs:/inbox/ are binary media files — reading them would
     * produce garbled output.  Use file_stat to inspect metadata instead. */
    if (path_is_inbox(path)) {
        *output = strdup("{\"error\":\"binary attachment: use file_stat to inspect, im_send_media to forward\"}");
        cJSON_Delete(root);
        return RTK_SUCCESS;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "cannot open file");
        cJSON_AddStringToObject(e, "path", path);
        *output = cJSON_PrintUnformatted(e);
        cJSON_Delete(e);
        cJSON_Delete(root);
        return RTK_SUCCESS;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz < 0) sz = 0;
    size_t read_sz = (size_t)sz < s_max_read ? (size_t)sz : s_max_read;
    /* Extra bytes: UTF-8 walkback may shrink by up to 3, suffix + newline */
    size_t sfx_reserve = sizeof(TOOL_RESULT_TRUNCATION_SUFFIX) + 2;
    char *buf = rtos_mem_malloc(read_sz + sfx_reserve);
    if (!buf) {
        fclose(f);
        *output = strdup("{\"error\":\"out of memory\"}");
        cJSON_Delete(root);
        return RTK_SUCCESS;
    }

    size_t n = fread(buf, 1, read_sz, f);
    fclose(f);

    /* Walk back to a valid UTF-8 character boundary. */
    n = claw_utf8_safe_len(buf, n);

    bool truncated = ((size_t)sz > s_max_read);
    if (truncated) {
        /* Append a human-readable notice so the LLM knows the file was cut. */
        buf[n++] = '\n';
        const char *sfx = TOOL_RESULT_TRUNCATION_SUFFIX;
        size_t sfx_len = strlen(sfx);
        memcpy(buf + n, sfx, sfx_len);
        n += sfx_len;
    }
    buf[n] = '\0';

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "path", path);
    cJSON_AddNumberToObject(resp, "size", (double)sz);
    cJSON_AddStringToObject(resp, "content", buf);
    if (truncated) {
        cJSON_AddTrueToObject(resp, "truncated");
    }
    *output = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    rtos_mem_free(buf);
    cJSON_Delete(root);
    return *output ? RTK_SUCCESS : RTK_ERR_NOMEM;
}

static int execute_file_write(const char *input_json,
                              const claw_cap_call_context_t *ctx,
                              char **output)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        *output = strdup("{\"error\":\"invalid json\"}");
        return RTK_SUCCESS;
    }

    cJSON *path_item    = cJSON_GetObjectItem(root, "path");
    cJSON *content_item = cJSON_GetObjectItem(root, "content");
    cJSON *append_item  = cJSON_GetObjectItem(root, "append");

    const char *path    = (path_item && cJSON_IsString(path_item)) ? path_item->valuestring : NULL;
    const char *content = (content_item && cJSON_IsString(content_item)) ? content_item->valuestring : "";
    int append = (append_item && cJSON_IsTrue(append_item)) ? 1 : 0;

    if (!path || !path_is_writable(path)) {
        *output = strdup("{\"error\":\"invalid path\"}");
        cJSON_Delete(root);
        return RTK_SUCCESS;
    }

    FILE *f = fopen(path, append ? "a" : "w");
    if (!f) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "cannot open file for write");
        cJSON_AddStringToObject(e, "path", path);
        *output = cJSON_PrintUnformatted(e);
        cJSON_Delete(e);
        cJSON_Delete(root);
        return RTK_SUCCESS;
    }

    size_t len = strlen(content);
    fwrite(content, 1, len, f);
    fclose(f);

    const char *fmt = "{\"status\":\"ok\",\"path\":\"%s\",\"bytes_written\":%zu}";
    int n = DiagSnPrintf(NULL, 0, fmt, path, len);
    *output = malloc((size_t)n + 1);
    if (*output) DiagSnPrintf(*output, (size_t)n + 1, fmt, path, len);
    cJSON_Delete(root);
    return *output ? RTK_SUCCESS : RTK_ERR_NOMEM;
}

static int execute_file_delete(const char *input_json,
                               const claw_cap_call_context_t *ctx,
                               char **output)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        *output = strdup("{\"error\":\"invalid json\"}");
        return RTK_SUCCESS;
    }

    cJSON *path_item = cJSON_GetObjectItem(root, "path");
    const char *path = (path_item && cJSON_IsString(path_item)) ? path_item->valuestring : NULL;

    if (!path || !path_is_writable(path)) {
        *output = strdup("{\"error\":\"invalid path\"}");
        cJSON_Delete(root);
        return RTK_SUCCESS;
    }

    /* For .lua files: use cap_lua_file_remove() which stops any running
     * lua_async job first.  Direct remove() on .lua files is unsafe. */
    int ret;
    int saved_errno = 0;
    size_t plen = strlen(path);
    if (plen > 4 && strcmp(path + plen - 4, ".lua") == 0) {
        ret = cap_lua_file_remove(path);
    } else {
        ret = remove(path);
        saved_errno = errno;  /* capture before any other call can clobber it */
    }
    cJSON *resp = cJSON_CreateObject();
    if (ret != 0) {
        if (saved_errno == ENOENT) {
            /* File already absent — treat as success (idempotent delete). */
            cJSON_AddStringToObject(resp, "status", "ok");
            cJSON_AddStringToObject(resp, "path", path);
            cJSON_AddStringToObject(resp, "note", "not_found");
        } else {
            cJSON_AddStringToObject(resp, "error", "delete failed");
            cJSON_AddStringToObject(resp, "path", path);
        }
    } else {
        cJSON_AddStringToObject(resp, "status", "ok");
        cJSON_AddStringToObject(resp, "path", path);
    }
    *output = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    cJSON_Delete(root);
    return *output ? RTK_SUCCESS : RTK_ERR_NOMEM;
}

static int execute_list_dir(const char *input_json,
                             const claw_cap_call_context_t *ctx,
                             char **output)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(input_json);
    const char *dir_path = "/";
    if (root) {
        cJSON *dir_item = cJSON_GetObjectItem(root, "path");
        if (dir_item && cJSON_IsString(dir_item) && path_is_safe(dir_item->valuestring)) {
            dir_path = dir_item->valuestring;
        }
    }

    /* "/" redirects to vfs:/ — the only listable user filesystem */
    int redirected = (strcmp(dir_path, "/") == 0);
    if (redirected) {
        dir_path = "vfs:/";
    }
    void *dir = opendir(dir_path);
    if (!dir) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", redirected
            ? "cannot open directory"
            : "cannot open directory — use vfs:/ for user files (rolfs:/ is not listable)");
        cJSON_AddStringToObject(e, "path", dir_path);
        *output = cJSON_PrintUnformatted(e);
        cJSON_Delete(e);
        if (root) cJSON_Delete(root);
        return RTK_SUCCESS;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "path", dir_path);
    cJSON *files = cJSON_CreateArray();
    cJSON_AddItemToObject(resp, "files", files);

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "name", ent->d_name);
        cJSON_AddStringToObject(entry, "type",
            (ent->d_type == DT_DIR) ? "dir" : "file");
        cJSON_AddItemToArray(files, entry);
    }
    closedir(dir);

    *output = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (root) cJSON_Delete(root);
    return *output ? RTK_SUCCESS : RTK_ERR_NOMEM;
}

static int execute_file_move(const char *input_json,
                             const claw_cap_call_context_t *ctx,
                             char **output)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        *output = strdup("{\"error\":\"invalid json\"}");
        return RTK_SUCCESS;
    }

    cJSON *src_item = cJSON_GetObjectItem(root, "src");
    cJSON *dst_item = cJSON_GetObjectItem(root, "dst");
    const char *src = (src_item && cJSON_IsString(src_item)) ? src_item->valuestring : NULL;
    const char *dst = (dst_item && cJSON_IsString(dst_item)) ? dst_item->valuestring : NULL;

    if (!src || !dst || !path_is_writable(src) || !path_is_writable(dst)) {
        *output = strdup("{\"error\":\"invalid path\"}");
        cJSON_Delete(root);
        return RTK_SUCCESS;
    }

    int ret = rename(src, dst);
    cJSON *resp = cJSON_CreateObject();
    if (ret != 0) {
        cJSON_AddStringToObject(resp, "error", "move failed");
        cJSON_AddStringToObject(resp, "src", src);
        cJSON_AddStringToObject(resp, "dst", dst);
    } else {
        cJSON_AddStringToObject(resp, "status", "ok");
        cJSON_AddStringToObject(resp, "src", src);
        cJSON_AddStringToObject(resp, "dst", dst);
    }
    *output = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    cJSON_Delete(root);
    return *output ? RTK_SUCCESS : RTK_ERR_NOMEM;
}

static int execute_copy_file(const char *input_json,
                             const claw_cap_call_context_t *ctx,
                             char **output)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        *output = strdup("{\"error\":\"invalid json\"}");
        return RTK_SUCCESS;
    }

    cJSON *src_item = cJSON_GetObjectItem(root, "src");
    cJSON *dst_item = cJSON_GetObjectItem(root, "dst");
    const char *src = (src_item && cJSON_IsString(src_item)) ? src_item->valuestring : NULL;
    const char *dst = (dst_item && cJSON_IsString(dst_item)) ? dst_item->valuestring : NULL;

    /* src is only read (copying FROM rolfs:/ built-ins is allowed); dst is written
     * so it must pass the write red line. */
    if (!src || !dst || !path_is_safe(src) || !path_is_writable(dst)) {
        *output = strdup("{\"error\":\"invalid path\"}");
        cJSON_Delete(root);
        return RTK_SUCCESS;
    }

    FILE *fsrc = fopen(src, "rb");
    if (!fsrc) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "cannot open source");
        cJSON_AddStringToObject(e, "src", src);
        *output = cJSON_PrintUnformatted(e);
        cJSON_Delete(e);
        cJSON_Delete(root);
        return RTK_SUCCESS;
    }
    FILE *fdst = fopen(dst, "wb");
    if (!fdst) {
        fclose(fsrc);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "cannot open dest");
        cJSON_AddStringToObject(e, "dst", dst);
        *output = cJSON_PrintUnformatted(e);
        cJSON_Delete(e);
        cJSON_Delete(root);
        return RTK_SUCCESS;
    }

    char buf[256];
    size_t total = 0, n;
    while ((n = fread(buf, 1, sizeof(buf), fsrc)) > 0) {
        fwrite(buf, 1, n, fdst);
        total += n;
    }
    fclose(fsrc);
    fclose(fdst);

    const char *fmt = "{\"status\":\"ok\",\"src\":\"%s\",\"dst\":\"%s\",\"bytes_copied\":%zu}";
    int nn = DiagSnPrintf(NULL, 0, fmt, src, dst, total);
    *output = malloc((size_t)nn + 1);
    if (*output) DiagSnPrintf(*output, (size_t)nn + 1, fmt, src, dst, total);
    cJSON_Delete(root);
    return *output ? RTK_SUCCESS : RTK_ERR_NOMEM;
}

static int execute_file_stat(const char *input_json,
                              const claw_cap_call_context_t *ctx,
                              char **output)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        *output = strdup("{\"error\":\"invalid json\"}");
        return RTK_SUCCESS;
    }

    cJSON *path_item = cJSON_GetObjectItem(root, "path");
    const char *path = (path_item && cJSON_IsString(path_item)) ? path_item->valuestring : NULL;

    if (!path || !path_is_safe(path)) {
        *output = strdup("{\"error\":\"invalid path\"}");
        cJSON_Delete(root);
        return RTK_SUCCESS;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "error", "not found");
        cJSON_AddStringToObject(e, "path", path);
        *output = cJSON_PrintUnformatted(e);
        cJSON_Delete(e);
        cJSON_Delete(root);
        return RTK_SUCCESS;
    }

    /* Infer MIME from extension */
    const char *mime = "application/octet-stream";
    const char *dot  = strrchr(path, '.');
    if (dot) {
        const char *ext = dot + 1;
        if (strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0) mime = "image/jpeg";
        else if (strcmp(ext, "png")  == 0) mime = "image/png";
        else if (strcmp(ext, "gif")  == 0) mime = "image/gif";
        else if (strcmp(ext, "webp") == 0) mime = "image/webp";
        else if (strcmp(ext, "mp3")  == 0) mime = "audio/mpeg";
        else if (strcmp(ext, "wav")  == 0) mime = "audio/wav";
        else if (strcmp(ext, "ogg")  == 0) mime = "audio/ogg";
        else if (strcmp(ext, "aac")  == 0) mime = "audio/aac";
        else if (strcmp(ext, "amr")  == 0) mime = "audio/amr";
        else if (strcmp(ext, "m4a")  == 0) mime = "audio/mp4";
        else if (strcmp(ext, "mp4")  == 0) mime = "video/mp4";
        else if (strcmp(ext, "pdf")  == 0) mime = "application/pdf";
        else if (strcmp(ext, "txt")  == 0) mime = "text/plain";
        else if (strcmp(ext, "lua")  == 0) mime = "text/x-lua";
        else if (strcmp(ext, "md")   == 0) mime = "text/markdown";
        else if (strcmp(ext, "json") == 0) mime = "application/json";
    }

    /* Extract filename */
    const char *slash = strrchr(path, '/');
    const char *fname = slash ? slash + 1 : path;

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "path",       path);
    cJSON_AddStringToObject(resp, "name",       fname);
    cJSON_AddNumberToObject(resp, "size_bytes", (double)st.st_size);
    cJSON_AddStringToObject(resp, "mime",       mime);
    cJSON_AddBoolToObject  (resp, "is_dir",     S_ISDIR(st.st_mode));
    *output = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    cJSON_Delete(root);
    return *output ? RTK_SUCCESS : RTK_ERR_NOMEM;
}

static const claw_cap_descriptor_t s_caps[] = {
    {
        .id          = "file_read",
        .name        = "file_read",
        .family      = "files",
        .description = "Read file contents (max " CAP_FILES_STR(CAP_FILES_MAX_READ_KB) "KB). Args: path(string).",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"}},"
            "\"required\":[\"path\"]}",
        .execute     = execute_read_file,
    },
    {
        .id          = "file_write",
        .name        = "file_write",
        .family      = "files",
        .description = "Write or append file contents. Args: path(string), content(string), append(bool, optional).",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"File path\"},"
            "\"content\":{\"type\":\"string\",\"description\":\"Content to write\"},"
            "\"append\":{\"type\":\"boolean\",\"description\":\"true=append, false=overwrite\"}"
            "},"
            "\"required\":[\"path\",\"content\"]}",
        .execute     = execute_file_write,
    },
    {
        .id          = "file_delete",
        .name        = "file_delete",
        .family      = "files",
        .description = "Delete a file. Args: path(string).",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"}},"
            "\"required\":[\"path\"]}",
        .execute     = execute_file_delete,
    },
    {
        .id          = "file_list",
        .name        = "file_list",
        .family      = "files",
        .description = "List files and subdirectories. Args: path(string, optional, default \"/\").",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Directory path\"}}}",
        .execute     = execute_list_dir,
    },
    {
        .id          = "file_move",
        .name        = "file_move",
        .family      = "files",
        .description = "Move or rename a file. Args: src(string), dst(string).",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"src\":{\"type\":\"string\",\"description\":\"Source path\"},"
            "\"dst\":{\"type\":\"string\",\"description\":\"Destination path\"}"
            "},"
            "\"required\":[\"src\",\"dst\"]}",
        .execute     = execute_file_move,
    },
    {
        .id          = "file_copy",
        .name        = "file_copy",
        .family      = "files",
        .description = "Copy a file to a new path. Args: src(string), dst(string).",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"src\":{\"type\":\"string\",\"description\":\"Source path\"},"
            "\"dst\":{\"type\":\"string\",\"description\":\"Destination path\"}"
            "},"
            "\"required\":[\"src\",\"dst\"]}",
        .execute     = execute_copy_file,
    },
    {
        .id          = "file_stat",
        .name        = "file_stat",
        .family      = "files",
        .description =
            "Get file metadata (name, size_bytes, mime, is_dir). "
            "Works on any path including binary attachments under vfs:/inbox/. "
            "Does NOT read file contents. Args: path(string).",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"}},"
            "\"required\":[\"path\"]}",
        .execute     = execute_file_stat,
    },
};

static const claw_cap_group_t s_group = {
    .group_id         = "files",
    .plugin_name      = "cap_files",
    .version          = "1",
    .descriptors      = s_caps,
    .descriptor_count = 7,
};

int cap_files_init(const cap_files_config_t *cfg)
{
    if (cfg && cfg->max_read_size > 0) {
        s_max_read = cfg->max_read_size;
    }
    claw_cap_register_group(&s_group);
    return RTK_SUCCESS;
}

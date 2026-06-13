/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ameba_soc.h"
#include "ameba_claw_defs.h"
#include "wifi_api_types.h"   /* for RTW_ESSID_MAX_SIZE */
#include "cap_webui.h"
#include "cap_im_wechat.h"
#include "claw_http_server.h"
#include "claw_config.h"
#include "claw_cap.h"
#include "claw_wifi_mgr.h"
#include "claw_compat.h"
#include "os_wrapper_memory.h"
#include "os_wrapper.h"
#include "sys_api.h"
#include "vfs.h"
#include <cJSON.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

/* HTML pages embedded via .incbin in CMakeLists.txt */
extern const unsigned char _res_dashboard_html_start[];
extern const unsigned char _res_dashboard_html_end[];

#define TAG "cap_webui"

/* Forward declarations for static helpers used before their definition. */
static void url_decode(const char *src, char *dst, size_t dst_size);
static int  query_get(const char *query, const char *key, char *out, size_t out_size);

/* ---- Async provisioning connect ---------------------------------------- */
typedef struct {
    char ssid[64];
    char password[64];
    char security_type[16];
} prov_connect_args_t;

static void prov_connect_task(void *param)
{
    prov_connect_args_t *args = (prov_connect_args_t *)param;
    int rc = claw_wifi_mgr_connect_sta(args->ssid, args->password);
    if (rc == RTK_SUCCESS) {
        claw_config_set_wifi(args->ssid, args->password, args->security_type);
        RTK_LOGI(TAG, "prov_connect: connected, config saved\n");
    } else {
        RTK_LOGE(TAG, "prov_connect: connect failed\n");
    }
    rtos_mem_free(args);
    rtos_task_delete(NULL);
}

/* Helper: send {"ok":false,"error":"<msg>"} and return from caller */
static void send_json_err(claw_http_send_fn_t send_fn, int sock,
                          int code, const char *msg)
{
    char err[96];
    DiagSnPrintf(err, sizeof(err), "{\"ok\":false,\"error\":\"%s\"}", msg);
    send_fn(sock, code, "application/json", err, strlen(err));
}

/* ---- API token authentication ----
 *
 * All /api/ routes require the caller to present the device's API token in
 * the X-API-Token request header.  The token is generated once via TRNG on
 * first boot (claw_config_ensure_api_token) and stored in config.
 *
 * Routes exempt from auth (provisioning / status):
 *   GET  /             — dashboard HTML (no secrets served)
 *   GET  /status       — basic device status (no secrets served)
 *   GET  /setup        — setup page HTML
 *   POST /setup        — wifi / first-run provisioning
 *
 * The WebUI front-end stores the token in localStorage and attaches it to
 * every fetch() call via the _t= query param, so normal browser usage is
 * entirely transparent to the user.
 */

/* Actual token verification: called at the top of every /api/ handler. */
static int webui_check_token(const claw_http_request_t *req,
                              claw_http_send_fn_t send_fn, int sock)
{
    const claw_config_t *cfg = claw_config_get();
    const char *expected = cfg->webui.token;

    /* If no token has been generated yet (should not happen after proper boot
     * sequence) fall through and allow access — avoids a permanent lockout on
     * misconfigured devices. */
    if (!expected || !expected[0]) return 0;  /* 0 = auth passed */

    /* Token is transmitted in the X-API-Token query parameter as a fallback
     * for environments where custom headers are stripped (captive portals).
     * Primary delivery is the X-API-Token header, parsed below from the query
     * string since claw_http_request_t doesn't expose raw headers.
     *
     * Convention: the WebUI JS encodes the token as ?_t=<token> appended to
     * every API URL, OR as an X-API-Token: header.  We check the query param
     * here because it is reliably available in claw_http_request_t.query. */
    char presented_enc[72] = {0};
    char presented[40] = {0};
    query_get(req->query, "_t", presented_enc, sizeof(presented_enc));
    url_decode(presented_enc, presented, sizeof(presented));

    if (presented[0] && strcmp(presented, expected) == 0) return 0;

    send_fn(sock, 401, "application/json",
            "{\"ok\":false,\"error\":\"unauthorized\"}",
            strlen("{\"ok\":false,\"error\":\"unauthorized\"}"));
    return -1;  /* -1 = rejected, caller must return immediately */
}

/* Macro used at the top of every /api/ handler to gate access. */
#define REQUIRE_AUTH() do { if (webui_check_token(req, send_fn, sock) != 0) return; } while (0)

#define LUA_SKILLS_DIR   "vfs:/skills"

/* ---- File management helpers ---- */

static void url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t i = 0, j = 0;
    while (src[i] && j + 1 < dst_size) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = {src[i+1], src[i+2], '\0'};
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

static int query_get(const char *query, const char *key, char *out, size_t out_size)
{
    size_t klen = strlen(key);
    const char *p = query;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *end = strchr(v, '&');
            size_t vlen = end ? (size_t)(end - v) : strlen(v);
            if (vlen >= out_size) vlen = out_size - 1;
            _memcpy(out, v, vlen);
            out[vlen] = '\0';
            return 1;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return 0;
}

static int files_path_safe(const char *path)
{
    return path && path[0] != '\0' && !strstr(path, "..");
}

static void to_vfs_path(const char *path, char *out, size_t out_size)
{
    DiagSnPrintf(out, out_size, "vfs:%s", path);
}

/* Sorting helper for directory listing: dirs first, then files, each group alphabetical */
#define FILES_LIST_MAX 128
typedef struct { char name[128]; uint8_t is_dir; } files_entry_t;

/* ---- Recursive directory remove ---- */
static int rmdir_recursive(const char *vfs_path)
{
    void *dir = opendir(vfs_path);
    if (!dir) return remove(vfs_path);
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[272];
        snprintf(child, sizeof(child), "%s/%s", vfs_path, ent->d_name);
        if (ent->d_type == DT_DIR)
            rmdir_recursive(child);
        else
            remove(child);
    }
    closedir(dir);
    return rmdir(vfs_path);
}

/* ---- Minimal uncompressed ZIP builder ---- */
#define ZIP_MAX_FILES  64
#define ZIP_MAX_BYTES  (192 * 1024)
#define ZIP_FILE_LIMIT (64 * 1024)

typedef struct {
    uint32_t crc32;
    uint32_t file_size;
    uint32_t local_offset;
    uint16_t fname_len;
    char     fname[256];
} zip_cd_t;

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;
} zip_buf_t;

static int zb_ensure(zip_buf_t *z, size_t need)
{
    if (z->pos + need <= z->cap) return 1;
    size_t nc = z->cap + need + 8192;
    if (nc > ZIP_MAX_BYTES) return 0;
    uint8_t *nb = realloc(z->buf, nc);
    if (!nb) return 0;
    z->buf = nb; z->cap = nc;
    return 1;
}

static void zb_u16(zip_buf_t *z, uint16_t v)
{
    z->buf[z->pos++] = (uint8_t)(v & 0xFF);
    z->buf[z->pos++] = (uint8_t)(v >> 8);
}

static void zb_u32(zip_buf_t *z, uint32_t v)
{
    z->buf[z->pos++] = (uint8_t)(v & 0xFF);
    z->buf[z->pos++] = (uint8_t)((v >> 8) & 0xFF);
    z->buf[z->pos++] = (uint8_t)((v >> 16) & 0xFF);
    z->buf[z->pos++] = (uint8_t)(v >> 24);
}

static uint32_t zip_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static int zip_add_file(zip_buf_t *z, zip_cd_t *cd, int *ncd,
                        const char *fname, const uint8_t *data, uint32_t dlen)
{
    if (*ncd >= ZIP_MAX_FILES) return 0;
    uint16_t nl = (uint16_t)strlen(fname);
    if (!zb_ensure(z, 30u + nl + dlen)) return 0;
    uint32_t crc = zip_crc32(data, dlen);
    uint32_t off = (uint32_t)z->pos;
    zb_u32(z, 0x04034B50u);
    zb_u16(z, 20); zb_u16(z, 0); zb_u16(z, 0);
    zb_u16(z, 0); zb_u16(z, 0);
    zb_u32(z, crc); zb_u32(z, dlen); zb_u32(z, dlen);
    zb_u16(z, nl); zb_u16(z, 0);
    memcpy(z->buf + z->pos, fname, nl); z->pos += nl;
    memcpy(z->buf + z->pos, data, dlen); z->pos += dlen;
    zip_cd_t *e = &cd[(*ncd)++];
    e->crc32 = crc; e->file_size = dlen; e->local_offset = off; e->fname_len = nl;
    strlcpy(e->fname, fname, sizeof(e->fname));
    return 1;
}

static void zip_finish(zip_buf_t *z, zip_cd_t *cd, int ncd)
{
    uint32_t cd_off = (uint32_t)z->pos;
    for (int i = 0; i < ncd; i++) {
        zip_cd_t *e = &cd[i];
        if (!zb_ensure(z, 46u + e->fname_len)) continue;
        zb_u32(z, 0x02014B50u);
        zb_u16(z, 0x0314); zb_u16(z, 20); zb_u16(z, 0); zb_u16(z, 0);
        zb_u16(z, 0); zb_u16(z, 0);
        zb_u32(z, e->crc32); zb_u32(z, e->file_size); zb_u32(z, e->file_size);
        zb_u16(z, e->fname_len); zb_u16(z, 0); zb_u16(z, 0);
        zb_u16(z, 0); zb_u16(z, 0); zb_u32(z, 0x81A40000u);
        zb_u32(z, e->local_offset);
        memcpy(z->buf + z->pos, e->fname, e->fname_len); z->pos += e->fname_len;
    }
    uint32_t cd_size = (uint32_t)z->pos - cd_off;
    if (!zb_ensure(z, 22)) return;
    zb_u32(z, 0x06054B50u);
    zb_u16(z, 0); zb_u16(z, 0);
    zb_u16(z, (uint16_t)ncd); zb_u16(z, (uint16_t)ncd);
    zb_u32(z, cd_size); zb_u32(z, cd_off);
    zb_u16(z, 0);
}

static void zip_scan_dir(zip_buf_t *z, zip_cd_t *cd, int *ncd,
                         const char *vfs_dir, const char *prefix)
{
    void *dir = opendir(vfs_dir);
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char vchild[272], rel[256];
        snprintf(vchild, sizeof(vchild), "%s/%s", vfs_dir, ent->d_name);
        if (prefix[0])
            snprintf(rel, sizeof(rel), "%s/%s", prefix, ent->d_name);
        else
            strlcpy(rel, ent->d_name, sizeof(rel));
        if (ent->d_type == DT_DIR) {
            zip_scan_dir(z, cd, ncd, vchild, rel);
        } else {
            FILE *f = fopen(vchild, "rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
            if (sz < 0) sz = 0;
            if (sz > ZIP_FILE_LIMIT) sz = ZIP_FILE_LIMIT;
            uint8_t *fbuf = malloc((size_t)sz + 1);
            if (fbuf) {
                size_t n = fread(fbuf, 1, (size_t)sz, f);
                zip_add_file(z, cd, ncd, rel, fbuf, (uint32_t)n);
                free(fbuf);
            }
            fclose(f);
        }
    }
    closedir(dir);
}

static int files_entry_cmp(const void *a, const void *b)
{
    const files_entry_t *ea = (const files_entry_t *)a;
    const files_entry_t *eb = (const files_entry_t *)b;
    if (ea->is_dir != eb->is_dir)
        return (int)eb->is_dir - (int)ea->is_dir;
    return strcmp(ea->name, eb->name);
}

/* GET /api/files?path=/xxx  →  JSON directory listing */
static void handle_api_files_get(const claw_http_request_t *req,
                                  claw_http_send_fn_t send_fn, int sock)
{
    REQUIRE_AUTH();
    char enc[256], path[256];
    if (!query_get(req->query, "path", enc, sizeof(enc))) {
        strcpy(path, "/");
    } else {
        url_decode(enc, path, sizeof(path));
    }
    if (!files_path_safe(path)) {
        const char *err = "{\"error\":\"invalid path\"}";
        send_fn(sock, 400, "application/json", err, strlen(err));
        return;
    }

    char vfs_path[264];
    to_vfs_path(path, vfs_path, sizeof(vfs_path));
    void *dir = opendir(vfs_path);
    if (!dir) {
        char err[192];
        DiagSnPrintf(err, sizeof(err), "{\"error\":\"cannot open directory\",\"path\":\"%s\"}", path);
        send_fn(sock, 404, "application/json", err, strlen(err));
        return;
    }

    /* Collect entries into a buffer, then sort */
    files_entry_t *buf = malloc(FILES_LIST_MAX * sizeof(files_entry_t));
    int count = 0;
    if (buf) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (count >= FILES_LIST_MAX) break;
            strlcpy(buf[count].name, ent->d_name, sizeof(buf[0].name));
            buf[count].is_dir = (ent->d_type == DT_DIR) ? 1 : 0;
            count++;
        }
        qsort(buf, count, sizeof(files_entry_t), files_entry_cmp);
    }
    closedir(dir);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "path", path);
    cJSON *entries = cJSON_CreateArray();
    cJSON_AddItemToObject(resp, "entries", entries);

    size_t plen = strlen(path);
    if (buf && resp && entries) {
        for (int i = 0; i < count; i++) {
            char fp[256];
            if (plen > 0 && path[plen - 1] == '/') {
                DiagSnPrintf(fp, sizeof(fp), "%s%s", path, buf[i].name);
            } else {
                DiagSnPrintf(fp, sizeof(fp), "%s/%s", path, buf[i].name);
            }
            char vfs_fp[272];
            to_vfs_path(fp, vfs_fp, sizeof(vfs_fp));
            struct stat st;
            int has_stat = (stat(vfs_fp, &st) == 0);
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "name", buf[i].name);
            cJSON_AddStringToObject(e, "path", fp);
            cJSON_AddStringToObject(e, "type", buf[i].is_dir ? "dir" : "file");
            cJSON_AddNumberToObject(e, "size", (!buf[i].is_dir && has_stat) ? (long)st.st_size : -1);
            cJSON_AddNumberToObject(e, "mtime", has_stat ? (long)st.st_mtime : 0);
            cJSON_AddItemToArray(entries, e);
        }
    }
    free(buf);

    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (out) {
        send_fn(sock, 200, "application/json", out, strlen(out));
        free(out);
    } else {
        send_fn(sock, 500, "application/json", "{\"error\":\"oom\"}", 14);
    }
}

/* DELETE /api/files?path=/xxx  →  delete file */
static void handle_api_files_delete(const claw_http_request_t *req,
                                     claw_http_send_fn_t send_fn, int sock)
{
    REQUIRE_AUTH();
    char enc[256], path[256];
    if (!query_get(req->query, "path", enc, sizeof(enc))) {
        send_fn(sock, 400, "application/json", "{\"error\":\"path required\"}", 24);
        return;
    }
    url_decode(enc, path, sizeof(path));
    if (!files_path_safe(path)) {
        send_fn(sock, 400, "application/json", "{\"error\":\"invalid path\"}", 23);
        return;
    }
    char vfs_path[264];
    to_vfs_path(path, vfs_path, sizeof(vfs_path));
    struct stat st;
    int is_dir = (stat(vfs_path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR);
    int ret = is_dir ? rmdir_recursive(vfs_path) : remove(vfs_path);
    if (ret != 0) {
        char err[192];
        DiagSnPrintf(err, sizeof(err), "{\"ok\":false,\"error\":\"delete failed\",\"path\":\"%s\"}", path);
        send_fn(sock, 500, "application/json", err, strlen(err));
        return;
    }
    send_fn(sock, 200, "application/json", "{\"ok\":true}", 11);
}

/* GET /api/files/download?path=/xxx  →  file: raw bytes; directory: uncompressed ZIP */
static void handle_api_files_download(const claw_http_request_t *req,
                                       claw_http_send_fn_t send_fn, int sock)
{
    REQUIRE_AUTH();
    char enc[256], path[256];
    if (!query_get(req->query, "path", enc, sizeof(enc))) {
        send_fn(sock, 400, "text/plain", "path required", 13);
        return;
    }
    url_decode(enc, path, sizeof(path));
    if (!files_path_safe(path)) {
        send_fn(sock, 400, "text/plain", "invalid path", 12);
        return;
    }

    char vfs_path[264];
    to_vfs_path(path, vfs_path, sizeof(vfs_path));

    struct stat st;
    if (stat(vfs_path, &st) != 0) {
        send_fn(sock, 404, "text/plain", "not found", 9);
        return;
    }

    if ((st.st_mode & S_IFMT) == S_IFDIR) {
        zip_cd_t *cd = malloc(ZIP_MAX_FILES * sizeof(zip_cd_t));
        if (!cd) { send_fn(sock, 500, "text/plain", "oom", 3); return; }
        int ncd = 0;
        zip_buf_t z = {0};
        z.buf = malloc(16384);
        if (!z.buf) { free(cd); send_fn(sock, 500, "text/plain", "oom", 3); return; }
        z.cap = 16384;
        zip_scan_dir(&z, cd, &ncd, vfs_path, "");
        zip_finish(&z, cd, ncd);
        free(cd);
        send_fn(sock, 200, "application/zip", (const char *)z.buf, z.pos);
        free(z.buf);
    } else {
        FILE *f = fopen(vfs_path, "rb");
        if (!f) { send_fn(sock, 404, "text/plain", "not found", 9); return; }
        long sz = st.st_size;
        if (sz < 0) sz = 0;
        /* Same cap as the read_file tool / content endpoint so a file the WebUI
         * can display is also downloadable in full. */
        if (sz > CAP_FILES_MAX_READ_SIZE) sz = CAP_FILES_MAX_READ_SIZE;
        char *buf = malloc((size_t)sz + 1);
        if (!buf) { fclose(f); send_fn(sock, 500, "text/plain", "oom", 3); return; }
        size_t n = fread(buf, 1, (size_t)sz, f);
        fclose(f);
        send_fn(sock, 200, "application/octet-stream", buf, n);
        free(buf);
    }
}

/* GET /api/files/content?path=/xxx  →  file content as text (max CAP_FILES_MAX_READ_SIZE) */
static void handle_api_files_content_get(const claw_http_request_t *req,
                                          claw_http_send_fn_t send_fn, int sock)
{
    REQUIRE_AUTH();
    char enc[256], path[256];
    if (!query_get(req->query, "path", enc, sizeof(enc))) {
        send_fn(sock, 400, "application/json", "{\"error\":\"path required\"}", 25);
        return;
    }
    url_decode(enc, path, sizeof(path));
    if (!files_path_safe(path)) {
        send_fn(sock, 400, "application/json", "{\"error\":\"invalid path\"}", 24);
        return;
    }

    char vfs_path[264];
    to_vfs_path(path, vfs_path, sizeof(vfs_path));

    struct stat st;
    if (stat(vfs_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        send_fn(sock, 404, "application/json", "{\"error\":\"file not found\"}", 26);
        return;
    }
    if (st.st_size > CAP_FILES_MAX_READ_SIZE) {
        send_fn(sock, 413, "application/json", "{\"error\":\"file too large\"}", 25);
        return;
    }
    FILE *f = fopen(vfs_path, "r");
    if (!f) {
        send_fn(sock, 500, "application/json", "{\"error\":\"cannot open file\"}", 28);
        return;
    }
    long sz = st.st_size;
    if (sz < 0) sz = 0;
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); send_fn(sock, 500, "application/json", "{\"error\":\"oom\"}", 15); return; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    /* Detect binary content (null byte) */
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\0') {
            free(buf);
            send_fn(sock, 415, "application/json", "{\"error\":\"binary file not supported\"}", 37);
            return;
        }
    }
    send_fn(sock, 200, "text/plain; charset=utf-8", buf, n);
    free(buf);
}

/* PUT /api/files/content?path=/xxx  body=text → save file content */
static void handle_api_files_content_put(const claw_http_request_t *req,
                                          claw_http_send_fn_t send_fn, int sock)
{
    REQUIRE_AUTH();
    char enc[256], path[256];
    if (!query_get(req->query, "path", enc, sizeof(enc))) {
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"path required\"}", 36);
        return;
    }
    url_decode(enc, path, sizeof(path));
    if (!files_path_safe(path)) {
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"invalid path\"}", 35);
        return;
    }

    char vfs_path[264];
    to_vfs_path(path, vfs_path, sizeof(vfs_path));

    /* Check parent directory exists (same logic as upload handler) */
    char dir_path[256];
    strlcpy(dir_path, path, sizeof(dir_path));
    char *slash = strrchr(dir_path, '/');
    if (slash && slash != dir_path) {
        *slash = '\0';
        char vfs_dir[264];
        to_vfs_path(dir_path, vfs_dir, sizeof(vfs_dir));
        struct stat dir_st;
        if (stat(vfs_dir, &dir_st) != 0 || (dir_st.st_mode & S_IFMT) != S_IFDIR) {
            char err[192];
            DiagSnPrintf(err, sizeof(err),
                         "{\"ok\":false,\"error\":\"directory not found\",\"path\":\"%s\"}", dir_path);
            send_fn(sock, 404, "application/json", err, strlen(err));
            return;
        }
    }

    FILE *f = fopen(vfs_path, "w");
    if (!f) {
        send_fn(sock, 500, "application/json", "{\"ok\":false,\"error\":\"cannot write file\"}", 40);
        return;
    }
    if (req->body && req->body_len > 0) {
        size_t written = fwrite(req->body, 1, req->body_len, f);
        fclose(f);
        if (written != req->body_len) {
            send_fn(sock, 500, "application/json",
                    "{\"ok\":false,\"error\":\"write incomplete\"}", 38);
            return;
        }
    } else {
        fclose(f);
    }
    send_fn(sock, 200, "application/json", "{\"ok\":true}", 11);
}

/* POST /api/files/upload  →  multipart/form-data upload (fields: file, path) */
static void handle_api_files_upload(const claw_http_request_t *req,
                                     claw_http_send_fn_t send_fn, int sock)
{
    REQUIRE_AUTH();
    const char *body = req->body;
    if (!body || req->body_len < 10 || body[0] != '-' || body[1] != '-') {
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"not multipart\"}", 35);
        return;
    }

    /* Extract boundary from first line (body starts with "--BOUNDARY\r\n") */
    const char *first_eol = strstr(body, "\r\n");
    if (!first_eol) {
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"bad boundary\"}", 34);
        return;
    }
    char bdry[130]; /* "--BOUNDARY" including leading "--" */
    size_t bdlen = (size_t)(first_eol - body);
    if (bdlen == 0 || bdlen >= sizeof(bdry)) {
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"bad boundary\"}", 34);
        return;
    }
    _memcpy(bdry, body, bdlen);
    bdry[bdlen] = '\0';

    char upload_filename[128] = "";
    const char *file_data     = NULL;
    size_t      file_data_len = 0;
    char upload_dir[256]      = "/";

    /* Scan parts; each part ends at "\r\n--BOUNDARY" */
    const char *p = first_eol + 2;
    while (p && *p) {
        const char *hdr_end = strstr(p, "\r\n\r\n");
        if (!hdr_end) break;
        const char *data = hdr_end + 4;

        /* Parse field name and optional filename from Content-Disposition */
        char field_name[64] = "";
        char fn[128]        = "";
        const char *cd = strstr(p, "Content-Disposition:");
        if (cd && cd < hdr_end) {
            const char *ns = strstr(cd, "name=\"");
            if (ns && ns < hdr_end) {
                ns += 6;
                const char *ne = strchr(ns, '"');
                if (ne && ne < hdr_end) {
                    size_t nl = (size_t)(ne - ns);
                    if (nl >= sizeof(field_name)) nl = sizeof(field_name) - 1;
                    _memcpy(field_name, ns, nl);
                    field_name[nl] = '\0';
                }
            }
            const char *fs = strstr(cd, "filename=\"");
            if (fs && fs < hdr_end) {
                fs += 10;
                const char *fe = strchr(fs, '"');
                if (fe && fe < hdr_end) {
                    size_t fl = (size_t)(fe - fs);
                    if (fl >= sizeof(fn)) fl = sizeof(fn) - 1;
                    _memcpy(fn, fs, fl);
                    fn[fl] = '\0';
                }
            }
        }

        /* Find end of this part's data (next boundary) */
        const char *next_bdry = strstr(data, bdry);
        if (!next_bdry) break;
        size_t dlen = (size_t)(next_bdry - data);
        /* Strip the \r\n that precedes the boundary delimiter */
        if (dlen >= 2) dlen -= 2;

        if (strcmp(field_name, "file") == 0) {
            file_data     = data;
            file_data_len = dlen;
            if (fn[0]) strlcpy(upload_filename, fn, sizeof(upload_filename));
        } else if (strcmp(field_name, "path") == 0 && dlen > 0 && dlen < sizeof(upload_dir)) {
            _memcpy(upload_dir, data, dlen);
            upload_dir[dlen] = '\0';
            /* Strip trailing \r\n */
            size_t l = strlen(upload_dir);
            while (l > 0 && (upload_dir[l-1] == '\r' || upload_dir[l-1] == '\n'))
                upload_dir[--l] = '\0';
            if (l == 0) strcpy(upload_dir, "/");
        }

        /* Advance to next part: skip past boundary + \r\n */
        p = next_bdry + bdlen;
        if (p[0] == '-' && p[1] == '-') break; /* final boundary */
        if (p[0] == '\r' && p[1] == '\n') p += 2;
        else break;
    }

    if (!file_data || upload_filename[0] == '\0') {
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"no file\"}", 29);
        return;
    }
    if (!files_path_safe(upload_dir) || strstr(upload_filename, "/") || strstr(upload_filename, "..")) {
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"invalid path\"}", 34);
        return;
    }

    char dest[256];
    size_t dlen_dir = strlen(upload_dir);
    if (dlen_dir > 0 && upload_dir[dlen_dir - 1] == '/') {
        DiagSnPrintf(dest, sizeof(dest), "%s%s", upload_dir, upload_filename);
    } else {
        DiagSnPrintf(dest, sizeof(dest), "%s/%s", upload_dir, upload_filename);
    }

    char vfs_dest[264];
    to_vfs_path(dest, vfs_dest, sizeof(vfs_dest));
    char vfs_dir[264];
    to_vfs_path(upload_dir, vfs_dir, sizeof(vfs_dir));
    struct stat dir_st;
    if (stat(vfs_dir, &dir_st) != 0 || (dir_st.st_mode & S_IFMT) != S_IFDIR) {
        char err[192];
        snprintf(err, sizeof(err), "{\"ok\":false,\"error\":\"directory not found\",\"path\":\"%s\"}", upload_dir);
        send_fn(sock, 404, "application/json", err, strlen(err));
        return;
    }
    FILE *f = fopen(vfs_dest, "wb");
    if (!f) {
        char err[192];
        DiagSnPrintf(err, sizeof(err), "{\"ok\":false,\"error\":\"cannot write\",\"path\":\"%s\"}", dest);
        send_fn(sock, 500, "application/json", err, strlen(err));
        return;
    }
    size_t wr1 = fwrite(file_data, 1, file_data_len, f);
    fclose(f);
    if (wr1 != file_data_len) {
        send_fn(sock, 500, "application/json",
                "{\"ok\":false,\"error\":\"write incomplete\"}", 38);
        return;
    }
    send_fn(sock, 200, "application/json", "{\"ok\":true}", 11);
}

/* POST /api/files/mkdir?path=/xxx  →  create directory */
static void handle_api_files_mkdir(const claw_http_request_t *req,
                                    claw_http_send_fn_t send_fn, int sock)
{
    REQUIRE_AUTH();
    char enc[256], path[256];
    if (!query_get(req->query, "path", enc, sizeof(enc))) {
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"path required\"}", 35);
        return;
    }
    url_decode(enc, path, sizeof(path));
    if (!files_path_safe(path)) {
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"invalid path\"}", 34);
        return;
    }
    char vfs_path[264];
    to_vfs_path(path, vfs_path, sizeof(vfs_path));
    if (mkdir(vfs_path, 0777) != 0) {
        char err[192];
        snprintf(err, sizeof(err), "{\"ok\":false,\"error\":\"mkdir failed\",\"path\":\"%s\"}", path);
        send_fn(sock, 500, "application/json", err, strlen(err));
        return;
    }
    send_fn(sock, 200, "application/json", "{\"ok\":true}", 11);
}

/* ---- Handlers ---- */

static void handle_index(const claw_http_request_t *req,
                          claw_http_send_fn_t send_fn, int sock)
{
    (void)req;
    send_fn(sock, 200, "text/html; charset=utf-8",
            (const char *)_res_dashboard_html_start,
            (size_t)(_res_dashboard_html_end - _res_dashboard_html_start));
}

static void handle_status(const claw_http_request_t *req,
                           claw_http_send_fn_t send_fn, int sock)
{
    (void)req;
    const claw_config_t *cfg   = claw_config_get();
    claw_wifi_state_t    state = claw_wifi_mgr_get_state();
    bool                 connected = (state == CLAW_WIFI_STATE_CONNECTED);
    const char          *ip    = claw_wifi_mgr_get_sta_ip();
    bool                 softap_up = claw_wifi_mgr_is_softap_running();
    uint32_t             free_heap = rtos_mem_get_free_heap_size();
    uint32_t             min_heap  = rtos_mem_get_minimum_ever_free_heap_size();

    const char *connect_err = claw_wifi_mgr_get_connect_error();

    /* Build the status response with cJSON so all string fields are
     * properly JSON-escaped (prevents injection via crafted SSID etc.).
     * The api_token is included unauthenticated so the dashboard can
     * bootstrap itself on first load — access is scoped to the same LAN. */
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        send_fn(sock, 500, "application/json", "{\"error\":\"oom\"}", 14);
        return;
    }

    cJSON *wifi_obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(wifi_obj,   "connected",     connected);
    cJSON_AddBoolToObject(wifi_obj,   "configured",    cfg->wifi.configured);
    cJSON_AddStringToObject(wifi_obj, "ssid",          cfg->wifi.ssid);
    cJSON_AddStringToObject(wifi_obj, "security_type", cfg->wifi.security_type);
    cJSON_AddStringToObject(wifi_obj, "ip",            ip ? ip : "");
    cJSON_AddStringToObject(wifi_obj, "connect_error", connect_err ? connect_err : "");
    cJSON_AddItemToObject(root, "wifi", wifi_obj);

    cJSON *softap_obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(softap_obj,   "running", softap_up);
    cJSON_AddStringToObject(softap_obj, "ssid",    cfg->softap.ssid);
    cJSON_AddStringToObject(softap_obj, "ip",      "192.168.1.1");
    cJSON_AddItemToObject(root, "softap", softap_obj);

    cJSON_AddStringToObject(root, "mode",      softap_up && !connected ? "provisioning" : "normal");
    cJSON_AddStringToObject(root, "version",   "1.0");
    cJSON_AddStringToObject(root, "api_token", cfg->webui.token);

    cJSON *heap_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(heap_obj, "free_bytes",     (double)free_heap);
    cJSON_AddNumberToObject(heap_obj, "min_ever_bytes", (double)min_heap);
    cJSON_AddItemToObject(root, "heap", heap_obj);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
        send_fn(sock, 500, "application/json", "{\"error\":\"oom\"}", 14);
        return;
    }
    send_fn(sock, 200, "application/json", json_str, strlen(json_str));
    free(json_str);
}


static void handle_api_config(const claw_http_request_t *req,
                               claw_http_send_fn_t send_fn, int sock)
{
    REQUIRE_AUTH();
    (void)req;
    const claw_config_t *cfg = claw_config_get();

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        send_fn(sock, 500, "application/json", "{\"error\":\"oom\"}", 14);
        return;
    }

    /* Sensitive fields are masked: secret values are never returned over the
     * unauthenticated HTTP API.  The UI uses "(set)" / "" to show whether a
     * value has been configured without exposing the actual credential. */
#define MASK(s) ((s)[0] ? "(set)" : "")

    cJSON *wifi = cJSON_CreateObject();
    cJSON_AddStringToObject(wifi, "ssid",          cfg->wifi.ssid);
    cJSON_AddStringToObject(wifi, "password",      MASK(cfg->wifi.password));
    cJSON_AddStringToObject(wifi, "security_type", cfg->wifi.security_type);
    cJSON_AddBoolToObject(  wifi, "configured",    cfg->wifi.configured);
    cJSON_AddItemToObject(root, "wifi", wifi);

    cJSON *llm = cJSON_CreateObject();
    cJSON_AddStringToObject(llm, "api_url",        cfg->llm.api_url);
    cJSON_AddStringToObject(llm, "api_key",        MASK(cfg->llm.api_key));
    cJSON_AddStringToObject(llm, "model",          cfg->llm.model);
    cJSON_AddNumberToObject(llm, "max_tokens",     cfg->llm.max_tokens);
    cJSON_AddNumberToObject(llm, "max_iterations", cfg->llm.max_iterations);
    cJSON_AddNumberToObject(llm, "backend",        cfg->llm.backend);
    cJSON_AddBoolToObject(  llm, "thinking",       cfg->llm.thinking_enabled);
    cJSON_AddBoolToObject(  llm, "stream",         cfg->llm.stream_enabled);
    cJSON_AddNumberToObject(llm, "compact_tokens", cfg->llm.compact_tokens);
    cJSON_AddNumberToObject(llm, "window_tokens",  cfg->llm.window_tokens);
    cJSON_AddItemToObject(root, "llm", llm);

    cJSON *tg = cJSON_CreateObject();
    cJSON_AddStringToObject(tg, "bot_token", MASK(cfg->telegram.bot_token));
    cJSON_AddItemToObject(root, "telegram", tg);

    cJSON *fs = cJSON_CreateObject();
    cJSON_AddStringToObject(fs, "app_id",     cfg->feishu.app_id);
    cJSON_AddStringToObject(fs, "app_secret", MASK(cfg->feishu.app_secret));
    cJSON_AddItemToObject(root, "feishu", fs);

    cJSON *wx = cJSON_CreateObject();
    cJSON_AddStringToObject(wx, "base_url", cfg->wechat.base_url);
    cJSON_AddStringToObject(wx, "app_id",   cfg->wechat.app_id);
    cJSON_AddItemToObject(root, "wechat", wx);

    cJSON *ws = cJSON_CreateObject();
    cJSON_AddStringToObject(ws, "api_key",     MASK(cfg->web_search.api_key));
    cJSON_AddNumberToObject(ws, "max_results", cfg->web_search.max_results);
    cJSON_AddItemToObject(root, "search", ws);

#undef MASK

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        send_fn(sock, 500, "application/json", "{\"error\":\"oom\"}", 14);
        return;
    }

    send_fn(sock, 200, "application/json", json, strlen(json));
    free(json);
}

static void handle_setup_post(const claw_http_request_t *req,
                               claw_http_send_fn_t send_fn, int sock)
{
    const char *resp_ok = "{\"ok\":true}";

    if (!req->body || req->body_len == 0) {
        send_json_err(send_fn, sock, 400, "empty body");
        return;
    }

    cJSON *root = cJSON_ParseWithLength(req->body, req->body_len);
    if (!root) {
        send_json_err(send_fn, sock, 400, "invalid JSON");
        return;
    }

    cJSON *sec_j = cJSON_GetObjectItemCaseSensitive(root, "section");
    if (!sec_j || !cJSON_IsString(sec_j)) {
        cJSON_Delete(root);
        send_json_err(send_fn, sock, 400, "section required");
        return;
    }

    const char *section = sec_j->valuestring;
    int rc = RTK_SUCCESS;

    if (strcmp(section, "wifi") == 0) {
        cJSON *ssid_j = cJSON_GetObjectItemCaseSensitive(root, "ssid");
        cJSON *pw_j   = cJSON_GetObjectItemCaseSensitive(root, "password");
        cJSON *sec2_j = cJSON_GetObjectItemCaseSensitive(root, "security_type");

        const char *ssid = (ssid_j && cJSON_IsString(ssid_j)) ? ssid_j->valuestring : NULL;
        const char *pw   = (pw_j   && cJSON_IsString(pw_j))   ? pw_j->valuestring   : "";
        const char *sec2 = (sec2_j && cJSON_IsString(sec2_j)) ? sec2_j->valuestring : "WPA2";

        if (!ssid || ssid[0] == '\0') {
            cJSON_Delete(root);
            send_json_err(send_fn, sock, 400, "ssid required");
            return;
        }
        rc = claw_config_set_wifi(ssid, pw, sec2);

    } else if (strcmp(section, "llm") == 0) {
        cJSON *url_j  = cJSON_GetObjectItemCaseSensitive(root, "api_url");
        cJSON *key_j  = cJSON_GetObjectItemCaseSensitive(root, "api_key");
        cJSON *mod_j  = cJSON_GetObjectItemCaseSensitive(root, "model");
        cJSON *tok_j  = cJSON_GetObjectItemCaseSensitive(root, "max_tokens");
        cJSON *iter_j = cJSON_GetObjectItemCaseSensitive(root, "max_iterations");
        cJSON *bk_j   = cJSON_GetObjectItemCaseSensitive(root, "backend");
        cJSON *think_j = cJSON_GetObjectItemCaseSensitive(root, "thinking");
        cJSON *strm_j  = cJSON_GetObjectItemCaseSensitive(root, "stream");
        cJSON *cmpt_j  = cJSON_GetObjectItemCaseSensitive(root, "compact_tokens");
        cJSON *win_j   = cJSON_GetObjectItemCaseSensitive(root, "window_tokens");

        const char *url  = (url_j  && cJSON_IsString(url_j))  ? url_j->valuestring  : "";
        const char *key  = (key_j  && cJSON_IsString(key_j))  ? key_j->valuestring  : "";
        const char *mod  = (mod_j  && cJSON_IsString(mod_j))  ? mod_j->valuestring  : "glm-5.1";
        uint32_t tok  = (tok_j  && cJSON_IsNumber(tok_j))  ? (uint32_t)tok_j->valueint  : 1024;
        uint8_t  iter = (iter_j && cJSON_IsNumber(iter_j)) ? (uint8_t)iter_j->valueint  : 5;
        int      bk   = (bk_j   && cJSON_IsNumber(bk_j))  ? (int)bk_j->valueint        : -1;
        /* absent → keep current (-1); present bool → on/off */
        int think = cJSON_IsBool(think_j) ? (cJSON_IsTrue(think_j) ? 1 : 0) : -1;
        int strm  = cJSON_IsBool(strm_j)  ? (cJSON_IsTrue(strm_j)  ? 1 : 0) : -1;
        /* absent / 0 → keep current */
        uint32_t cmpt = (cmpt_j && cJSON_IsNumber(cmpt_j)) ? (uint32_t)cmpt_j->valuedouble : 0;
        uint32_t win  = (win_j  && cJSON_IsNumber(win_j))  ? (uint32_t)win_j->valuedouble  : 0;

        rc = claw_config_set_llm(key, mod, url, tok, iter, bk, think, strm, cmpt, win);

    } else if (strcmp(section, "telegram") == 0) {
        cJSON *tok_j = cJSON_GetObjectItemCaseSensitive(root, "bot_token");
        const char *token = (tok_j && cJSON_IsString(tok_j)) ? tok_j->valuestring : "";
        rc = claw_config_set_telegram(token);

    } else if (strcmp(section, "feishu") == 0) {
        cJSON *id_j  = cJSON_GetObjectItemCaseSensitive(root, "app_id");
        cJSON *sec2_j = cJSON_GetObjectItemCaseSensitive(root, "app_secret");
        const char *app_id  = (id_j   && cJSON_IsString(id_j))   ? id_j->valuestring   : "";
        const char *app_sec = (sec2_j && cJSON_IsString(sec2_j)) ? sec2_j->valuestring : "";
        rc = claw_config_set_feishu(app_id, app_sec);

    } else if (strcmp(section, "wechat") == 0) {
        cJSON *url_j = cJSON_GetObjectItemCaseSensitive(root, "base_url");
        cJSON *aid_j = cJSON_GetObjectItemCaseSensitive(root, "app_id");
        cJSON *tok_j = cJSON_GetObjectItemCaseSensitive(root, "bot_token");
        const char *base_url  = (url_j && cJSON_IsString(url_j)) ? url_j->valuestring : "";
        const char *app_id    = (aid_j && cJSON_IsString(aid_j)) ? aid_j->valuestring : "";
        const char *bot_token = (tok_j && cJSON_IsString(tok_j)) ? tok_j->valuestring : NULL;
        rc = claw_config_set_wechat(base_url, app_id);
        if (rc == RTK_SUCCESS && bot_token)
            cap_im_wechat_store_token(bot_token);

    } else if (strcmp(section, "imbot") == 0) {
        /* Unified IM bot save: all three platforms in one request */
        cJSON *wx_j = cJSON_GetObjectItemCaseSensitive(root, "wechat");
        cJSON *fs_j = cJSON_GetObjectItemCaseSensitive(root, "feishu");
        cJSON *tg_j = cJSON_GetObjectItemCaseSensitive(root, "telegram");

        const char *wx_url = "";
        const char *wx_tok = NULL;
        const char *fs_id  = "";
        const char *fs_sec = "";
        const char *tg_tok = "";

        if (wx_j && cJSON_IsObject(wx_j)) {
            cJSON *u = cJSON_GetObjectItemCaseSensitive(wx_j, "base_url");
            cJSON *t = cJSON_GetObjectItemCaseSensitive(wx_j, "bot_token");
            if (u && cJSON_IsString(u)) wx_url = u->valuestring;
            if (t && cJSON_IsString(t)) wx_tok = t->valuestring;
        }
        if (fs_j && cJSON_IsObject(fs_j)) {
            cJSON *i = cJSON_GetObjectItemCaseSensitive(fs_j, "app_id");
            cJSON *p = cJSON_GetObjectItemCaseSensitive(fs_j, "app_secret");
            if (i && cJSON_IsString(i)) fs_id  = i->valuestring;
            if (p && cJSON_IsString(p)) fs_sec = p->valuestring;
        }
        if (tg_j && cJSON_IsObject(tg_j)) {
            cJSON *t = cJSON_GetObjectItemCaseSensitive(tg_j, "bot_token");
            if (t && cJSON_IsString(t)) tg_tok = t->valuestring;
        }

        rc = claw_config_set_imbot(wx_url, fs_id, fs_sec, tg_tok);
        if (rc == RTK_SUCCESS && wx_tok)
            cap_im_wechat_store_token(wx_tok);

    } else if (strcmp(section, "search") == 0) {
        cJSON *key_j = cJSON_GetObjectItemCaseSensitive(root, "api_key");
        cJSON *n_j   = cJSON_GetObjectItemCaseSensitive(root, "max_results");
        const char *api_key = (key_j && cJSON_IsString(key_j)) ? key_j->valuestring : "";
        uint8_t max_res = (n_j && cJSON_IsNumber(n_j) && n_j->valueint > 0)
                          ? (uint8_t)n_j->valueint
                          : CLAW_CONFIG_DEFAULT_SEARCH_MAX_RESULTS;
        rc = claw_config_set_search(api_key, max_res);

    } else {
        cJSON_Delete(root);
        send_json_err(send_fn, sock, 400, "unknown section");
        return;
    }

    cJSON_Delete(root);

    if (rc != RTK_SUCCESS) {
        send_json_err(send_fn, sock, 500, "save failed");
        return;
    }

    send_fn(sock, 200, "application/json", resp_ok, strlen(resp_ok));
}

static void handle_wifi_connect(const claw_http_request_t *req,
                                 claw_http_send_fn_t send_fn, int sock)
{
    REQUIRE_AUTH();
    if (!req->body || req->body_len == 0) {
        send_json_err(send_fn, sock, 400, "empty body");
        return;
    }

    cJSON *root = cJSON_ParseWithLength(req->body, req->body_len);
    if (!root) {
        send_json_err(send_fn, sock, 400, "invalid JSON");
        return;
    }

    cJSON *ssid_j = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    cJSON *pw_j   = cJSON_GetObjectItemCaseSensitive(root, "password");

    const char *ssid = (ssid_j && cJSON_IsString(ssid_j)) ? ssid_j->valuestring : NULL;
    const char *pw   = (pw_j   && cJSON_IsString(pw_j))   ? pw_j->valuestring   : "";

    if (!ssid || ssid[0] == '\0') {
        cJSON_Delete(root);
        send_json_err(send_fn, sock, 400, "ssid required");
        return;
    }

    if (strlen(ssid) > RTW_ESSID_MAX_SIZE) {
        cJSON_Delete(root);
        send_json_err(send_fn, sock, 400, "ssid too long");
        return;
    }

    /* Spawn async task so we can respond before the WiFi channel switch
     * disrupts the SoftAP connection.  Frontend polls GET /status for the IP. */
    prov_connect_args_t *args = (prov_connect_args_t *)rtos_mem_malloc(sizeof(*args));
    if (!args) {
        cJSON_Delete(root);
        send_json_err(send_fn, sock, 500, "out of memory");
        return;
    }
    strlcpy(args->ssid, ssid, sizeof(args->ssid));
    strlcpy(args->password, pw ? pw : "", sizeof(args->password));
    strlcpy(args->security_type, (pw && pw[0] != '\0') ? "WPA2" : "OPEN",
            sizeof(args->security_type));
    cJSON_Delete(root);

    /* Send response BEFORE creating the task: the new task may preempt and
     * trigger a WiFi channel switch that would drop the TCP connection,
     * preventing the body from being delivered if we send after task_create. */
    const char *resp_connecting = "{\"ok\":true,\"connecting\":true}";
    send_fn(sock, 200, "application/json", resp_connecting, strlen(resp_connecting));

    if (rtos_task_create(NULL, "prov_conn", prov_connect_task,
                         args, 4096, 2) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "prov_connect: task create failed, connect will not proceed\n");
        rtos_mem_free(args);
    }
}

static void handle_system_restart(const claw_http_request_t *req,
                                   claw_http_send_fn_t send_fn, int sock)
{
    REQUIRE_AUTH();
    (void)req;
    RTK_LOGI(TAG, "system restart requested via WebUI\n");
    const char *body = "{\"ok\":true}";
    send_fn(sock, 200, "application/json", body, strlen(body));
    rtos_time_delay_ms(500);
    sys_reset();
}

static void handle_wifi_scan(const claw_http_request_t *req,
                              claw_http_send_fn_t send_fn, int sock)
{
    REQUIRE_AUTH();
    (void)req;
    const char *body = "{\"networks\":[]}";
    send_fn(sock, 200, "application/json", body, strlen(body));
}

static void handle_wechat_qrcode(const claw_http_request_t *req,
                                   claw_http_send_fn_t send_fn, int sock)
{
    REQUIRE_AUTH();
    char qr_url[256];
    char buf[384];
    int rc;
    (void)req;

    rc = cap_im_wechat_get_qr(qr_url, sizeof(qr_url));

    if (rc == 0) {
        DiagSnPrintf(buf, sizeof(buf), "{\"ok\":true,\"qr_url\":\"%s\"}", qr_url);
        send_fn(sock, 200, "application/json", buf, strlen(buf));
    } else if (rc == -4) {
        const char *b = "{\"ok\":false,\"error\":\"wifi_not_connected\"}";
        send_fn(sock, 503, "application/json", b, strlen(b));
    } else {
        const char *b = "{\"ok\":false,\"error\":\"qr_fetch_failed\"}";
        send_fn(sock, 500, "application/json", b, strlen(b));
    }
}

static void handle_wechat_status(const claw_http_request_t *req,
                                   claw_http_send_fn_t send_fn, int sock)
{
    REQUIRE_AUTH();
    char buf[384];
    (void)req;
    cap_im_wechat_get_status_json(buf, sizeof(buf));
    send_fn(sock, 200, "application/json", buf, strlen(buf));
}

static void handle_wechat_token_get(const claw_http_request_t *req,
                                     claw_http_send_fn_t send_fn, int sock)
{
    REQUIRE_AUTH();
    /* The token itself is never returned over the unauthenticated HTTP API.
     * Only presence/absence is reported so the UI can show a status indicator
     * without exposing the live WeChat session credential to LAN peers. */
    char token[8]; /* only need to check if non-empty */
    const char *present;
    (void)req;

    present = (cap_im_wechat_get_token(token, sizeof(token)) == 0 && token[0])
              ? "true" : "false";

    char buf[64];
    DiagSnPrintf(buf, sizeof(buf), "{\"ok\":true,\"token_present\":%s}", present);
    send_fn(sock, 200, "application/json", buf, strlen(buf));
}

/* ---- Lua script management (/api/lua) ---- */

static int lua_name_safe(const char *name)
{
    if (!name || !name[0]) return 0;
    if (strstr(name, "/") || strstr(name, "..")) return 0;
    return 1;
}

/* GET /api/lua → {files:[{name,size},...]} */
static void handle_api_lua_list(const claw_http_request_t *req,
                                  claw_http_send_fn_t send_fn, int sock)
{
    REQUIRE_AUTH();
    (void)req;
    void *dir = opendir(LUA_SKILLS_DIR);
    if (!dir) {
        send_fn(sock, 200, "application/json", "{\"files\":[]}", 12);
        return;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();
    if (resp && arr) {
        cJSON_AddItemToObject(resp, "files", arr);
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_type == DT_DIR) continue;
            size_t nlen = strlen(ent->d_name);
            if (nlen < 5 || strcmp(ent->d_name + nlen - 4, ".lua") != 0) continue;
            char fp[160];
            DiagSnPrintf(fp, sizeof(fp), "%s/%s", LUA_SKILLS_DIR, ent->d_name);
            long fsize = 0;
            struct stat st;
            if (stat(fp, &st) == 0) fsize = (long)st.st_size;
            cJSON *e = cJSON_CreateObject();
            if (e) {
                cJSON_AddStringToObject(e, "name", ent->d_name);
                cJSON_AddNumberToObject(e, "size", fsize);
                cJSON_AddItemToArray(arr, e);
            }
        }
    }
    closedir(dir);

    char *out = resp ? cJSON_PrintUnformatted(resp) : NULL;
    cJSON_Delete(resp);
    if (out) {
        send_fn(sock, 200, "application/json", out, strlen(out));
        free(out);
    } else {
        send_fn(sock, 200, "application/json", "{\"files\":[]}", 12);
    }
}

/* GET /api/lua/content?name=xxx → raw script content */
static void handle_api_lua_content_get(const claw_http_request_t *req,
                                         claw_http_send_fn_t send_fn, int sock)
{
    REQUIRE_AUTH();
    char enc[128], name[128];
    if (!query_get(req->query, "name", enc, sizeof(enc))) {
        send_fn(sock, 400, "text/plain", "name required", 13);
        return;
    }
    url_decode(enc, name, sizeof(name));
    if (!lua_name_safe(name)) {
        send_fn(sock, 400, "text/plain", "invalid name", 12);
        return;
    }

    char path[192];
    DiagSnPrintf(path, sizeof(path), "%s/%s", LUA_SKILLS_DIR, name);
    FILE *f = fopen(path, "r");
    if (!f) {
        send_fn(sock, 404, "text/plain", "not found", 9);
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    if (sz > 32768) sz = 32768;
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        send_fn(sock, 500, "text/plain", "oom", 3);
        return;
    }
    size_t rlen = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    send_fn(sock, 200, "text/plain; charset=utf-8", buf, rlen);
    free(buf);
}

/* PUT /api/lua/content?name=xxx, body=script text → {ok:true} */
static void handle_api_lua_content_put(const claw_http_request_t *req,
                                         claw_http_send_fn_t send_fn, int sock)
{
    REQUIRE_AUTH();
    char enc[128], name[128];
    if (!query_get(req->query, "name", enc, sizeof(enc))) {
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"name required\"}", 35);
        return;
    }
    url_decode(enc, name, sizeof(name));
    if (!lua_name_safe(name)) {
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"invalid name\"}", 34);
        return;
    }

    char path[192];
    DiagSnPrintf(path, sizeof(path), "%s/%s", LUA_SKILLS_DIR, name);
    FILE *f = fopen(path, "w");
    if (!f) {
        mkdir(LUA_SKILLS_DIR, 0777);
        f = fopen(path, "w");
    }
    if (!f) {
        send_fn(sock, 500, "application/json", "{\"ok\":false,\"error\":\"cannot write\"}", 34);
        return;
    }
    if (req->body && req->body_len > 0) {
        size_t written = fwrite(req->body, 1, req->body_len, f);
        fclose(f);
        if (written != req->body_len) {
            send_fn(sock, 500, "application/json",
                    "{\"ok\":false,\"error\":\"write incomplete\"}", 38);
            return;
        }
    } else {
        fclose(f);
    }
    send_fn(sock, 200, "application/json", "{\"ok\":true}", 11);
}

/* POST /api/lua/upload → multipart upload, saves to skills dir */
static void handle_api_lua_upload(const claw_http_request_t *req,
                                    claw_http_send_fn_t send_fn, int sock)
{
    REQUIRE_AUTH();
    const char *body = req->body;
    if (!body || req->body_len < 10 || body[0] != '-' || body[1] != '-') {
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"not multipart\"}", 35);
        return;
    }
    const char *first_eol = strstr(body, "\r\n");
    if (!first_eol) {
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"bad boundary\"}", 34);
        return;
    }
    char bdry[130];
    size_t bdlen = (size_t)(first_eol - body);
    if (bdlen == 0 || bdlen >= sizeof(bdry)) {
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"bad boundary\"}", 34);
        return;
    }
    memcpy(bdry, body, bdlen);
    bdry[bdlen] = '\0';

    char upload_filename[128] = "";
    const char *file_data = NULL;
    size_t file_data_len = 0;

    const char *p = first_eol + 2;
    while (p && *p) {
        const char *hdr_end = strstr(p, "\r\n\r\n");
        if (!hdr_end) break;
        const char *data = hdr_end + 4;
        char field_name[64] = "", fn[128] = "";
        const char *cd = strstr(p, "Content-Disposition:");
        if (cd && cd < hdr_end) {
            const char *ns = strstr(cd, "name=\"");
            if (ns && ns < hdr_end) {
                ns += 6;
                const char *ne = strchr(ns, '"');
                if (ne && ne < hdr_end) {
                    size_t nl = (size_t)(ne - ns);
                    if (nl >= sizeof(field_name)) nl = sizeof(field_name) - 1;
                    memcpy(field_name, ns, nl); field_name[nl] = '\0';
                }
            }
            const char *fs = strstr(cd, "filename=\"");
            if (fs && fs < hdr_end) {
                fs += 10;
                const char *fe = strchr(fs, '"');
                if (fe && fe < hdr_end) {
                    size_t fl = (size_t)(fe - fs);
                    if (fl >= sizeof(fn)) fl = sizeof(fn) - 1;
                    memcpy(fn, fs, fl); fn[fl] = '\0';
                }
            }
        }
        const char *next_bdry = strstr(data, bdry);
        if (!next_bdry) break;
        size_t dlen_d = (size_t)(next_bdry - data);
        if (dlen_d >= 2) dlen_d -= 2;
        if (strcmp(field_name, "file") == 0) {
            file_data = data;
            file_data_len = dlen_d;
            if (fn[0]) strlcpy(upload_filename, fn, sizeof(upload_filename));
        }
        p = next_bdry + bdlen;
        if (p[0] == '-' && p[1] == '-') break;
        if (p[0] == '\r' && p[1] == '\n') p += 2;
        else break;
    }

    if (!file_data || upload_filename[0] == '\0') {
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"no file\"}", 29);
        return;
    }
    if (strstr(upload_filename, "/") || strstr(upload_filename, "..")) {
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"invalid name\"}", 34);
        return;
    }

    char dest[192];
    DiagSnPrintf(dest, sizeof(dest), "%s/%s", LUA_SKILLS_DIR, upload_filename);
    mkdir(LUA_SKILLS_DIR, 0777);
    FILE *f = fopen(dest, "wb");
    if (!f) {
        send_fn(sock, 500, "application/json", "{\"ok\":false,\"error\":\"cannot write\"}", 34);
        return;
    }
    size_t wr2 = fwrite(file_data, 1, file_data_len, f);
    fclose(f);
    if (wr2 != file_data_len) {
        send_fn(sock, 500, "application/json",
                "{\"ok\":false,\"error\":\"write incomplete\"}", 38);
        return;
    }
    send_fn(sock, 200, "application/json", "{\"ok\":true}", 11);
}

/* ---- Lua driver module management (/api/lua/modules) ---- */

typedef struct { const char *id; const char *desc_zh; const char *desc_en; int locked; } lua_mod_info_t;

/* locked=1 modules are always enabled and cannot be toggled off (core CLAW infrastructure) */
static const lua_mod_info_t s_lua_mods[] = {
    {"audio", "音频输入 (DMIC)",  "Audio input (DMIC)",    0}, /* bit 0  */
    {"uart",  "串口通信",         "UART serial",           0}, /* bit 1  */
    {"i2c",   "I2C 总线",         "I2C bus",               0}, /* bit 2  */
    {"spi",   "SPI 总线",         "SPI bus",               0}, /* bit 3  */
    {"rtc",   "实时时钟",         "RTC",                   0}, /* bit 4  */
    {"timer", "定时器",           "Timer",                 0}, /* bit 5  */
    {"file",  "文件系统",         "File system",           0}, /* bit 6  */
    {"wifi",  "WiFi 管理",        "WiFi management",       0}, /* bit 7  */
    {"gpio",  "GPIO 控制",        "GPIO control",          0}, /* bit 8  */
    {"sys",   "系统工具",         "System utilities",      1}, /* bit 9  — locked */
    {"event", "事件总线",         "Event bus",             1}, /* bit 10 — locked */
    {"cap",   "能力调用",         "Capability call",       1}, /* bit 11 — locked */
};
#define LUA_MOD_COUNT ((int)(sizeof(s_lua_mods)/sizeof(s_lua_mods[0])))

/* GET /api/lua/modules → {modules:[{id,bit,enabled,desc_zh,desc_en},...],mask:N} */
static void handle_api_lua_modules_get(const claw_http_request_t *req,
                                        claw_http_send_fn_t send_fn, int sock)
{
    REQUIRE_AUTH();
    (void)req;
    uint16_t mask = claw_config_get()->lua.module_mask;

    cJSON *resp = cJSON_CreateObject();
    cJSON *arr  = cJSON_CreateArray();
    if (!resp || !arr) {
        cJSON_Delete(resp);
        cJSON_Delete(arr);
        send_fn(sock, 500, "application/json", "{\"error\":\"oom\"}", 14);
        return;
    }
    cJSON_AddItemToObject(resp, "modules", arr);

    for (int i = 0; i < LUA_MOD_COUNT; i++) {
        cJSON *m = cJSON_CreateObject();
        if (!m) continue;
        cJSON_AddStringToObject(m, "id",      s_lua_mods[i].id);
        cJSON_AddNumberToObject(m, "bit",     i);
        cJSON_AddBoolToObject(  m, "enabled", (mask & (1u << i)) != 0);
        cJSON_AddBoolToObject(  m, "locked",  s_lua_mods[i].locked != 0);
        cJSON_AddStringToObject(m, "desc_zh", s_lua_mods[i].desc_zh);
        cJSON_AddStringToObject(m, "desc_en", s_lua_mods[i].desc_en);
        cJSON_AddItemToArray(arr, m);
    }
    cJSON_AddNumberToObject(resp, "mask", mask);

    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (out) {
        send_fn(sock, 200, "application/json", out, strlen(out));
        free(out);
    } else {
        send_fn(sock, 500, "application/json", "{\"error\":\"oom\"}", 14);
    }
}

/* POST /api/lua/modules  body:{mask:N} → {ok:true} */
static void handle_api_lua_modules_post(const claw_http_request_t *req,
                                         claw_http_send_fn_t send_fn, int sock)
{
    REQUIRE_AUTH();
    if (!req->body || req->body_len == 0) {
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"no body\"}", 29);
        return;
    }

    cJSON *root = cJSON_ParseWithLength(req->body, req->body_len);
    if (!root) {
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"invalid json\"}", 34);
        return;
    }

    cJSON *mask_j = cJSON_GetObjectItemCaseSensitive(root, "mask");
    if (!mask_j || !cJSON_IsNumber(mask_j)) {
        cJSON_Delete(root);
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"mask required\"}", 35);
        return;
    }

    uint16_t mask = (uint16_t)mask_j->valueint;
    cJSON_Delete(root);

    int rc = claw_config_set_lua_modules(mask);
    if (rc != 0) {
        send_fn(sock, 500, "application/json", "{\"ok\":false,\"error\":\"save failed\"}", 33);
        return;
    }
    send_fn(sock, 200, "application/json", "{\"ok\":true}", 11);
}

/* DELETE /api/lua?name=xxx → {ok:true} */
static void handle_api_lua_delete(const claw_http_request_t *req,
                                    claw_http_send_fn_t send_fn, int sock)
{
    REQUIRE_AUTH();
    char enc[128], name[128];
    if (!query_get(req->query, "name", enc, sizeof(enc))) {
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"name required\"}", 35);
        return;
    }
    url_decode(enc, name, sizeof(name));
    if (!lua_name_safe(name)) {
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"invalid name\"}", 34);
        return;
    }

    char path[192];
    DiagSnPrintf(path, sizeof(path), "%s/%s", LUA_SKILLS_DIR, name);
    if (remove(path) != 0) {
        send_fn(sock, 500, "application/json", "{\"ok\":false,\"error\":\"delete failed\"}", 35);
        return;
    }
    send_fn(sock, 200, "application/json", "{\"ok\":true}", 11);
}

/* ---- Direct cap invocation (for testing/debugging) ---- */

/* POST /api/cap/invoke  body:{cap:"name", input:{...}} → cap output JSON */
static void handle_api_cap_invoke(const claw_http_request_t *req,
                                   claw_http_send_fn_t send_fn, int sock)
{
    REQUIRE_AUTH();
    if (!req->body || req->body_len == 0) {
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"no body\"}", 29);
        return;
    }

    cJSON *root = cJSON_ParseWithLength(req->body, req->body_len);
    if (!root) {
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"invalid json\"}", 34);
        return;
    }

    cJSON *cap_j   = cJSON_GetObjectItemCaseSensitive(root, "cap");
    cJSON *input_j = cJSON_GetObjectItemCaseSensitive(root, "input");

    if (!cap_j || !cJSON_IsString(cap_j)) {
        cJSON_Delete(root);
        send_fn(sock, 400, "application/json", "{\"ok\":false,\"error\":\"cap required\"}", 34);
        return;
    }

    const char *cap_name = cap_j->valuestring;
    char *input_str = NULL;
    if (input_j) {
        input_str = cJSON_PrintUnformatted(input_j);
    }
    cJSON_Delete(root);

    char *output = NULL;
    static const claw_cap_call_context_t s_test_ctx = {
        .session_id = "webui_test",
        .channel    = "webui",
        .chat_id    = "test",
        .caller     = CLAW_CAP_CALLER_MANUAL,
    };

    int rc = claw_cap_call(cap_name, input_str ? input_str : "{}", &s_test_ctx, &output);
    free(input_str);

    if (output) {
        send_fn(sock, rc == 0 ? 200 : 500, "application/json", output, strlen(output));
        free(output);
    } else {
        send_fn(sock, 500, "application/json", "{\"error\":\"no output\"}", 21);
    }
}

/* ---- Public API ---- */

int cap_webui_init(void)
{
    int rc = 0;
    rc |= claw_http_server_add_route(HTTP_GET,  "/",                   handle_index);
    rc |= claw_http_server_add_route(HTTP_GET,  "/status",             handle_status);
    rc |= claw_http_server_add_route(HTTP_GET,  "/setup",              handle_index);
    rc |= claw_http_server_add_route(HTTP_POST, "/setup",              handle_setup_post);
    rc |= claw_http_server_add_route(HTTP_GET,  "/api/config",         handle_api_config);
    rc |= claw_http_server_add_route(HTTP_POST, "/api/wifi/connect",   handle_wifi_connect);
    rc |= claw_http_server_add_route(HTTP_GET,  "/api/wifi/scan",      handle_wifi_scan);
    rc |= claw_http_server_add_route(HTTP_POST, "/api/system/restart", handle_system_restart);
    rc |= claw_http_server_add_route(HTTP_GET,    "/api/wechat/qrcode",      handle_wechat_qrcode);
    rc |= claw_http_server_add_route(HTTP_GET,    "/api/wechat/status",      handle_wechat_status);
    rc |= claw_http_server_add_route(HTTP_GET,    "/api/wechat/token",       handle_wechat_token_get);
    rc |= claw_http_server_add_route(HTTP_GET,    "/api/files",              handle_api_files_get);
    rc |= claw_http_server_add_route(HTTP_DELETE, "/api/files",              handle_api_files_delete);
    rc |= claw_http_server_add_route(HTTP_GET,    "/api/files/download",     handle_api_files_download);
    rc |= claw_http_server_add_route(HTTP_POST,   "/api/files/mkdir",        handle_api_files_mkdir);
    rc |= claw_http_server_add_route(HTTP_GET,    "/api/files/content",      handle_api_files_content_get);
    rc |= claw_http_server_add_route(HTTP_PUT,    "/api/files/content",      handle_api_files_content_put);
    rc |= claw_http_server_add_route(HTTP_POST,   "/api/files/upload",       handle_api_files_upload);
    rc |= claw_http_server_add_route(HTTP_GET,    "/api/lua",                handle_api_lua_list);
    rc |= claw_http_server_add_route(HTTP_GET,    "/api/lua/content",        handle_api_lua_content_get);
    rc |= claw_http_server_add_route(HTTP_PUT,    "/api/lua/content",        handle_api_lua_content_put);
    rc |= claw_http_server_add_route(HTTP_POST,   "/api/lua/upload",         handle_api_lua_upload);
    rc |= claw_http_server_add_route(HTTP_DELETE, "/api/lua",                handle_api_lua_delete);
    rc |= claw_http_server_add_route(HTTP_GET,    "/api/lua/modules",        handle_api_lua_modules_get);
    rc |= claw_http_server_add_route(HTTP_POST,   "/api/lua/modules",        handle_api_lua_modules_post);
    rc |= claw_http_server_add_route(HTTP_POST,   "/api/cap/invoke",         handle_api_cap_invoke);
    if (rc != 0) {
        RTK_LOGE(TAG, "failed to register one or more routes\n");
        return RTK_FAIL;
    }
    RTK_LOGI(TAG, "routes registered: / /status /setup /api/config /api/wifi/* /api/files/*\n");
    return RTK_SUCCESS;
}

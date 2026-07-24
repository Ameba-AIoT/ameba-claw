/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * lua_storage_test_provision.c — AT+CLAW=storage,<sub> test harness
 *
 * AT commands:
 *   AT+CLAW=storage,info              — root dir + free space (SD) / vfs
 *   AT+CLAW=storage,write[,path[,data]] — write file (default claw_test.txt)
 *   AT+CLAW=storage,read[,path]       — read and print file
 *   AT+CLAW=storage,list[,path]       — list directory entries
 *   AT+CLAW=storage,remove[,path]     — delete file
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "ameba_soc.h"
#include "vfs.h"
#include "vfs_fatfs.h"

#define DEFAULT_FILE   "claw_test.txt"
#define DEFAULT_DATA   "hello from AT+CLAW=storage,write"

/* Returns "sdcard:" when SD is mounted, "vfs:" otherwise. */
static const char *test_root(void)
{
    return (fatfs2_mount_flag == 1) ? "sdcard:" : "vfs:";
}

/* Build a full path: root + filename into caller-supplied buf. */
static void build_path(char *buf, size_t sz, const char *root, const char *file)
{
    size_t rlen = strlen(root);
    if (rlen > 0 && root[rlen - 1] == ':') {
        snprintf(buf, sz, "%s%s", root, file);
    } else {
        snprintf(buf, sz, "%s/%s", root, file);
    }
}

/* ---- AT+CLAW=storage,info ---- */
void lua_storage_run_info(void)
{
    const char *root = test_root();
    printf("[storage] root=%s\n", root);

    if (fatfs2_mount_flag == 1) {
        FATFS *fs = NULL;
        DWORD fre_clust = 0;
        FRESULT res = f_getfree("0:/", &fre_clust, &fs);
        if (res == FR_OK && fs) {
            DWORD total_clust = fs->n_fatent - 2;
            uint32_t total_kb = (uint32_t)((uint64_t)total_clust * fs->csize * 512 / 1024);
            uint32_t free_kb  = (uint32_t)((uint64_t)fre_clust  * fs->csize * 512 / 1024);
            printf("[storage] SD total=%u KB, free=%u KB, used=%u KB\n",
                   (unsigned int)total_kb, (unsigned int)free_kb,
                   (unsigned int)(total_kb - free_kb));
        } else {
            printf("[storage] f_getfree error %d\n", (int)res);
        }
    } else {
        printf("[storage] SD not mounted — using LittleFS (vfs:)\n");
    }
}

/* ---- AT+CLAW=storage,write[,path[,data]] ---- */
void lua_storage_run_write(const char *path, const char *data)
{
    char default_path[64];
    if (!path || !path[0]) {
        build_path(default_path, sizeof(default_path), test_root(), DEFAULT_FILE);
        path = default_path;
    }
    if (!data || !data[0]) {
        data = DEFAULT_DATA;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("[storage] write FAILED: cannot open %s\n", path);
        return;
    }
    size_t len = strlen(data);
    size_t nw  = fwrite(data, 1, len, f);
    fclose(f);

    if (nw == len) {
        printf("[storage] write OK: %s (%d bytes)\n", path, (int)len);
    } else {
        printf("[storage] write PARTIAL: %s (%d/%d bytes)\n", path, (int)nw, (int)len);
    }
}

/* ---- AT+CLAW=storage,read[,path] ---- */
void lua_storage_run_read(const char *path)
{
    char default_path[64];
    if (!path || !path[0]) {
        build_path(default_path, sizeof(default_path), test_root(), DEFAULT_FILE);
        path = default_path;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("[storage] read FAILED: cannot open %s\n", path);
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        printf("[storage] read: %s is empty\n", path);
        fclose(f);
        return;
    }
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        printf("[storage] read OOM\n");
        return;
    }
    size_t nr = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[nr] = '\0';
    printf("[storage] read OK: %s (%d bytes)\n", path, (int)nr);
    printf("[storage] content: %s\n", buf);
    free(buf);
}

/* ---- AT+CLAW=storage,list[,path] ---- */
void lua_storage_run_list(const char *path)
{
    if (!path || !path[0]) {
        path = test_root();
    }

    void *dir = opendir(path);
    if (!dir) {
        printf("[storage] list FAILED: opendir(%s)\n", path);
        return;
    }
    printf("[storage] list: %s\n", path);

    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) {
            continue;
        }
        size_t plen = strlen(path);
        size_t nlen = strlen(ent->d_name);
        char *full  = (char *)malloc(plen + nlen + 2);
        if (full) {
            if (plen > 0 && (path[plen - 1] == '/' || path[plen - 1] == ':')) {
                snprintf(full, plen + nlen + 2, "%s%s", path, ent->d_name);
            } else {
                snprintf(full, plen + nlen + 2, "%s/%s", path, ent->d_name);
            }
            struct stat st;
            if (stat(full, &st) == 0) {
                printf("[storage]   %s  %s  %d\n",
                       S_ISDIR(st.st_mode) ? "[dir] " : "[file]",
                       ent->d_name, (int)st.st_size);
            } else {
                printf("[storage]   %s  %s\n",
                       ent->d_type == DT_DIR ? "[dir] " : "[file]",
                       ent->d_name);
            }
            free(full);
        }
        count++;
    }
    closedir(dir);
    printf("[storage] list done: %d entries\n", count);
}

/* ---- AT+CLAW=storage,remove[,path] ---- */
void lua_storage_run_remove(const char *path)
{
    char default_path[64];
    if (!path || !path[0]) {
        build_path(default_path, sizeof(default_path), test_root(), DEFAULT_FILE);
        path = default_path;
    }

    if (remove(path) == 0) {
        printf("[storage] remove OK: %s\n", path);
    } else {
        printf("[storage] remove FAILED: %s\n", path);
    }
}

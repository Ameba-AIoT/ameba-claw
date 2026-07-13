/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ameba_soc.h"
#include "atcmd_service.h"
#include "vfs.h"
#include <string.h>
#include <stdlib.h>

#define CLAWFS_PATH "vfs:/clawfs_test.txt"
#define CLAWFS_DATA "CLAWFS_OK"

/* ---- Background tasks ---- */

static void fs_rw_task(void *p)
{
    const char *op = p ? (const char *)p : "test";
    int err = 0;

    if (strcmp(op, "write") == 0 || strcmp(op, "test") == 0) {
        FILE *f = fopen(CLAWFS_PATH, "w");
        if (!f) { err = 1; goto done; }
        fwrite(CLAWFS_DATA, 1, strlen(CLAWFS_DATA), f);
        fclose(f);
        at_printf("+CLAW:fs,wrote %zu bytes to %s\r\n",
                  strlen(CLAWFS_DATA), CLAWFS_PATH);
    }
    if (strcmp(op, "read") == 0 || strcmp(op, "test") == 0) {
        FILE *f = fopen(CLAWFS_PATH, "r");
        if (!f) { err = 2; goto done; }
        char rbuf[64] = {0};
        size_t n = fread(rbuf, 1, sizeof(rbuf) - 1, f);
        fclose(f);
        rbuf[n] = '\0';
        at_printf("+CLAW:fs,read=%s,match=%s\r\n", rbuf,
                  strcmp(rbuf, CLAWFS_DATA) == 0 ? "OK" : "FAIL");
        if (strcmp(rbuf, CLAWFS_DATA) != 0) err = 3;
    }
done:
    if (p) rtos_mem_free(p);
    if (err == 0) at_printf(ATCMD_OK_END_STR);
    else          at_printf(ATCMD_ERROR_END_STR, err);
    rtos_task_delete(NULL);
}

static void fs_delete_task(void *p)
{
    char *path = (char *)p;
    int rc = path ? remove(path) : -1;
    at_printf("\r\n+CLAW:fs,delete=%s,%s\r\n",
              path ? path : "?", rc == 0 ? "ok" : "fail");
    rtos_mem_free(path);
    if (rc == 0) at_printf(ATCMD_OK_END_STR);
    else         at_printf(ATCMD_ERROR_END_STR, 2);
    rtos_task_delete(NULL);
}

static void list_vfs_task(void *p)
{
    (void)p;
    void *dir = opendir("vfs:/");
    if (!dir) {
        at_printf("\r\n+CLAW:fs,list=empty\r\n");
        rtos_task_delete(NULL);
        return;
    }
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        at_printf("+CLAW:fs,#%d=%s\r\n", count++, ent->d_name);
    }
    closedir(dir);
    at_printf("+CLAW:fs,total=%d\r\n", count);
    rtos_task_delete(NULL);
}

/* ---- Handler ---- */

void handle_cmd_fs(u16 argc, char **argv, const char *arg2, const char *arg3)
{
    (void)argc;
    (void)argv;

    const char *op = arg2[0] ? arg2 : "list";

    if (strcmp(op, "list") == 0) {
        if (rtos_task_create(NULL, "vfs_list", list_vfs_task,
                             NULL, 4096, 1) != RTK_SUCCESS) {
            at_printf(ATCMD_ERROR_END_STR, 1);
        } else {
            at_printf(ATCMD_OK_END_STR);
        }
        return;
    }

    if (strcmp(op, "delete") == 0) {
        const char *path = arg3[0] ? arg3 : "";
        if (!path[0]) {
            at_printf("\r\n+CLAW:usage: AT+CLAW=fs,delete,<path>\r\n");
            at_printf(ATCMD_ERROR_END_STR, 1);
            return;
        }
        /* spawn task — remove() calls LittleFS which needs more stack */
        char *path_copy = (char *)rtos_mem_malloc(128);
        if (path_copy) strlcpy(path_copy, path, 128);
        if (!path_copy || rtos_task_create(NULL, "fs_del", fs_delete_task,
                                           path_copy, 4096, 1) != RTK_SUCCESS) {
            rtos_mem_free(path_copy);
            at_printf(ATCMD_ERROR_END_STR, 1);
        } else {
            at_printf(ATCMD_OK_END_STR);
        }
        return;
    }

    /* write / read / test — spawn task (VFS needs more stack than AT task) */
    if (strcmp(op, "write") == 0 || strcmp(op, "read") == 0 || strcmp(op, "test") == 0) {
        char *op_copy = (char *)rtos_mem_malloc(8);
        if (op_copy) strlcpy(op_copy, op, 8);
        if (rtos_task_create(NULL, "fs_rw", fs_rw_task,
                             op_copy, 4096, 1) != RTK_SUCCESS) {
            rtos_mem_free(op_copy);
            at_printf(ATCMD_ERROR_END_STR, 1);
        } else {
            at_printf(ATCMD_OK_END_STR);
        }
        return;
    }

    at_printf("\r\n+CLAW:fs usage: list,write,read,test,delete,<path>\r\n");
    at_printf(ATCMD_ERROR_END_STR, 4);
}

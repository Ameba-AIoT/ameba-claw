/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * lua_module_storage.c — Lua `storage` module for Ameba RTOS
 *
 * Writable storage root:
 *   - SD card available (fatfs2_mount_flag == 1) → "sdcard:"  (FatFS, VFS_REGION_4)
 *   - No SD card                                 → "vfs:"     (LittleFS, VFS_REGION_1)
 *
 * SD hotplug is handled by registering fatfs_set_hotplug_usr_cb() at open time;
 * the callback mounts/unmounts the "sdcard:" region automatically.
 */

#define lua_lib
#include "ameba_soc.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "vfs.h"
#include "vfs_fatfs.h"
#include "ff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SDCARD_PREFIX     "sdcard:"
#define SDCARD_PREFIX_LEN 7
#define VFS_PREFIX_STR    "vfs:"
#define VFS_PREFIX_LEN    4

/* Returns the active writable root prefix. */
static const char *active_root(void)
{
    return (fatfs2_mount_flag == 1) ? SDCARD_PREFIX : VFS_PREFIX_STR;
}

/* Hotplug callback: mounts/unmounts "sdcard:" region on card insert/remove. */
static void storage_hotplug_cb(int status)
{
    if (status == HOTPULG_IN) {
        if (fatfs2_mount_flag != 1) {
            vfs_user_register(SDCARD_PREFIX, VFS_FATFS, VFS_INF_SD, VFS_REGION_4, VFS_RW);
        }
    } else {
        if (fatfs2_mount_flag == 1) {
            vfs_user_unregister(SDCARD_PREFIX, VFS_FATFS, VFS_INF_SD);
        }
    }
}

/* storage.get_root_dir() → "sdcard:" | "vfs:"
 * Verifies SD is actually openable before returning "sdcard:" to avoid
 * returning a root that fatfs2_mount_flag says is mounted but cannot be
 * accessed (e.g. card detected but physically absent). */
static int storage_get_root_dir(lua_State *L)
{
    if (fatfs2_mount_flag == 1) {
        void *dir = opendir(SDCARD_PREFIX);
        if (dir) {
            closedir(dir);
            lua_pushstring(L, SDCARD_PREFIX);
            return 1;
        }
        /* Registered but inaccessible — fall through to vfs: */
    }
    lua_pushstring(L, VFS_PREFIX_STR);
    return 1;
}

/* storage.join_path(part, ...) → joined path string
 * Separator is "/"; treats trailing ":" same as "/" so "vfs:" + "foo" → "vfs:foo". */
static int storage_join_path(lua_State *L)
{
    int argc = lua_gettop(L);
    luaL_Buffer buf;
    int wrote = 0;
    int ends_with_sep = 0;

    luaL_buffinit(L, &buf);

    for (int i = 1; i <= argc; i++) {
        size_t len = 0;
        size_t start = 0;
        size_t end = 0;
        const char *part = luaL_checklstring(L, i, &len);

        if (len == 0) {
            continue;
        }

        end = len;
        if (wrote) {
            while (start < end && part[start] == '/') {
                start++;
            }
        }
        while (end > start + 1 && part[end - 1] == '/') {
            end--;
        }
        if (end <= start) {
            continue;
        }

        if (wrote && !ends_with_sep) {
            luaL_addchar(&buf, '/');
        }
        luaL_addlstring(&buf, part + start, end - start);
        wrote = 1;
        ends_with_sep = (part[end - 1] == '/' || part[end - 1] == ':');
    }

    luaL_pushresult(&buf);
    return 1;
}

/* storage.exists(path) → boolean */
static int storage_exists(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    struct stat st;
    lua_pushboolean(L, stat(path, &st) == 0);
    return 1;
}

/* storage.stat(path) → {type, size} | nil, err */
static int storage_stat(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    struct stat st;

    if (stat(path, &st) != 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "stat failed: %s", path);
        return 2;
    }

    lua_newtable(L);
    lua_pushstring(L, S_ISDIR(st.st_mode) ? "dir" : "file");
    lua_setfield(L, -2, "type");
    lua_pushinteger(L, (lua_Integer)st.st_size);
    lua_setfield(L, -2, "size");
    return 1;
}

/* storage.mkdir(path) → true | error */
static int storage_mkdir(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);

    if (mkdir(path, 0755) != 0) {
        return luaL_error(L, "mkdir failed: %s", path);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* storage.write_file(path, data) → true | error */
static int storage_write_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);

    FILE *f = fopen(path, "wb");
    if (!f) {
        return luaL_error(L, "cannot open for write: %s", path);
    }
    size_t nw = fwrite(data, 1, len, f);
    fclose(f);
    if (nw != len) {
        return luaL_error(L, "short write to %s: %d/%d", path, (int)nw, (int)len);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* storage.read_file(path) → data | error */
static int storage_read_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);

    FILE *f = fopen(path, "rb");
    if (!f) {
        return luaL_error(L, "cannot open for read: %s", path);
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return luaL_error(L, "seek failed: %s", path);
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return luaL_error(L, "tell failed: %s", path);
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return luaL_error(L, "seek failed: %s", path);
    }
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return luaL_error(L, "out of memory reading %s", path);
    }
    size_t nr = (size > 0) ? fread(buf, 1, (size_t)size, f) : 0;
    fclose(f);
    lua_pushlstring(L, buf, nr);
    free(buf);
    return 1;
}

/* storage.listdir(path) → [{name, type, size}, ...] | error */
static int storage_listdir(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    int index = 1;

    void *dir = opendir(path);
    if (!dir) {
        return luaL_error(L, "opendir failed: %s", path);
    }

    lua_newtable(L);

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.' &&
                (ent->d_name[1] == '\0' ||
                 (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) {
            continue;
        }

        /* Build full path for stat: malloc to stay within stack budget. */
        size_t plen = strlen(path);
        size_t nlen = strlen(ent->d_name);
        char *full = (char *)malloc(plen + 1 + nlen + 1);
        if (!full) {
            closedir(dir);
            return luaL_error(L, "out of memory in listdir");
        }
        if (plen > 0 && path[plen - 1] == '/') {
            DiagSnPrintf(full, plen + 1 + nlen + 1, "%s%s", path, ent->d_name);
        } else {
            DiagSnPrintf(full, plen + 1 + nlen + 1, "%s/%s", path, ent->d_name);
        }

        lua_newtable(L);
        lua_pushstring(L, ent->d_name);
        lua_setfield(L, -2, "name");

        struct stat st;
        if (stat(full, &st) == 0) {
            lua_pushstring(L, S_ISDIR(st.st_mode) ? "dir" : "file");
            lua_setfield(L, -2, "type");
            lua_pushinteger(L, (lua_Integer)st.st_size);
            lua_setfield(L, -2, "size");
        } else {
            lua_pushstring(L, ent->d_type == DT_DIR ? "dir" : "file");
            lua_setfield(L, -2, "type");
            lua_pushinteger(L, 0);
            lua_setfield(L, -2, "size");
        }

        free(full);
        lua_rawseti(L, -2, index++);
    }

    closedir(dir);
    return 1;
}

/* storage.remove(path) → true | error */
static int storage_remove(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);

    if (remove(path) != 0) {
        return luaL_error(L, "remove failed: %s", path);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* storage.rename(old, new) → true | error */
static int storage_rename(lua_State *L)
{
    const char *old_path = luaL_checkstring(L, 1);
    const char *new_path = luaL_checkstring(L, 2);

    if (rename(old_path, new_path) != 0) {
        return luaL_error(L, "rename failed: %s -> %s", old_path, new_path);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* storage.get_free_space() → {total, free, used} | nil, err
 * Only available when SD card is mounted; FatFS drive 0. */
static int storage_get_free_space(lua_State *L)
{
    if (fatfs2_mount_flag != 1) {
        lua_pushnil(L);
        lua_pushstring(L, "SD card not mounted");
        return 2;
    }

    FATFS *fs = NULL;
    DWORD fre_clust = 0;
    FRESULT res = f_getfree("0:/", &fre_clust, &fs);
    if (res != FR_OK || fs == NULL) {
        lua_pushnil(L);
        lua_pushfstring(L, "f_getfree error: %d", (int)res);
        return 2;
    }

    /* Sector size is always 512 (FF_MIN_SS == FF_MAX_SS == 512 in ffconf.h).
     * Divide to KB here: lua_Integer is 32-bit (LUA_USE_C89 / long) on Cortex-M,
     * so raw byte counts overflow for SD cards larger than ~2 GB. KB fits up to
     * ~2 TB in int32 and is more useful to callers than raw bytes. */
    DWORD total_clust = fs->n_fatent - 2;
    uint64_t total_kb = ((uint64_t)total_clust * fs->csize * 512) / 1024;
    uint64_t free_kb  = ((uint64_t)fre_clust   * fs->csize * 512) / 1024;

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)total_kb);
    lua_setfield(L, -2, "total");
    lua_pushinteger(L, (lua_Integer)free_kb);
    lua_setfield(L, -2, "free");
    lua_pushinteger(L, (lua_Integer)(total_kb - free_kb));
    lua_setfield(L, -2, "used");
    return 1;
}

static const luaL_Reg storagelib[] = {
    {"get_root_dir",   storage_get_root_dir},
    {"join_path",      storage_join_path},
    {"exists",         storage_exists},
    {"stat",           storage_stat},
    {"mkdir",          storage_mkdir},
    {"write_file",     storage_write_file},
    {"read_file",      storage_read_file},
    {"listdir",        storage_listdir},
    {"remove",         storage_remove},
    {"rename",         storage_rename},
    {"get_free_space", storage_get_free_space},
    {NULL, NULL}
};

LUAMOD_API int luaopen_storage(lua_State *L)
{
    /* Register hotplug callback and attempt initial SD mount.
     * Guard matches SDK vfs_fatfs.c — only compiled when SD/USB-host hotplug enabled. */
#if defined(CONFIG_FATFS_SD_HOTPLUG) || defined(CONFIG_FATFS_USB_HOST)
    fatfs_set_hotplug_usr_cb(storage_hotplug_cb);
#endif
    if (fatfs2_mount_flag != 1) {
        vfs_user_register(SDCARD_PREFIX, VFS_FATFS, VFS_INF_SD, VFS_REGION_4, VFS_RW);
    }

    luaL_newlib(L, storagelib);
    return 1;
}

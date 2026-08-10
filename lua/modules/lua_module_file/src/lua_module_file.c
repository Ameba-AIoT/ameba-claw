/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
** lufile.c — Lua `file` module for Ameba RTOS
** Provides VFS file management: list, exists, remove, rename, run
*/

#define lua_lib
#include "ameba_soc.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VFS_PREFIX_STR "vfs:"
#define VFS_PREFIX_LEN 4

/* Ensure path has "vfs:" prefix. Returns pointer to static buffer. */
static const char *ensure_vfs_prefix(const char *name)
{
	static char buf[VFS_PATH_MAX + 1];

	if (strncmp(name, VFS_PREFIX_STR, VFS_PREFIX_LEN) == 0) {
		return name;
	}
	DiagSnPrintf(buf, sizeof(buf), "%s%s", VFS_PREFIX_STR, name);
	return buf;
}

/* file.list() -> table {name=size, ...}
 * d_reclen is the struct dirent size, NOT the file size — use fseek/ftell. */
static int file_list(lua_State *L)
{
	void *dir = opendir(VFS_PREFIX_STR);
	if (dir == NULL) {
		lua_newtable(L);
		return 1;
	}

	lua_newtable(L);
	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_type == DT_REG) {
			/* Measure actual file size via fseek/ftell.  d_reclen is the
			 * struct dirent size on this VFS implementation, not the file
			 * byte count (unlike POSIX d_reclen semantics). */
			char fpath[VFS_PATH_MAX + 1];
			DiagSnPrintf(fpath, sizeof(fpath), "%s%s", VFS_PREFIX_STR, ent->d_name);
			long fsize = -1;
			FILE *f = fopen(fpath, "rb");
			if (f) {
				fseek(f, 0, SEEK_END);
				fsize = ftell(f);
				fclose(f);
			}
			lua_pushstring(L, ent->d_name);
			lua_pushinteger(L, (lua_Integer)(fsize >= 0 ? fsize : 0));
			lua_settable(L, -3);
		}
	}
	closedir(dir);
	return 1;
}

/* file.exists("name") -> boolean */
static int file_exists(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	const char *path = ensure_vfs_prefix(name);
	lua_pushboolean(L, access(path, F_OK) == 0);
	return 1;
}

/* file.remove("name") -> boolean */
static int file_remove(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	const char *path = ensure_vfs_prefix(name);
	lua_pushboolean(L, remove(path) == 0);
	return 1;
}

/* file.rename("old", "new") -> boolean */
static int file_rename(lua_State *L)
{
	const char *oldname = luaL_checkstring(L, 1);
	const char *newname = luaL_checkstring(L, 2);
	const char *oldpath = ensure_vfs_prefix(oldname);
	/* ensure_vfs_prefix returns static buffer, so we must copy oldpath */
	char oldbuf[VFS_PATH_MAX + 1];
	strncpy(oldbuf, oldpath, sizeof(oldbuf) - 1);
	oldbuf[sizeof(oldbuf) - 1] = '\0';
	const char *newpath = ensure_vfs_prefix(newname);
	lua_pushboolean(L, rename(oldbuf, newpath) == 0);
	return 1;
}

/* file.mkdir("path") — create VFS directory, returns true | nil, err */
static int file_mkdir(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	const char *path = ensure_vfs_prefix(name);
	if (mkdir(path, 0777) == 0) {
		lua_pushboolean(L, 1);
		return 1;
	}
	lua_pushnil(L);
	lua_pushfstring(L, "mkdir '%s' failed", name);
	return 2;
}

/* file.write("name", data) — write binary data to VFS file, returns true | nil, err */
static int file_write(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	size_t len;
	const char *data = luaL_checklstring(L, 2, &len);

	char path[VFS_PATH_MAX + 1];
	if (strncmp(name, VFS_PREFIX_STR, VFS_PREFIX_LEN) == 0) {
		snprintf(path, sizeof(path), "%s", name);
	} else {
		snprintf(path, sizeof(path), "%s%s", VFS_PREFIX_STR, name);
	}

	FILE *f = fopen(path, "wb");
	if (f == NULL) {
		lua_pushnil(L);
		lua_pushfstring(L, "cannot open '%s' for writing", name);
		return 2;
	}
	size_t nw = fwrite(data, 1, len, f);
	fclose(f);

	if (nw != len) {
		lua_pushnil(L);
		lua_pushfstring(L, "write '%s': wrote %d/%d bytes", name, (int)nw, (int)len);
		return 2;
	}
	lua_pushboolean(L, 1);
	return 1;
}

/* file.read("name") — read binary data from VFS file, returns data | nil, err */
static int file_read(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);

	char path[VFS_PATH_MAX + 1];
	if (strncmp(name, VFS_PREFIX_STR, VFS_PREFIX_LEN) == 0) {
		snprintf(path, sizeof(path), "%s", name);
	} else {
		snprintf(path, sizeof(path), "%s%s", VFS_PREFIX_STR, name);
	}

	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		lua_pushnil(L);
		lua_pushfstring(L, "cannot open '%s'", name);
		return 2;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0) {
		fclose(f);
		lua_pushlstring(L, "", 0);
		return 1;
	}
	char *buf = (char *)malloc((size_t)size);
	if (!buf) {
		fclose(f);
		return luaL_error(L, "not enough memory to read '%s'", name);
	}
	size_t nr = fread(buf, 1, (size_t)size, f);
	fclose(f);
	lua_pushlstring(L, buf, nr);
	free(buf);
	return 1;
}

/* file.run("name") — execute script, forward its return values */
static int file_run(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	const char *path = ensure_vfs_prefix(name);

	/* Use fread+luaL_loadstring instead of luaL_loadfile: VFS FILE* objects
	 * do not have retarget locks initialized, so getc() (used by luaL_loadfile)
	 * triggers a NULL-lock assert in locks.c. */
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		lua_pushnil(L);
		lua_pushfstring(L, "cannot open '%s'", name);
		return 2;
	}

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (size <= 0) {
		fclose(f);
		lua_pushnil(L);
		lua_pushfstring(L, "empty file '%s'", name);
		return 2;
	}

	char *buf = (char *)malloc((size_t)size + 1);
	if (buf == NULL) {
		fclose(f);
		return luaL_error(L, "not enough memory to load '%s'", name);
	}

	size_t nread = fread(buf, 1, (size_t)size, f);
	buf[nread] = '\0';
	fclose(f);

	int status = luaL_loadstring(L, buf);
	free(buf);

	if (status != LUA_OK) {
		lua_pushnil(L);
		lua_insert(L, -2);
		return 2;
	}

	int base = lua_gettop(L) - 1;
	status = lua_pcall(L, 0, LUA_MULTRET, 0);
	if (status != LUA_OK) {
		lua_pushnil(L);
		lua_insert(L, -2);
		return 2;
	}

	return lua_gettop(L) - base;
}

static const luaL_Reg filelib[] = {
	{"list",   file_list},
	{"exists", file_exists},
	{"remove", file_remove},
	{"rename", file_rename},
	{"mkdir",  file_mkdir},
	{"write",  file_write},
	{"read",   file_read},
	{"run",    file_run},
	{NULL, NULL}
};

LUAMOD_API int luaopen_file(lua_State *L)
{
	luaL_newlib(L, filelib);
	return 1;
}

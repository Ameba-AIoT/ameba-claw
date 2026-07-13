/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lauxlib.h"
#include "lualib.h"
#include "lua_module_registry.h"
#include "platform_stdlib.h"
#include "basic_types.h"
#include <stdlib.h>
#include "os_wrapper.h"
#include "sys_api.h"

#define LUA_SCRIPT_PATH "vfs:main.lua"

static const char default_script[] =
	"print('=== Ameba Lua RTOS ===')\n"
	"print('Lua ready.')\n"
	"print()\n"
	"for k, v in pairs(file.list()) do\n"
	"  print('  ' .. k .. '  (' .. tostring(v) .. ' bytes)')\n"
	"end\n"
	"print()\n";

static void provision_default_script(void)
{
	FILE *f = fopen(LUA_SCRIPT_PATH, "w");
	if (f == NULL) {
		printf("Lua: failed to create %s\n", LUA_SCRIPT_PATH);
		return;
	}
	size_t len = strlen(default_script);
	size_t written = fwrite(default_script, 1, len, f);
	fclose(f);
	if (written == len) {
		printf("Lua: provisioned %s (%d bytes)\n", LUA_SCRIPT_PATH, (int)written);
	} else {
		printf("Lua: write incomplete %d/%d\n", (int)written, (int)len);
	}
}

/* VFS-safe dofile: replaces standard dofile which uses getc() — not in the
 * VFS redirect list, so it reads fp->_lock on the fake vfs_file* and crashes.
 * Uses fread+luaL_loadbuffer (both VFS-safe) instead. */
static int lua_vfs_dofile(lua_State *L)
{
	const char *fname = luaL_checkstring(L, 1);
	lua_settop(L, 1);

	FILE *f = fopen(fname, "r");
	if (f == NULL) {
		return luaL_error(L, "dofile: cannot open '%s'", fname);
	}

	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (sz <= 0) {
		fclose(f);
		return 0;
	}

	char *buf = (char *)malloc((size_t)sz + 1);
	if (buf == NULL) {
		fclose(f);
		return luaL_error(L, "dofile: out of memory");
	}

	size_t n = fread(buf, 1, (size_t)sz, f);
	buf[n] = '\0';
	fclose(f);

	if (luaL_loadbuffer(L, buf, n, fname) != LUA_OK) {
		free(buf);
		return lua_error(L);
	}
	free(buf);

	lua_call(L, 0, LUA_MULTRET);
	return lua_gettop(L) - 1;
}

static void run_script(lua_State *L, const char *path)
{
	/* Read file content via fread (VFS-safe), then load as string.
	 * luaL_loadfile uses getc + ferror which conflicts with VFS wrapping. */
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		printf("Lua: failed to open %s\n", path);
		return;
	}

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (size <= 0) {
		fclose(f);
		return;
	}

	char *buf = (char *)malloc((size_t)size + 1);
	if (buf == NULL) {
		fclose(f);
		printf("Lua: out of memory\n");
		return;
	}

	size_t nread = fread(buf, 1, (size_t)size, f);
	buf[nread] = '\0';
	fclose(f);

	if (luaL_loadstring(L, buf) != LUA_OK) {
		printf("Lua: failed to parse %s: %s\n", path, lua_tostring(L, -1));
		lua_pop(L, 1);
		free(buf);
		return;
	}
	free(buf);

	if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
		printf("Lua error: %s\n", lua_tostring(L, -1));
		lua_pop(L, 1);
	}
}

static int lua_reboot(lua_State *L)
{
	(void)L;
	sys_reset();
	return 0;
}

void lua_task(void *param)
{
	(void)param;

	lua_State *L = luaL_newstate();
	if (L == NULL) {
		printf("Failed to create Lua state\n");
		rtos_task_delete(NULL);
		return;
	}

	/* Provision default script and hardware test scripts.
	 * Speaker test (test_speaker.lua) is auto-provisioned on every boot.
	 * DMIC test (test_dmic.lua) is also provisioned but not auto-run;
	 * trigger via AT+CLAW=dmic or dofile("vfs:test_dmic.lua") from REPL. */
	provision_default_script();
	/* Boot-time test scripts for every provisioning-capable module, driven by
	 * the registry — no hand-maintained per-driver list to keep in sync. */
	lua_module_registry_provision_all();

	luaL_openlibs(L);

	lua_pushcfunction(L, lua_reboot);
	lua_setglobal(L, "reboot");

	/* Override standard dofile: VFS fopen returns a fake FILE* (vfs_file* cast),
	 * so getc() inside luaL_loadfile crashes on the zeroed _lock field. */
	lua_pushcfunction(L, lua_vfs_dofile);
	lua_setglobal(L, "dofile");

	printf("Lua: loading %s\n", LUA_SCRIPT_PATH);
	run_script(L, LUA_SCRIPT_PATH);

	/* REPL is triggered via AT+CLAW=lua, not auto-started here.
	 * UART AT test: AT+CLAW=uart_test, or run from REPL:
	 *   dofile("vfs:test_uart_at.lua") */

	lua_close(L);
	rtos_task_delete(NULL);
}

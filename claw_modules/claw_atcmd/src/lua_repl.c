/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
** lua_repl.c — Interactive Lua REPL for Ameba RTOS
**
** Runs on the log UART console. Disables the shell RX interrupt
** so characters are consumed by the REPL instead of the shell.
*/

#include "lua_repl.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "ameba_soc.h"

#include <string.h>
#include <stdbool.h>

#define REPL_LINE_MAX 512

/* Pending character from \r\n pair consumption (-1 = empty) */
static int s_pending = -1;

/* Read one character, checking pending buffer first */
static char getch(void)
{
	if (s_pending >= 0) {
		char c = (char)s_pending;
		s_pending = -1;
		return c;
	}
	return (char)DiagGetChar(true);
}

/* Read one line from log UART with echo and basic editing */
static int read_line(char *buf, int maxlen)
{
	int pos = 0;

	while (pos < maxlen - 1) {
		char c = getch();

		if (c == '\r' || c == '\n') {
			/* Enter — consume the paired \n after \r (or \r after \n).
			 * Brief delay to let UART finish receiving the pair byte.
			 * At 1500000 baud one char takes ~7us; at 240MHz ~1680 cycles.
			 */
			volatile int wait = 5000;
			while (wait--) {}
			if (LOGUART_Readable()) {
				u8 next = DiagGetChar(false);
				if (next != '\r' && next != '\n') {
					s_pending = (int)next;  /* Not a pair — save for next read */
				}
			}
			DiagPutChar('\r');
			DiagPutChar('\n');
			buf[pos] = '\0';
			return pos;
		} else if (c == 0x7F || c == 0x08) {
			/* Backspace */
			if (pos > 0) {
				pos--;
				DiagPutChar(0x08);
				DiagPutChar(' ');
				DiagPutChar(0x08);
			}
		} else if (c >= 0x20) {
			/* Printable character */
			buf[pos++] = c;
			DiagPutChar(c);
		}
		/* Ignore other control characters */
	}
	buf[pos] = '\0';
	return pos;
}

/* Print Lua value on the stack, pop it */
static void print_lua_value(lua_State *L, int idx)
{
	const char *s;
	size_t len;

	switch (lua_type(L, idx)) {
	case LUA_TNIL:
		printf("nil");
		break;
	case LUA_TBOOLEAN:
		printf(lua_toboolean(L, idx) ? "true" : "false");
		break;
	case LUA_TNUMBER:
		if (lua_isinteger(L, idx)) {
			printf(LUA_INTEGER_FMT, lua_tointeger(L, idx));
		} else {
			printf("%.14g", lua_tonumber(L, idx));
		}
		break;
	case LUA_TSTRING:
		s = lua_tolstring(L, idx, &len);
		printf("%s", s);
		break;
	default:
		printf("%s: %p", luaL_typename(L, idx), lua_topointer(L, idx));
		break;
	}
}

/* Check if error message indicates incomplete input */
static bool is_incomplete_chunk(const char *errmsg)
{
	if (errmsg == NULL) {
		return false;
	}
	return strstr(errmsg, "<eof>") != NULL;
}

void lua_repl_run(lua_State *L)
{
	char line[REPL_LINE_MAX];
	char chunk[REPL_LINE_MAX * 4];
	int chunk_len = 0;
	bool need_more = false;

	/* Disable log UART RX interrupt to stop shell from eating chars */
	LOGUART_INTConfig(LOGUART_DEV, LOGUART_BIT_ERBI, 0);

	/* Drain any stale data in UART RX buffer from boot */
	s_pending = -1;
	while (LOGUART_Readable()) {
		(void)DiagGetChar(false);
	}

	while (1) {
		/* Show prompt */
		if (need_more) {
			printf(">> ");
		} else {
			printf("> ");
		}

		int linelen = read_line(line, sizeof(line));

		/* Handle empty line */
		if (linelen == 0) {
			if (need_more) {
				/* Append newline to continuation */
				if (chunk_len + 1 < (int)sizeof(chunk)) {
					chunk[chunk_len++] = '\n';
					chunk[chunk_len] = '\0';
				}
			}
			continue;
		}

		/* Handle exit */
		if (!need_more && strcmp(line, "exit()") == 0) {
			printf("Lua REPL exited.\n");
			break;
		}

		/* Append line to chunk buffer */
		if (chunk_len + linelen + 2 >= (int)sizeof(chunk)) {
			printf("REPL: input too long\n");
			chunk_len = 0;
			need_more = false;
			continue;
		}
		memcpy(chunk + chunk_len, line, linelen);
		chunk_len += linelen;
		chunk[chunk_len++] = '\n';
		chunk[chunk_len] = '\0';

		/* Try to compile and run */
		int status = luaL_loadstring(L, chunk);

		if (status == LUA_ERRSYNTAX && is_incomplete_chunk(lua_tostring(L, -1))) {
			/* Incomplete chunk — wait for more input */
			lua_pop(L, 1);
			need_more = true;
			continue;
		}

		if (status != LUA_OK) {
			/* Compile error */
			printf("%s\n", lua_tostring(L, -1));
			lua_pop(L, 1);
			chunk_len = 0;
			need_more = false;
			continue;
		}

		/* Compile succeeded — execute */
		need_more = false;
		chunk_len = 0;

		status = lua_pcall(L, 0, LUA_MULTRET, 0);
		if (status != LUA_OK) {
			printf("%s\n", lua_tostring(L, -1));
			lua_pop(L, 1);
			continue;
		}

		/* Print non-nil return values */
		int nres = lua_gettop(L);
		if (nres > 0) {
			for (int i = 1; i <= nres; i++) {
				if (!lua_isnil(L, i)) {
					print_lua_value(L, i);
					printf("\n");
				}
			}
		}
		lua_settop(L, 0);
	}

	/* Re-enable log UART RX interrupt */
	LOGUART_INTConfig(LOGUART_DEV, LOGUART_BIT_ERBI, 1);
}

void lua_run_repl_once(void)
{
	lua_State *L = luaL_newstate();
	if (!L) {
		printf("Lua REPL: failed to create state\n");
		return;
	}
	/* luaL_openlibs installs REPL modules via lua_module_registry_install.
	 * Hardware driver mutexes (gpio/i2c/rtc) are created by
	 * lua_module_registry_provision_all() in lua_task() during boot; the
	 * _init functions guard with "if (lock == NULL)" so it is safe to call
	 * even if lua_task ran first (the normal path on this platform). */
	luaL_openlibs(L);
	lua_repl_run(L);
	lua_close(L);
}

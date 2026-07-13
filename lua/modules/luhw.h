/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
** luhw.h — Shared hardware helpers for Lua peripheral modules.
**
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef LUHW_H
#define LUHW_H

#include "lua.h"
#include "lauxlib.h"
#include "PinNames.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/*
** Parse a pin argument from the Lua stack.
** Accepts:
**   integer  — raw PinName value (e.g. 0x12 for PA_18)
**   string   — pin name like "PA_18", "PB_8", case-insensitive
** Calls luaL_error() on invalid input.
*/
static inline PinName luhw_check_pin(lua_State *L, int arg)
{
	if (lua_type(L, arg) == LUA_TNUMBER) {
		lua_Integer v = luaL_checkinteger(L, arg);
		return (PinName)(int)v;
	}

	const char *s = luaL_checkstring(L, arg);
	/* Accept "PA_0" .. "PA_31", "PB_0" .. "PB_31", "PC_0" .. "PC_8" */
	char port = 0;
	if (s[0] == 'p' || s[0] == 'P') {
		if (s[1] == 'a' || s[1] == 'A') {
			port = 'A';
		} else if (s[1] == 'b' || s[1] == 'B') {
			port = 'B';
		} else if (s[1] == 'c' || s[1] == 'C') {
			port = 'C';
		}
	}

	if (port == 0 || (s[2] != '_' && s[2] != '\0')) {
		luaL_error(L, "invalid pin name '%s' (expected PA_0..PA_31, PB_0..PB_31, PC_0..PC_8)",
			   s);
	}

	const char *num_str = (s[2] == '_') ? &s[3] : &s[2];
	char *end;
	long pin = strtol(num_str, &end, 10);
	if (*end != '\0' || pin < 0 || pin > 31) {
		luaL_error(L, "invalid pin number in '%s'", s);
	}

	int port_val;
	if (port == 'A') {
		port_val = 0;
	} else if (port == 'B') {
		port_val = 1;
	} else {
		if (pin > 8) {
			luaL_error(L, "invalid pin '%s': PC supports pins 0-8 only", s);
		}
		port_val = 2;
	}

	return (PinName)((port_val << 5) | (int)pin);
}

/*
** Convert a PinName integer back to its canonical string form.
** Writes into buf (caller supplies, minimum 8 bytes).
** Returns buf.  Used to push consistent string pin names into Lua event tables
** so that ev.pin matches the string form accepted by all driver APIs.
*/
static inline const char *luhw_pin_to_str(PinName pin, char *buf, size_t buflen)
{
	int v = (int)pin;
	int port_idx = v >> 5;
	int pin_num  = v & 0x1F;
	char port_char = (port_idx == 0) ? 'A' : (port_idx == 1) ? 'B' : 'C';
	snprintf(buf, buflen, "P%c_%d", port_char, pin_num);
	return buf;
}

#endif /* LUHW_H */

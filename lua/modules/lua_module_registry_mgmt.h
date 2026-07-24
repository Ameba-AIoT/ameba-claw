/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * lua_module_registry_mgmt.h — management API that does not depend on lua.h.
 *
 * Include this header (instead of lua_module_registry.h) in translation units
 * that only need the config/filter hooks and must not pull in lua.h.
 * lua_module_registry.h includes this file automatically.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One-time init: creates the internal mutex. Must be called once at boot,
 * single-threaded, before lua_module_registry_set_disabled() or
 * lua_module_registry_install() are called from any task.
 */
void lua_module_registry_init(void);

/*
 * Set the comma-separated list of disabled module names.
 * The string is copied internally; the caller's buffer need not remain valid
 * after this call returns.  Thread-safe (protected by an internal mutex).
 * Call once at boot from claw_config, and again whenever the user updates
 * the list at runtime.
 */
void lua_module_registry_set_disabled(const char *disabled_csv);

/*
 * Chip peripheral filter: set a callback that returns 1 if a named peripheral
 * exists on the current board chip, 0 if absent, -1 on error.  Cap_board_mgr
 * installs this once after loading the board JSON.  NULL = allow all.
 */
typedef int (*lua_chip_filter_fn)(const char *chip_peripheral_key);
void lua_module_registry_set_chip_filter(lua_chip_filter_fn fn);

/* Return 1 if 'chip_peripheral' passes the installed chip filter (or no filter
 * is set), 0 if the filter rejects it. */
int lua_module_registry_chip_ok(const char *chip_peripheral);

/*
 * Return 1 if 'name' appears as a token in the comma-separated 'csv',
 * 0 otherwise.  Safe to call with NULL arguments.
 * For external callers (e.g. cap_webui filter) that hold their own CSV
 * string — does NOT access the registry's internal disabled buffer.
 */
int lua_module_registry_csv_contains(const char *csv, const char *name);

#ifdef __cplusplus
}
#endif

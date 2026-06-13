/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * lua_module_registry.h — single source of truth for Lua C modules.
 *
 * Historically the set of Lua modules was hand-maintained in three separate
 * places that had to be kept in sync by hand:
 *   1. linit.c          — the full REPL / lua_task environment
 *   2. cap_skill_mgr.c  — the restricted sandbox used to run skills
 *   3. lua_main.c       — the list of boot-time test-script provisioners
 *
 * This registry replaces those three lists with one table (see
 * lua_module_registry.c). Each module declares its category (software vs
 * hardware), how it is loaded (eager vs preload), and which environments it
 * belongs to (REPL and/or the skill sandbox). Disabled modules
 * (lua_modules_config.h) drop out of the table automatically.
 */
#pragma once

#include <stddef.h>
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Software helper vs hardware-peripheral driver. Purely descriptive: lets the
 * REPL banner / debug tooling group modules, and documents the layering. */
typedef enum {
    LUA_MOD_CAT_SW = 0,   /* pure-software module (cap, event, file, ...)      */
    LUA_MOD_CAT_HW = 1,   /* hardware-peripheral driver (gpio, i2c, ...)       */
} lua_module_category_t;

/* How the module is installed into a lua_State. */
typedef enum {
    LUA_MOD_LOAD_EAGER   = 0, /* luaL_requiref at openlibs (always present)    */
    LUA_MOD_LOAD_PRELOAD = 1, /* package.preload[name] — loaded on require()   */
} lua_module_load_t;

/* Environment membership flags. */
#define LUA_MOD_ENV_REPL    (1u << 0) /* full REPL / lua_task environment      */
#define LUA_MOD_ENV_SKILL   (1u << 1) /* sandboxed skill execution environment */
#define LUA_MOD_ENV_TIMER   (1u << 2) /* timer callback sandbox (lightweight;
                                        * no blocking I/O, no heavy HW init)   */

typedef struct {
    const char            *name;        /* require() name, e.g. "gpio"         */
    lua_CFunction          open_fn;     /* luaopen_<name>                      */
    void                 (*provision_fn)(void); /* optional: write test script
                                                 * to VFS at boot (REPL only)  */
    lua_module_category_t  category;
    lua_module_load_t      load;
    unsigned               env_flags;   /* OR of LUA_MOD_ENV_*                 */
} lua_module_desc_t;

/* Returns the module table and writes its length to *count (never NULL). */
const lua_module_desc_t *lua_module_registry(size_t *count);

/*
 * Install the registry's modules into L.
 *
 *   env_flag — LUA_MOD_ENV_REPL or LUA_MOD_ENV_SKILL: only modules tagged with
 *              that flag are installed.
 *
 * Eager modules are registered with luaL_requiref (global). Preload modules are
 * placed into package.preload so they load lazily on require() — but only when
 * the target environment has a package table (the REPL); in the skill sandbox,
 * preload modules tagged LUA_MOD_ENV_SKILL are installed eagerly instead.
 *
 * Note: this installs only the Ameba custom modules. Standard-library setup
 * (base/string/math/table/package and any sandbox stripping) remains the
 * caller's responsibility, since the two environments sandbox the stdlib
 * differently.
 */
void lua_module_registry_install(lua_State *L, unsigned env_flag);

/*
 * Run every module's provision_fn (boot-time test-script writers). Only modules
 * that define a provision_fn and are tagged LUA_MOD_ENV_REPL participate.
 */
void lua_module_registry_provision_all(void);

#ifdef __cplusplus
}
#endif

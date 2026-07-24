/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Private header: declares per-subcommand handler functions called from
 * at_claw() in cap_atcmd.c. Each handler owns its own .c file.
 */

#pragma once

#include "ameba_soc.h"
#include "ameba_claw_defs.h"   /* CLAW_AGENT_AUTO_TEST gate */

/* ask */
void handle_cmd_ask(u16 argc, char **argv, const char *arg2, const char *arg3);
void handle_cmd_ask_buf(u16 argc, char **argv, const char *arg2);

/* lua — REPL (own lua_State) + direct-by-path execution (shares cap_lua's
 * job table/core with the LLM tools and thread.run/start — see
 * design_spec/lua/lua_module_thread_architecture.md). All in atcmd_lua.c. */
void handle_cmd_lua_repl(void);
void handle_cmd_lua_execute_sync(u16 argc, char **argv, const char *arg2, const char *arg3);
void handle_cmd_lua_execute_async(u16 argc, char **argv, const char *arg2, const char *arg3);

/* cfg + wifi */
void handle_cmd_cfg(u16 argc, char **argv, const char *arg2, const char *arg3);
void handle_cmd_wifi(const char *arg2);

/* im — IM channel config (telegram/feishu/qq/wechat) */
void handle_cmd_im(u16 argc, char **argv, const char *arg2, const char *arg3);

/* session */
void handle_cmd_session(u16 argc, char **argv, const char *arg2, const char *arg3);

/* memory */
void handle_cmd_memory(const char *arg2);

/* cap + tools */
void handle_cmd_cap(u16 argc, char **argv, const char *arg2, const char *arg3);
void handle_cmd_tools(const char *arg2);

/* fs */
void handle_cmd_fs(u16 argc, char **argv, const char *arg2, const char *arg3);

/* sys */
void handle_cmd_sys(const char *arg2);

/* gpio_ctrl — test-bench GPIO button stimulus (only when CLAW_AGENT_AUTO_TEST) */
#if CLAW_AGENT_AUTO_TEST
void handle_cmd_gpio_ctrl(u16 argc, char **argv, const char *arg2, const char *arg3);
#endif

/* hardware driver tests — returns 1 if sub matched, 0 if not */
int  handle_cmd_hw_test(u16 argc, char **argv, const char *sub,
                        const char *arg2, const char *arg3);

/* unit tests (only present when CONFIG_CLAW_BUILD_TESTS is defined) */
#ifdef CONFIG_CLAW_BUILD_TESTS
void handle_cmd_test(const char *arg2);
#endif

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

/* ask + lua */
void handle_cmd_ask(u16 argc, char **argv, const char *arg2, const char *arg3);
void handle_cmd_ask_buf(u16 argc, char **argv, const char *arg2);
void handle_cmd_lua(void);

/* cfg + wifi + wechat */
void handle_cmd_cfg(u16 argc, char **argv, const char *arg2, const char *arg3);
void handle_cmd_wifi(const char *arg2);
void handle_cmd_wechat(const char *arg2);

/* session */
void handle_cmd_session(u16 argc, char **argv, const char *arg2, const char *arg3);

/* memory */
void handle_cmd_memory(const char *arg2);

/* cap + tools + skill */
void handle_cmd_cap(u16 argc, char **argv, const char *arg2, const char *arg3);
void handle_cmd_tools(const char *arg2);
void handle_cmd_skill(u16 argc, char **argv, const char *arg2, const char *arg3);

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

/* unit tests (only present when CLAW_BUILD_TESTS is defined) */
#ifdef CLAW_BUILD_TESTS
void handle_cmd_test(const char *arg2);
#endif

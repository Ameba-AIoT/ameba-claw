/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stddef.h>

/* Returns number of failures (0 = all passed). Results written to buf. */
int claw_test_cap(char *buf, size_t bufsz);
int claw_test_mem(char *buf, size_t bufsz);
int claw_test_router(char *buf, size_t bufsz);
int claw_test_fs(char *buf, size_t bufsz);
int claw_test_cap_hash_collision(char *buf, size_t bufsz);
int claw_test_agent_loop_stub(char *buf, size_t bufsz);
int claw_test_session_mgr(char *buf, size_t bufsz);
int claw_test_all(char *buf, size_t bufsz);

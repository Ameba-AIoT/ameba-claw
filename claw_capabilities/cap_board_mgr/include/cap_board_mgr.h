/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "claw_agent.h"

typedef struct {
    const char *vfs_path;   /* e.g. "vfs:/board.json"; NULL → use default */
} cap_board_mgr_config_t;

int cap_board_mgr_init(const cap_board_mgr_config_t *config);

/* Returns 1 if the named peripheral is listed in the current chip constraints,
 * 0 if not supported, -1 if no board is loaded (treat as "allow all"). */
int cap_board_mgr_chip_has_peripheral(const char *peripheral_name);

extern const claw_agent_context_provider_t cap_board_mgr_context_provider;

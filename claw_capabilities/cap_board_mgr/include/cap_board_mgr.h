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

extern const claw_agent_context_provider_t cap_board_mgr_context_provider;

/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "claw_compat.h"

typedef struct {
    const char *rules_dir;   /* e.g. "/router_rules" */
    size_t max_rules;        /* default 32 */
} cap_router_mgr_config_t;

int cap_router_mgr_init(const cap_router_mgr_config_t *config);

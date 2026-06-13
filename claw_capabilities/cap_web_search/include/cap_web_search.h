/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "claw_compat.h"

typedef struct {
    const char *api_key;    /* Tavily API key, leave empty to disable */
    int max_results;        /* default 3 */
} cap_web_search_config_t;

int cap_web_search_init(const cap_web_search_config_t *config);

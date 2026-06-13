/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "claw_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *config_dir;   /* e.g. "/mcp" — loads /mcp/servers.json */
} cap_mcp_client_config_t;

int cap_mcp_client_init(const cap_mcp_client_config_t *config);

#ifdef __cplusplus
}
#endif

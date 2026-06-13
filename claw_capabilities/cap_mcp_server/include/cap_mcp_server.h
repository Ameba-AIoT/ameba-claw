/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "claw_compat.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAP_MCP_SERVER_DEFAULT_ENDPOINT  "/mcp"

typedef struct {
    const char *endpoint;    /* HTTP path, default "/mcp" */
    const char *server_name; /* server name reported in initialize, default "ameba-claw" */
} cap_mcp_server_config_t;

#define CAP_MCP_SERVER_DEFAULT_CONFIG() \
    { .endpoint = CAP_MCP_SERVER_DEFAULT_ENDPOINT, .server_name = "ameba-claw" }

int cap_mcp_server_init(const cap_mcp_server_config_t *cfg);

#ifdef __cplusplus
}
#endif

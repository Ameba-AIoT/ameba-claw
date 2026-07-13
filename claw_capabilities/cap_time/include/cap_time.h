/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "claw_compat.h"
#include "claw_agent.h"

typedef struct {
    const char *ntp_server;  /* NTP server hostname, default: "pool.ntp.org" */
    int         timezone_hrs; /* UTC offset in hours, e.g. 8 for UTC+8 */
} cap_time_config_t;

extern claw_agent_context_provider_t cap_time_context_provider;

int cap_time_init(const cap_time_config_t *cfg);

/* (Re)start SNTP time sync.  Idempotent — safe to call on every WiFi connect.
 * cap_time_init() no longer kicks SNTP itself (the network is down that early),
 * so this must be wired to claw_wifi_mgr's on-connected callback. */
void cap_time_kick_sntp(void);

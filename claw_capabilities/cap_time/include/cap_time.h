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

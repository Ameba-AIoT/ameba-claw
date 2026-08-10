/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>
#include <time.h>
#include "claw_compat.h"
#include "claw_agent.h"

typedef struct {
    const char *ntp_server;  /* NTP server hostname, default: "pool.ntp.org" */
    int         timezone_hrs; /* legacy/unused; timezone now comes from claw_config */
} cap_time_config_t;

extern claw_agent_context_provider_t cap_time_context_provider;

int cap_time_init(const cap_time_config_t *cfg);

/* ---- Unified time API — the single source of truth for the whole project ----
 * The system clock is UTC (SNTP). Local time is derived by MANUALLY adding the
 * configured offset (do NOT use localtime_r here: no TZ env is set, so it would
 * return UTC). Every consumer that needs local wall-clock time (get_local_time,
 * the agent time-context provider, cap_scheduler cron matching / listings) MUST
 * go through these so the offset is applied consistently in one place. */

/* True once the wall clock has been SNTP-synced (>= CLAW_TIME_MIN_VALID_UNIX). */
bool cap_time_is_synced(void);

/* True if the user has configured a timezone. When *out_offset_sec is non-NULL
 * it always receives the current offset in seconds east of UTC (valid to use
 * only when this returns true). */
bool cap_time_get_tz_offset_sec(long *out_offset_sec);

/* Fill *out with LOCAL broken-down time. Returns true only when the clock is
 * synced AND a timezone is configured; false otherwise (caller must not use
 * *out and should prompt the user to sync/set timezone). */
bool cap_time_local_now(struct tm *out);

/* (Re)start SNTP time sync.  Idempotent — safe to call on every WiFi connect.
 * cap_time_init() no longer kicks SNTP itself (the network is down that early),
 * so this must be wired to claw_wifi_mgr's on-connected callback. */
void cap_time_kick_sntp(void);

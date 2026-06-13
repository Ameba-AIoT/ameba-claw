/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "claw_compat.h"

typedef struct {
    const char *schedule_root_dir;  /* e.g. "/scheduler" */
    size_t max_jobs;                /* max 16 */
} cap_scheduler_config_t;

int cap_scheduler_init(const cap_scheduler_config_t *config);
int cap_scheduler_start(void);
int cap_scheduler_stop(void);

/* Fire all enabled jobs whose event_type matches the given string.
 * Called by system components (e.g. wifi_mgr) when a lifecycle event occurs.
 * Thread-safe; no-op if the scheduler has not been started. */
void cap_scheduler_fire_event(const char *event_type);

/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "claw_compat.h"

typedef struct {
    size_t max_read_size;   /* max bytes returned per read, default 4096 */
} cap_files_config_t;

int cap_files_init(const cap_files_config_t *cfg);

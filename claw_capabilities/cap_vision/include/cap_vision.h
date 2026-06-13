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
    /* Vision model, e.g. "glm-5v-turbo". NULL → claw_config vision.model. */
    const char *model;
    /* API key. NULL or empty → fall back to claw_config vision.api_key,
     * then claw_config llm.api_key. */
    const char *api_key;
    /* API host, e.g. "open.bigmodel.cn". NULL → claw_config vision.base_url. */
    const char *base_url;
    /* API path, e.g. "/api/paas/v4/chat/completions". NULL → claw_config vision.api_path. */
    const char *api_path;
    /* Max image size allowed (bytes); 0 → default 2 MB. */
    size_t max_image_bytes;
} cap_vision_config_t;

/**
 * Initialise the vision capability and register the "vision_describe" LLM tool.
 * Call once during startup, after claw_cap_register_group is ready.
 */
int cap_vision_init(const cap_vision_config_t *cfg);

#ifdef __cplusplus
}
#endif

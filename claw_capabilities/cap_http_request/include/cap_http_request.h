/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize and register the http_request capability.
 * Exposes an "http_request" tool to the LLM that can send HTTPS requests
 * with optional URL allowlist security filtering.
 */
int cap_http_request_init(void);

#ifdef __cplusplus
}
#endif

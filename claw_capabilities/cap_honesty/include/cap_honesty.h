/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "claw_agent.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Completion observer: warns when the model claims to have invoked a tool
 * but no matching tool call appears in the round's tool_calls_csv.
 * Register via claw_agent_add_completion_observer(). */
void cap_honesty_observe_completion(const claw_agent_completion_summary_t *summary,
                                    void *user_ctx);

#ifdef __cplusplus
}
#endif

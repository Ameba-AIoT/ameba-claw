/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "claw_agent.h"

/* Must be called once before registering the observer. Creates the async task
 * and queue so that LLM extraction does not run in engine_task context. */
void claw_memory_extract_init(void);

/* Completion observer: enqueues the turn for async extraction.
 * Register with claw_agent_add_completion_observer(). */
void claw_memory_extract_observer(const claw_agent_completion_summary_t *summary,
                                  void *user_ctx);

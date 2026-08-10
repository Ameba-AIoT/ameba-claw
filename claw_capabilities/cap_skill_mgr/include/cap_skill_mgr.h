/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "claw_compat.h"
#include "claw_agent.h"
#include "claw_config.h"

typedef struct {
    const char *skills_dir;   /* e.g. "/skills" */
} cap_skill_mgr_config_t;

extern claw_agent_context_provider_t cap_skill_mgr_context_provider;
extern claw_agent_context_provider_t cap_skill_catalog_provider;

int cap_skill_mgr_init(const cap_skill_mgr_config_t *config);

/* Install the global LLM-visible cap-group base set (improvement #12 Inc 6).
 *
 * Must be called AFTER all capability groups have registered (i.e. after
 * claw_cap_start_all()). It sets the global visible list to every registered
 * group EXCEPT the gateable peripheral groups, so those start hidden and are
 * surfaced per-session only when a skill that declares them is activated. */
void cap_skill_mgr_apply_base_visibility(void);

/* Return true if gid appears in vis->hidden[].
 * Shared with cap_webui to avoid duplicating the linear scan. */
bool cap_skill_mgr_group_is_hidden(const char *gid, const claw_cap_visibility_config_t *vis);

/* Deactivate every skill active in one session (clears its active-skills file
 * and resets its LLM-visible cap groups to the global base). Skill bodies are
 * NOT removed. session_id NULL/empty → the default session. Called by
 * session,clear. */
void cap_skill_mgr_deactivate_all(const char *session_id);

/* Deactivate active skills across ALL sessions (removes every sk_*.skills.json
 * and resets all per-session cap-group visibility). Skill bodies are NOT
 * removed. Called by session,clear,all for clean test isolation. */
void cap_skill_mgr_deactivate_all_sessions(void);

/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_cap_registry.h"
#include "ameba_claw_defs.h"

#include <string.h>

/* ── Fixed static storage ────────────────────────────────────────────────────
 * Populated by C constructors (CLAW_CAP_REGISTER), which run before the heap,
 * RTOS and UART exist. No malloc, no lock: constructors run single-threaded
 * during __libc_init_array(), and every consumer (claw_cap_registry_run and the
 * fail-fast in app_example) runs later, after all constructors have completed.
 */
static const claw_cap_desc_t *s_descs[CLAW_CAP_REGISTRY_MAX];
static size_t s_count;
static bool   s_overflowed;
static bool   s_dup_group;

void claw_cap_registry_add(const claw_cap_desc_t *desc)
{
    if (!desc) {
        return;
    }

    /* Bounds check first: a silently-vanished cap is the hardest failure to
     * diagnose, so record the overflow for the fail-fast assert. */
    if (s_count >= CLAW_CAP_REGISTRY_MAX) {
        s_overflowed = true;
        return;
    }

    /* Duplicate group id => two caps fighting over the same group. Flag it
     * (asserted at fail-fast). Skip NULL groups (none expected, but be safe). */
    if (desc->group) {
        for (size_t i = 0; i < s_count; i++) {
            if (s_descs[i]->group &&
                strcmp(s_descs[i]->group, desc->group) == 0) {
                s_dup_group = true;
                break;
            }
        }
    }

    s_descs[s_count++] = desc;
}

/* Select the hook pointer for a phase. */
static void (*phase_hook(const claw_cap_desc_t *d, claw_cap_phase_t phase))(const claw_config_t *)
{
    switch (phase) {
    case CLAW_CAP_PHASE_INIT:  return d->on_init;
    case CLAW_CAP_PHASE_AGENT: return d->on_agent;
    case CLAW_CAP_PHASE_IO:    return d->on_io;
    default:                   return NULL;
    }
}

/* Runtime lifecycle filter: CORE caps always run; others are skipped if their
 * group appears in cfg->cap_runtime.disabled[]. Changes take effect on next boot. */
static bool cap_active(const claw_cap_desc_t *d, const claw_config_t *cfg)
{
    if (d->flags & CLAW_CAP_FLAG_CORE) {
        return true;
    }
    const claw_cap_runtime_config_t *rt = &cfg->cap_runtime;
    for (uint8_t i = 0; i < rt->disabled_count; i++) {
        if (d->group && strcmp(rt->disabled[i], d->group) == 0) {
            return false;
        }
    }
    return true;
}

void claw_cap_registry_run(claw_cap_phase_t phase, const claw_config_t *cfg)
{
    /* Iterate by ascending `order`, stable. We never rely on registration
     * (link) order for semantics; a stable sort by `order` gives a fully
     * deterministic sequence that is identical on every boot of the same image.
     * N is tiny (~25) so an insertion sort over an index array is ideal. */
    uint8_t idx[CLAW_CAP_REGISTRY_MAX];
    size_t n = s_count;
    for (size_t i = 0; i < n; i++) {
        idx[i] = (uint8_t)i;
    }
    for (size_t i = 1; i < n; i++) {
        uint8_t cur = idx[i];
        int16_t key = s_descs[cur]->order;
        size_t j = i;
        while (j > 0 && s_descs[idx[j - 1]]->order > key) {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = cur;
    }

    for (size_t i = 0; i < n; i++) {
        const claw_cap_desc_t *d = s_descs[idx[i]];
        if (!cap_active(d, cfg)) {
            continue;
        }
        void (*hook)(const claw_config_t *) = phase_hook(d, phase);
        if (hook) {
            hook(cfg);
        }
    }
}

size_t claw_cap_registry_count(void)          { return s_count; }
bool   claw_cap_registry_overflowed(void)     { return s_overflowed; }
bool   claw_cap_registry_has_dup_group(void)  { return s_dup_group; }

bool claw_cap_registry_group_is_core(const char *group_id)
{
    if (!group_id) return false;
    for (size_t i = 0; i < s_count; i++) {
        if (s_descs[i]->group &&
            strcmp(s_descs[i]->group, group_id) == 0 &&
            (s_descs[i]->flags & CLAW_CAP_FLAG_CORE)) {
            return true;
        }
    }
    return false;
}

size_t claw_cap_registry_list_groups(const char **out, size_t max_count)
{
    size_t n = 0;
    for (size_t i = 0; i < s_count && n < max_count; i++) {
        if (!s_descs[i]->group) continue;
        /* Deduplicate: skip if already added (should not happen, but be safe). */
        bool dup = false;
        for (size_t k = 0; k < n; k++) {
            if (strcmp(out[k], s_descs[i]->group) == 0) { dup = true; break; }
        }
        if (!dup) out[n++] = s_descs[i]->group;
    }
    return n;
}

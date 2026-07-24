/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/*
 * claw_cap_registry — capability lifecycle self-registration.
 *
 * Distinct from claw_cap (the LLM *tool* registry): this module owns the
 * *boot lifecycle* of capability plugins. Each cap declares one descriptor in
 * its own .c and self-registers via CLAW_CAP_REGISTER() using a C constructor,
 * so no central table lists every cap. ameba_claw_main() drives the three
 * phases in order; the registry only iterates descriptors by their `order`
 * field (registration/link order is never relied upon).
 *
 * Phases (see the audit in design_spec/arch/kconfig_cap_trimming_plan.md §3.1):
 *   INIT  — register the cap group; before claw_cap_start_all().
 *   AGENT — add context providers / completion observers / set visibility;
 *           after claw_agent_init(), before claw_agent_start().
 *   IO    — channels / background services / wifi hooks / HTTP routes;
 *           after dispatcher + http_init, before http_server_start().
 *
 * Constructors run before the RTOS, heap and UART exist (see
 * ram_km4tz/ameba_app_start.c → __libc_init_array()). Therefore a constructor
 * MUST only register — it must not malloc, touch the RTOS, or read config.
 * Config is delivered to each phase hook via its (const claw_config_t *) arg.
 *
 * ── `order` allocation (single coordination point) ───────────────────────────
 * The hook execution order within a phase is defined ONLY by `order` (ascending,
 * stable). Order values are chosen so the three-phase hook sequence reproduces
 * the historical ameba_claw_main() call order. Order-SENSITIVE caps (notably
 * those adding a context provider in on_agent, whose relative order shapes the
 * system prompt) MUST have distinct `order` values. Caps whose relative order is
 * irrelevant may share a value. Keep the assignments below authoritative; do not
 * scatter conflicting values across cap files.
 *
 *   order   cap                phases (hooks)
 *   -----   ----------------   ----------------------------------------------
 *   10      cap_time           INIT, AGENT (time provider), IO (SNTP wifi hook)
 *   20      cap_skill_mgr      INIT, AGENT (base visibility + 2 providers)
 *   30      cap_board_mgr      INIT, AGENT (board provider)
 *   35      cap_honesty        AGENT (completion observer)
 *   40      cap_web_search     INIT
 *   50      cap_files          INIT
 *   55      cap_system         INIT
 *   60      cap_net_discover   INIT
 *   65      cap_audio_stream   INIT
 *   70      cap_http_request   INIT
 *   75      cap_vision         INIT
 *   80      cap_lua            INIT (CORE — always active)
 *   82      cap_im_attachment  INIT, IO (start)
 *   85      cap_scheduler      INIT, IO (start + wifi hook)
 *   100     cap_mcp_client     IO (wifi hook → discovery task)
 *   110     cap_webui          IO (HTTP routes) (CORE — always active)
 *   120     cap_im_local       IO (HTTP routes + channel)
 *   130     cap_im_telegram    IO (channel)
 *   140     cap_im_feishu      IO (channel)
 *   150     cap_im_qq          IO (channel)
 *   160     cap_im_wechat      IO (channel)
 *   170     cap_mcp_server     IO (HTTP route)
 *   200     cap_router_mgr     IO (loads rules; high order keeps its group
 *                                  registered after the base-visibility snapshot)
 *
 * Only order-SENSITIVE relationships must hold: AGENT providers 10<20<30 (system
 * prompt order); IO route registrars (110/120/170) all precede http_server_start
 * (driven by main, after registry_run(IO)); router_mgr (200) after everything so
 * its late group registration stays hidden from the LLM. INIT-only orders are
 * behaviourally irrelevant (each just registers a group).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "claw_compat.h"
#include "claw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLAW_CAP_PHASE_INIT = 0,  /* register cap group; before claw_cap_start_all() */
    CLAW_CAP_PHASE_AGENT,     /* providers / observers / visibility; between agent_init and agent_start */
    CLAW_CAP_PHASE_IO,        /* channels / services / wifi hooks / HTTP routes; between http_init and http_start */
} claw_cap_phase_t;

/* Ignore the runtime enable-list — this cap is always active (e.g. cap_lua). */
#define CLAW_CAP_FLAG_CORE      (1u << 0)

typedef struct {
    const char *group;                        /* cap group id; used for LLM visibility gating via cap_visibility.hidden[] */
    uint16_t    flags;                        /* CLAW_CAP_FLAG_* */
    int16_t     order;                        /* intra-phase order, ascending; default 0 */
    void (*on_init) (const claw_config_t *);  /* all hooks optional; a cap implements only the phases it needs */
    void (*on_agent)(const claw_config_t *);
    void (*on_io)   (const claw_config_t *);
} claw_cap_desc_t;

/*
 * Declare + self-register a capability descriptor. Place once in a cap's .c:
 *
 *   CLAW_CAP_REGISTER(time, {
 *       .group = "time", .order = 10,
 *       .on_init = time_on_init, .on_agent = time_on_agent, .on_io = time_on_io,
 *   });
 *
 * The descriptor is a static const (rodata); the constructor merely records its
 * address into the registry's static array.
 */
#define CLAW_CAP_REGISTER(ident, ...)                                        \
    static const claw_cap_desc_t claw_cap_desc_##ident = __VA_ARGS__;        \
    __attribute__((constructor)) static void claw_cap_ctor_##ident(void)     \
    {                                                                        \
        claw_cap_registry_add(&claw_cap_desc_##ident);                       \
    }

/* Record a descriptor. Called from constructors — bounded static array, no
 * heap, no lock. Silently drops on overflow (sets the overflow flag) or on a
 * duplicate group id (sets the dup flag); both are asserted at fail-fast. */
void claw_cap_registry_add(const claw_cap_desc_t *desc);

/* Run every registered cap's hook for `phase`, in ascending `order` (stable).
 * A hook runs when (flags & CLAW_CAP_FLAG_CORE) or the group is enabled by the
 * runtime allow-list. Boot-time lifecycle filtering is not implemented; all caps
 * currently run. LLM-tool visibility is a separate layer (cap_visibility.hidden[]). */
void claw_cap_registry_run(claw_cap_phase_t phase, const claw_config_t *cfg);

/* Number of descriptors actually recorded (<= CLAW_CAP_REGISTRY_MAX). */
size_t claw_cap_registry_count(void);

/* True if any registration was dropped because the static array was full. */
bool claw_cap_registry_overflowed(void);

/* True if two descriptors declared the same non-NULL group id. */
bool claw_cap_registry_has_dup_group(void);

/* True if the descriptor for group_id carries CLAW_CAP_FLAG_CORE. */
bool claw_cap_registry_group_is_core(const char *group_id);

/* Copy up to max_count group IDs (non-NULL .group fields) into out[].
 * Returns the actual number written. Includes runtime-disabled groups. */
size_t claw_cap_registry_list_groups(const char **out, size_t max_count);

#ifdef __cplusplus
}
#endif

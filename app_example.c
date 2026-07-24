/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ameba_claw_main.h"
#include "ameba_claw_defs.h"
#include "ameba_soc.h"
#include "claw_cap_registry.h"
#include <assert.h>

#if CLAW_AGENT_AUTO_TEST
extern void claw_gpio_ctrl_startup_pullup(void);
#endif

/* Fail-fast validation of the capability self-registration mechanism.
 * Constructors run before this (during __libc_init_array), so by now every
 * CLAW_CAP_REGISTER() descriptor should be recorded. A zero count, an overflow,
 * or a duplicate group id all indicate a broken build/link — surface it loudly
 * here rather than letting caps silently vanish at runtime. */
static void verify_cap_registry(void)
{
    size_t n = claw_cap_registry_count();
    RTK_LOGI("claw", "cap_registry: %u caps registered (max=%d)\n",
             (unsigned)n, CLAW_CAP_REGISTRY_MAX);

    if (n == 0 || claw_cap_registry_overflowed() ||
        claw_cap_registry_has_dup_group()) {
        RTK_LOGE("claw", "cap_registry INVALID (count=%u overflow=%d dup=%d)\n",
                 (unsigned)n, (int)claw_cap_registry_overflowed(),
                 (int)claw_cap_registry_has_dup_group());
    }
    assert(n > 0);
    assert(!claw_cap_registry_overflowed());
    assert(!claw_cap_registry_has_dup_group());
}

void app_example(void)
{
    rtk_log_level_set("HTTPC", RTK_LOG_NONE);
#if CLAW_AGENT_AUTO_TEST
    claw_gpio_ctrl_startup_pullup();
#endif
    verify_cap_registry();
    ameba_claw_main();
}

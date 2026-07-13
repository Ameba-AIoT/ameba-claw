/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ameba_claw_main.h"
#include "ameba_claw_defs.h"

#if CLAW_AGENT_AUTO_TEST
extern void claw_gpio_ctrl_startup_pullup(void);
#endif

void app_example(void)
{
#if CLAW_AGENT_AUTO_TEST
    claw_gpio_ctrl_startup_pullup();
#endif
    ameba_claw_main();
}

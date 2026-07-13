/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "claw_event.h"

/**
 * Attempt to handle ev->text as a session slash command
 * (/new /list /resume /rename /delete /clear).
 *
 * If ev->text matches one of the commands, executes it, sends a reply via
 * claw_im_dispatch_send, and returns 1 (event consumed — caller must not
 * submit to the agent).  Returns 0 if the text is not a session command.
 */
int session_cmd_try_handle(const claw_event_t *ev);

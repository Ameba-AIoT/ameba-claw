/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "claw_agent.h"

typedef struct {
    const char *vfs_path;   /* e.g. "vfs:/board.json"; NULL → use default */
} cap_board_mgr_config_t;

int cap_board_mgr_init(const cap_board_mgr_config_t *config);

/* Returns 1 if the named peripheral is listed in the current chip constraints,
 * 0 if not supported, -1 if no board is loaded (treat as "allow all"). */
int cap_board_mgr_chip_has_peripheral(const char *peripheral_name);

/*
 * Runtime board switching (AT-only; see AT+CLAW=board).
 *
 * cap_board_mgr_list_boards: fills out_names[] with pointers to the embedded
 *   board registry names (board directory names). Returns the total count of
 *   embedded boards regardless of max_names; pass out_names=NULL to just count.
 *   Returned pointers are static — do not free.
 *
 * cap_board_mgr_active: registry name of the board last selected via
 *   cap_board_mgr_switch(). Empty string when loaded straight from VFS at boot
 *   (registry origin unknown). Never NULL.
 *
 * cap_board_mgr_switch: overwrite vfs:/board.json with the embedded JSON of
 *   board_name (a registry/directory name) and re-parse the model. The choice
 *   persists across reboot because the VFS file is overwritten. Returns
 *   RTK_SUCCESS, or RTK_FAIL if the name is unknown / write / reload failed.
 */
int         cap_board_mgr_list_boards(const char **out_names, int max_names);
const char *cap_board_mgr_active(void);
int         cap_board_mgr_switch(const char *board_name);

extern const claw_agent_context_provider_t cap_board_mgr_context_provider;

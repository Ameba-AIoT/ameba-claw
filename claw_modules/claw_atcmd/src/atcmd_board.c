/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AT+CLAW=board — list embedded boards / switch the active board at runtime.
 *
 *   AT+CLAW=board            List all embedded boards, mark the active one
 *   AT+CLAW=board,<name>     Switch to <name> (a board directory name), persist
 *
 * Switching overwrites vfs:/board.json with the selected board's embedded JSON
 * and re-parses the model; the choice survives reboot. Board identifiers are
 * the board *directory* names (see cap_board_mgr/boards/<name>/board.json), not
 * the board.name field — two boards may share the same board.name.
 */

#include "ameba_soc.h"
#include "atcmd_service.h"
#include "cap_board_mgr.h"
#include "atcmd_handlers.h"
#include <string.h>

#define ATCMD_BOARD_MAX 16

void handle_cmd_board(const char *arg2)
{
    if (arg2[0] == '\0') {
        /* AT+CLAW=board  →  list embedded boards, mark the active one */
        const char *names[ATCMD_BOARD_MAX];
        int total  = cap_board_mgr_list_boards(names, ATCMD_BOARD_MAX);
        int shown  = (total < ATCMD_BOARD_MAX) ? total : ATCMD_BOARD_MAX;
        const char *active = cap_board_mgr_active();

        at_printf("\r\n+CLAW:board,current=%s\r\n",
                  (active && active[0]) ? active : "(from vfs)");
        at_printf("+CLAW:board,count=%d\r\n", total);
        for (int i = 0; i < shown; i++) {
            bool is_active = (active && active[0]
                              && strcmp(active, names[i]) == 0);
            at_printf("+CLAW:board,[%d]=%s%s\r\n",
                      i, names[i], is_active ? " *" : "");
        }
        at_printf(ATCMD_OK_END_STR);
        return;
    }

    /* AT+CLAW=board,<name>  →  switch */
    int rc = cap_board_mgr_switch(arg2);
    if (rc == RTK_SUCCESS) {
        at_printf("\r\n+CLAW:board,switched=%s\r\n", cap_board_mgr_active());
        at_printf(ATCMD_OK_END_STR);
    } else {
        at_printf("\r\n+CLAW:board,error=switch failed for '%s' "
                  "(unknown name? try AT+CLAW=board to list)\r\n", arg2);
        at_printf(ATCMD_ERROR_END_STR, rc ? rc : -1);
    }
}

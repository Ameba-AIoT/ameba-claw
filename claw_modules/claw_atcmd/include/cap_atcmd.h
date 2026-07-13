/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CAP_ATCMD_H
#define CAP_ATCMD_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the "serial" outbound reply channel with claw_im_dispatch.
 *
 * The AT+CLAW command table itself is registered via the linker data section
 * (ATCMD_TABLE_DATA_SECTION) and needs no runtime call; this only wires the
 * serial console as an outbound channel so agent replies for serial sessions
 * route back out as "+CLAW:<text>". Treat it like any other IM channel
 * registration and call it from the composition root.
 */
void at_claw_init(void);

/**
 * Echo text to the serial console unconditionally.
 * Used to mirror LLM narration and responses from non-serial channels so the
 * UART always shows the full conversation regardless of which channel is active.
 * Format: "\r\n+CLAW:<text>\r\n". No-op if text is NULL or empty.
 */
void at_claw_serial_echo(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* CAP_ATCMD_H */

/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef CLAW_WS_ROUTER_H
#define CLAW_WS_ROUTER_H

/*
 * Inbound WebSocket dispatch router.
 *
 * The SDK wsclient library (component/network/websocket) exposes only ONE set
 * of global receive callbacks (ws_dispatch / ws_dispatch_close / ws_pong). If
 * two WS-based IMs each call ws_dispatch() directly, the second overwrites the
 * first's callback, so whichever connected last silently steals every incoming
 * frame — the other IM stops responding even though its socket is still alive.
 *
 * claw_ws_router owns those global callbacks (installed lazily on first
 * register) and fans each frame out to the owning connection by matching the
 * wsclient_context pointer that the SDK passes to the callback. This keeps the
 * fix entirely in the harness — the SDK library is not modified.
 *
 * Contract for WS-based IMs:
 *   - Do NOT call ws_dispatch() / ws_dispatch_close() / ws_pong() directly.
 *   - After create_wsclient() + ws_connect_url() succeed, and BEFORE entering
 *     the ws_poll() loop, call claw_ws_router_register(ctx, ...).
 *   - Before tearing the connection down (ws_close / ws_free), call
 *     claw_ws_router_unregister(ctx) so the stale pointer can't match a future
 *     connection that reuses the same address.
 *
 * HTTP-polling IMs (telegram, wechat) do not touch this library and need no
 * changes.
 */

#include "websocket/wsclient_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Frame received on a connection (opcode is TEXT_FRAME / BINARY_FRAME / …). */
typedef void (*claw_ws_on_data_fn)(wsclient_context **ctx, int len,
                                   enum opcode_type opcode);
/* Connection closed by the SDK. May be NULL if the IM doesn't need it. */
typedef void (*claw_ws_on_close_fn)(wsclient_context *ctx);
/* PONG received. May be NULL if the IM doesn't need it. */
typedef void (*claw_ws_on_pong_fn)(wsclient_context **ctx);

/*
 * Register a live connection's callbacks. Call once per connection, after the
 * socket is connected and before the first ws_poll(). on_close / on_pong may be
 * NULL. Returns 0 on success, -1 on bad args or a full table.
 *
 * Re-registering the same ctx pointer updates its callbacks in place.
 */
int  claw_ws_router_register(wsclient_context *ctx,
                             claw_ws_on_data_fn  on_data,
                             claw_ws_on_close_fn on_close,
                             claw_ws_on_pong_fn  on_pong);

/* Remove a connection from the routing table. Safe to call with an unknown or
 * NULL ctx (no-op). */
void claw_ws_router_unregister(wsclient_context *ctx);

#ifdef __cplusplus
}
#endif

#endif /* CLAW_WS_ROUTER_H */

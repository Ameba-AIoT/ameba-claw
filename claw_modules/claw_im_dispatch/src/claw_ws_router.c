/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_ws_router.h"
#include "ameba_claw_defs.h"
#include "ameba_soc.h"
#include "os_wrapper.h"
#include <string.h>
#include <stdbool.h>

#define TAG "claw_ws_router"

typedef struct {
    wsclient_context   *ctx;       /* NULL slot == free */
    claw_ws_on_data_fn  on_data;
    claw_ws_on_close_fn on_close;
    claw_ws_on_pong_fn  on_pong;
} ws_conn_t;

static ws_conn_t     s_conns[CLAW_WS_ROUTER_MAX_CONNS];
static rtos_mutex_t  s_lock;
static bool          s_installed;   /* global SDK callbacks installed yet? */

/* ---- Global callback trampolines (installed into the SDK once) ------------
 * These run on the owning IM's task from inside ws_poll(). We copy the matched
 * handler out under the lock, then invoke it OUTSIDE the lock so a handler that
 * blocks (e.g. runs the agent) can never stall another IM's register call. */

static void router_on_data(wsclient_context **ctx, int len, enum opcode_type op)
{
    if (!ctx || !*ctx) return;
    claw_ws_on_data_fn fn = NULL;
    rtos_mutex_take(s_lock, 0xFFFFFFFFUL);
    for (int i = 0; i < CLAW_WS_ROUTER_MAX_CONNS; i++) {
        if (s_conns[i].ctx == *ctx) { fn = s_conns[i].on_data; break; }
    }
    rtos_mutex_give(s_lock);
    if (fn) fn(ctx, len, op);
    /* Unknown ctx → frame dropped (connection not registered / already gone). */
}

static void router_on_close(wsclient_context *ctx)
{
    if (!ctx) return;
    claw_ws_on_close_fn fn = NULL;
    rtos_mutex_take(s_lock, 0xFFFFFFFFUL);
    for (int i = 0; i < CLAW_WS_ROUTER_MAX_CONNS; i++) {
        if (s_conns[i].ctx == ctx) { fn = s_conns[i].on_close; break; }
    }
    rtos_mutex_give(s_lock);
    if (fn) fn(ctx);
}

static void router_on_pong(wsclient_context **ctx)
{
    if (!ctx || !*ctx) return;
    claw_ws_on_pong_fn fn = NULL;
    rtos_mutex_take(s_lock, 0xFFFFFFFFUL);
    for (int i = 0; i < CLAW_WS_ROUTER_MAX_CONNS; i++) {
        if (s_conns[i].ctx == *ctx) { fn = s_conns[i].on_pong; break; }
    }
    rtos_mutex_give(s_lock);
    if (fn) fn(ctx);
}

/* ---- Lazy one-time setup -------------------------------------------------- */

/* Create the lock on first use. IM tasks are created after boot and the first
 * register() call happens on one of those tasks, so there is no earlier
 * concurrent caller to race with this. */
static void router_ensure_lock(void)
{
    if (!s_lock) {
        rtos_mutex_create(&s_lock);
    }
}

/* ---- Public API ----------------------------------------------------------- */

int claw_ws_router_register(wsclient_context *ctx,
                            claw_ws_on_data_fn  on_data,
                            claw_ws_on_close_fn on_close,
                            claw_ws_on_pong_fn  on_pong)
{
    if (!ctx || !on_data) return -1;
    router_ensure_lock();

    rtos_mutex_take(s_lock, 0xFFFFFFFFUL);

    if (!s_installed) {
        /* Take ownership of the SDK's single global callback set. From now on
         * every WS connection's frames arrive here and get routed by ctx. */
        ws_dispatch(router_on_data);
        ws_dispatch_close(router_on_close);
        ws_pong(router_on_pong);
        s_installed = true;
    }

    int free_slot = -1;
    for (int i = 0; i < CLAW_WS_ROUTER_MAX_CONNS; i++) {
        if (s_conns[i].ctx == ctx) { free_slot = i; break; }   /* update in place */
        if (s_conns[i].ctx == NULL && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) {
        rtos_mutex_give(s_lock);
        RTK_LOGE(TAG, "conn table full (%d), ctx %p not registered\n",
                 CLAW_WS_ROUTER_MAX_CONNS, ctx);
        return -1;
    }

    s_conns[free_slot].ctx      = ctx;
    s_conns[free_slot].on_data  = on_data;
    s_conns[free_slot].on_close = on_close;
    s_conns[free_slot].on_pong  = on_pong;
    rtos_mutex_give(s_lock);

    RTK_LOGI(TAG, "registered ctx %p (slot %d)\n", ctx, free_slot);
    return 0;
}

void claw_ws_router_unregister(wsclient_context *ctx)
{
    if (!ctx || !s_lock) return;
    rtos_mutex_take(s_lock, 0xFFFFFFFFUL);
    for (int i = 0; i < CLAW_WS_ROUTER_MAX_CONNS; i++) {
        if (s_conns[i].ctx == ctx) {
            memset(&s_conns[i], 0, sizeof(s_conns[i]));
            rtos_mutex_give(s_lock);
            RTK_LOGI(TAG, "unregistered ctx %p (slot %d)\n", ctx, i);
            return;
        }
    }
    rtos_mutex_give(s_lock);
}

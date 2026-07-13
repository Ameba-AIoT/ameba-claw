/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_driver_ir.c — Lua IR (infrared) driver for Ameba RTOS.
**
** Provides require("ir") with an object-oriented API:
**   ir.new(tx_pin, rx_pin [, opts])           → dev
**   dev:send_raw(symbols [, "poll"|"intr"])   — raw TX: {{level=0|1, duration_us=N}, ...}
**   dev:receive([timeout_ms])                 — raw RX, returns {{level, duration_us}, ...}
**   dev:info()                                → {carrier_hz, tx_pin, rx_pin}
**   dev:close()
**
** Uses raw fwlib IR API (ameba_ir.h) — no HAL/mbed layer.
** TX poll: task busy-polls TX FIFO free space to refill.
** TX intr: ISR refills FIFO on IR_BIT_TX_FIFO_LEVEL_INT; semaphore signals completion
**          on IR_BIT_TX_FIFO_EMPTY_INT.
** RX: interrupt-driven with semaphore signalling.
** NEC protocol encoding/decoding is handled at the Lua test layer (test/).
**
** WARNING: TX and RX are mutually exclusive.
** The IR peripheral has a single hardware block — only one mode (TX or RX)
** can be active at a time.  send_raw() and receive() each reconfigure the
** hardware unconditionally, so calling one while the other is in progress
** will corrupt the ongoing operation.
*/

#define LUA_LIB
#include "lua_driver_ir.h"

#include "ameba_soc.h"
#include "lauxlib.h"
#include "luhw.h"
#include "os_wrapper.h"
#include <string.h>

#define LUA_DRIVER_IR_METATABLE     "ir.dev"
#define IR_CLOCK_HZ                 40000000UL
#define IR_TX_DEFAULT_CARRIER_HZ    38000UL
/* 1 MHz sample clock: 1 count = 1 µs, which simplifies RX duration reporting */
#define IR_RX_SAMPLE_HZ             1000000UL
#define IR_RX_MAX_SYMBOLS           256
/* 5 ms of low-level silence signals end of NEC frame at 1 MHz sample clock */
#define IR_RX_END_SILENCE_CNT       5000U
#define IR_TX_REFILL_THRESHOLD      15
#define IR_IRQ_PRIORITY             6
/* Any RX pulse ≤ this (µs at 1 MHz) is a carrier half-cycle and gets
 * accumulated into the current mark entry rather than stored separately.
 * 200 µs is well above the 38 kHz half-period (~13 µs) and well below
 * the shortest NEC space (560 µs). */
#define IR_RX_CARRIER_MAX_CNT       200U

typedef struct {
    PinName  tx_pin;
    PinName  rx_pin;
    u32      carrier_hz;
    int      closed;
} lua_driver_ir_ud_t;

/* Module-level RX state shared with the ISR (single IR peripheral) */
static struct {
    rtos_sema_t      end_sema;
    volatile u32     buf[IR_RX_MAX_SYMBOLS];
    volatile u32     len;
    volatile int     carrier_accum; /* 1 while accumulating carrier half-cycles */
    int              initialized;
} s_rx;

/* Module-level TX interrupt state shared with the ISR */
static struct {
    rtos_sema_t  done_sema;
    const u32   *buf;
    u32          total;
    volatile u32 sent;
} s_tx;

/* One mutex guards the entire peripheral for its lifetime.  All hardware
 * accesses (new / send_raw / receive / close) hold this lock so no two Lua
 * tasks can ever interleave on the single IR block.  Created once in
 * lua_driver_ir_init() before any concurrent task starts (CONC-01/02).
 * RTOS_MAX_DELAY is safe here: every IR operation is bounded — send_raw() by
 * its own timing, receive() by timeout_ms — so the lock is always released. */
static rtos_mutex_t s_ir_lock;

#define IR_LOCK()   do { if (s_ir_lock) rtos_mutex_take(s_ir_lock, RTOS_MAX_DELAY); } while (0)
#define IR_UNLOCK() do { if (s_ir_lock) rtos_mutex_give(s_ir_lock); } while (0)

/* ---- ISR ------------------------------------------------------------------ */

/* Drain FIFO into s_rx.buf with carrier accumulation.
 * Consecutive short pulses (≤ IR_RX_CARRIER_MAX_CNT) from the 38 kHz carrier
 * are summed into the current entry so the output matches standard NEC timing
 * (one mark entry per carrier burst, one space entry per silence interval). */
static void ir_rx_drain_fifo(void)
{
    u32 n = IR_GetRxDataLen(IR_DEV);
    while (n-- && s_rx.len < IR_RX_MAX_SYMBOLS) {
        u32 raw = IR_ReceiveData(IR_DEV);
        u32 dur = raw & (u32)IR_MASK_RX_CNT;
        if (dur > IR_RX_CARRIER_MAX_CNT) {
            s_rx.carrier_accum = 0;
            s_rx.buf[s_rx.len++] = raw;
        } else if (!s_rx.carrier_accum) {
            s_rx.carrier_accum = 1;
            s_rx.buf[s_rx.len++] = raw;
        } else {
            s_rx.buf[s_rx.len - 1] += dur;
        }
    }
}

static void ir_irq_handler(void)
{
    u32 status = IR_GetINTStatus(IR_DEV);
    u32 imr    = IR_GetIMR(IR_DEV);

    /* IR_BIT_MODE_SEL=0 → TX mode; =1 → RX mode.
     * IR_GetINTStatus() is mode-aware: returns IR_TX_SR in TX mode,
     * IR_RX_SR in RX mode.  TX and RX status bits share the same bit
     * positions, so we must dispatch on mode before checking bits. */
    if (!(IR_DEV->IR_TX_CONFIG & IR_BIT_MODE_SEL)) {
        /* ---- TX mode ---- */
        if (status & IR_BIT_TX_FIFO_LEVEL_INT_STATUS) {
            IR_ClearINTPendingBit(IR_DEV, IR_BIT_TX_FIFO_LEVEL_INT_CLR);
            u32 remaining = s_tx.total - s_tx.sent;
            if (remaining > 0) {
                u32 free    = IR_GetTxFIFOFreeLen(IR_DEV);
                u32 n       = (remaining < free) ? remaining : free;
                u32 is_last = ((s_tx.sent + n) >= s_tx.total) ? TRUE : FALSE;
                IR_SendBuf(IR_DEV, (u32 *)s_tx.buf + s_tx.sent, n, is_last);
                s_tx.sent += n;
                if (is_last) {
                    /* last batch queued — switch to FIFO-empty for completion */
                    IR_INTConfig(IR_DEV, IR_BIT_TX_FIFO_LEVEL_INT_EN, DISABLE);
                    IR_INTConfig(IR_DEV, IR_BIT_TX_FIFO_EMPTY_INT_EN, ENABLE);
                }
            }
        }
        if (status & IR_BIT_TX_FIFO_EMPTY_INT_STATUS) {
            IR_ClearINTPendingBit(IR_DEV, IR_BIT_TX_FIFO_EMPTY_INT_CLR);
            IR_INTConfig(IR_DEV, IR_BIT_TX_FIFO_EMPTY_INT_EN, DISABLE);
            rtos_sema_give(s_tx.done_sema);
        }
        if (status & IR_BIT_TX_FIFO_OVER_INT_STATUS) {
            IR_ClearINTPendingBit(IR_DEV, IR_BIT_TX_FIFO_OVER_INT_CLR);
        }
        return;
    }

    /* ---- RX mode ---- */
    IR_MaskINTConfig(IR_DEV, IR_RX_INT_ALL_MASK, DISABLE);

    if (status & IR_BIT_RX_FIFO_LEVEL_INT_STATUS) {
        IR_ClearINTPendingBit(IR_DEV, IR_BIT_RX_FIFO_LEVEL_INT_CLR);
        ir_rx_drain_fifo();
    }

    if (status & IR_BIT_RX_FIFO_FULL_INT_STATUS) {
        IR_ClearINTPendingBit(IR_DEV, IR_BIT_RX_FIFO_FULL_INT_CLR);
        ir_rx_drain_fifo();
    }

    if (status & IR_BIT_RX_CNT_OF_INT_STATUS) {
        IR_ClearINTPendingBit(IR_DEV, IR_BIT_RX_CNT_OF_INT_CLR);
    }

    if (status & IR_BIT_RX_FIFO_OF_INT_STATUS) {
        IR_ClearINTPendingBit(IR_DEV, IR_BIT_RX_FIFO_OF_INT_CLR);
    }

    if (status & IR_BIT_RX_CNT_THR_INT_STATUS) {
        IR_ClearINTPendingBit(IR_DEV, IR_BIT_RX_CNT_THR_INT_CLR);
        ir_rx_drain_fifo();
        rtos_sema_give(s_rx.end_sema);
    }

    IR_MaskINTConfig(IR_DEV, imr, ENABLE);
}

/* ---- helpers -------------------------------------------------------------- */

static lua_driver_ir_ud_t *ir_get_ud(lua_State *L, int idx)
{
    lua_driver_ir_ud_t *ud = (lua_driver_ir_ud_t *)luaL_checkudata(
        L, idx, LUA_DRIVER_IR_METATABLE);
    if (!ud || ud->closed) {
        luaL_error(L, "ir: invalid or closed device");
    }
    return ud;
}

static void ir_init_tx(u32 carrier_hz)
{
    IR_Cmd(IR_DEV, IR_MODE_RX, DISABLE);
    IR_Cmd(IR_DEV, IR_MODE_TX, DISABLE);
    IR_InitTypeDef init;
    IR_StructInit(&init);
    init.IR_Clock = IR_CLOCK_HZ;
    init.IR_Mode  = IR_MODE_TX;
    init.IR_Freq  = carrier_hz;
    IR_Init(IR_DEV, &init);
}

/* Polling TX: task busy-polls TX FIFO free space to refill in chunks.
 * IR_FSMRunning stays true while TX_START is set, so we use a fixed post-TX
 * delay (100ms, well beyond the longest standard IR frame) instead of polling. */
static void ir_tx_send_poll(u32 *ir_buf, u32 buf_len)
{
    u32 sent       = 0;
    u32 first_n    = (buf_len < (u32)IR_TX_FIFO_SIZE) ? buf_len : (u32)IR_TX_FIFO_SIZE;
    u32 first_last = (first_n == buf_len) ? TRUE : FALSE;

    IR_SendBuf(IR_DEV, ir_buf, first_n, first_last);
    IR_Cmd(IR_DEV, IR_MODE_TX, ENABLE);
    sent = first_n;

    while (sent < buf_len) {
        while (IR_GetTxFIFOFreeLen(IR_DEV) < IR_TX_REFILL_THRESHOLD) {
            rtos_task_yield();
        }
        u32 remaining = buf_len - sent;
        u32 n         = (remaining > (u32)IR_TX_REFILL_THRESHOLD)
                          ? (u32)IR_TX_REFILL_THRESHOLD
                          : remaining;
        u32 is_last   = ((sent + n) >= buf_len) ? TRUE : FALSE;
        IR_SendBuf(IR_DEV, ir_buf + sent, n, is_last);
        sent += n;
    }

    rtos_time_delay_ms(100);
    IR_Cmd(IR_DEV, IR_MODE_TX, DISABLE);
}

/* Interrupt TX: ISR refills FIFO on IR_BIT_TX_FIFO_LEVEL_INT; task blocks on
 * semaphore until IR_BIT_TX_FIFO_EMPTY_INT fires (all data consumed from FIFO).
 * A 2 ms tail delay covers the last symbol still in the hardware shift register. */
static void ir_tx_send_intr(u32 *ir_buf, u32 buf_len)
{
    s_tx.buf   = ir_buf;
    s_tx.total = buf_len;
    s_tx.sent  = 0;

    /* Set FIFO level threshold: interrupt fires when FIFO depth ≤ threshold */
    IR_SetTxThreshold(IR_DEV, (uint8_t)IR_TX_REFILL_THRESHOLD);

    u32 first_n = (buf_len < (u32)IR_TX_FIFO_SIZE) ? buf_len : (u32)IR_TX_FIFO_SIZE;
    u32 is_last = (first_n == buf_len) ? TRUE : FALSE;
    IR_SendBuf(IR_DEV, ir_buf, first_n, is_last);
    s_tx.sent = first_n;

    if (is_last) {
        /* All data fits in initial fill — skip FIFO level, go straight to empty */
        IR_INTConfig(IR_DEV, IR_BIT_TX_FIFO_EMPTY_INT_EN, ENABLE);
    } else {
        IR_INTConfig(IR_DEV, IR_BIT_TX_FIFO_LEVEL_INT_EN, ENABLE);
    }

    IR_Cmd(IR_DEV, IR_MODE_TX, ENABLE);
    rtos_sema_take(s_tx.done_sema, RTOS_MAX_DELAY);

    /* 2 ms tail: last symbol (up to 1690 µs NEC space) may still be transmitting */
    rtos_time_delay_ms(2);
    IR_Cmd(IR_DEV, IR_MODE_TX, DISABLE);
}

/* ---- Lua API -------------------------------------------------------------- */

/* ir.new([tx_pin,] [rx_pin,] [opts]) → dev
 * Either pin may be nil/omitted to configure only one direction.
 * opts table: { carrier_hz = <Hz> }
 */
static int lua_driver_ir_new(lua_State *L)
{
    PinName tx_pin    = NC;
    PinName rx_pin    = NC;
    u32     carrier_hz = IR_TX_DEFAULT_CARRIER_HZ;

    if (!lua_isnoneornil(L, 1)) {
        tx_pin = luhw_check_pin(L, 1);
    }
    if (!lua_isnoneornil(L, 2)) {
        rx_pin = luhw_check_pin(L, 2);
    }

    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        lua_getfield(L, 3, "carrier_hz");
        if (!lua_isnil(L, -1)) {
            carrier_hz = (u32)luaL_checkinteger(L, -1);
            if (carrier_hz < 25000 || carrier_hz > 500000) {
                return luaL_error(L, "ir: carrier_hz must be 25000–500000");
            }
        }
        lua_pop(L, 1);
    }

    /* Semaphores and IRQ are registered in lua_driver_ir_init() during the
     * single-threaded boot phase — no lazy init needed here. */
    RCC_PeriphClockCmd(APBPeriph_IRDA, APBPeriph_IRDA_CLOCK, ENABLE);

    /* Allocate the handle BEFORE taking the lock: lua_newuserdata can longjmp
     * on OOM and must never execute inside the critical section (CONC-02). */
    lua_driver_ir_ud_t *ud = (lua_driver_ir_ud_t *)lua_newuserdata(
        L, sizeof(*ud));
    ud->tx_pin     = tx_pin;
    ud->rx_pin     = rx_pin;
    ud->carrier_hz = carrier_hz;
    ud->closed     = 0;
    luaL_getmetatable(L, LUA_DRIVER_IR_METATABLE);
    lua_setmetatable(L, -2);

    IR_LOCK();
    if (tx_pin != NC) {
        Pinmux_Config((u8)tx_pin, PINMUX_FUNCTION_IR_TX);
    }
    if (rx_pin != NC) {
        Pinmux_Config((u8)rx_pin, PINMUX_FUNCTION_IR_RX);
    }
    ir_init_tx(carrier_hz);
    IR_UNLOCK();

    return 1;
}

/* dev:send_raw(symbols [, mode])
 * symbols = { {level=0|1, duration_us=N}, ... }
 * mode    = "poll" (default) | "intr"
 *   "poll" — task busy-polls TX FIFO free space (no interrupts).
 *   "intr" — ISR refills FIFO; task blocks on semaphore until TX FIFO empty.
 * duration_us is converted to carrier clock cycles internally.
 * WARNING: switches hardware to TX mode — do not call concurrently with receive().
 */
static int lua_driver_ir_send_raw(lua_State *L)
{
    lua_driver_ir_ud_t *ud = ir_get_ud(L, 1);
    if (ud->tx_pin == NC) {
        return luaL_error(L, "ir: send_raw called on an RX-only device (no tx_pin configured)");
    }
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_Integer n = luaL_len(L, 2);

    if (n <= 0 || n > (lua_Integer)IR_RX_MAX_SYMBOLS) {
        return luaL_error(L, "ir: send_raw: symbol count must be 1–%d",
                          IR_RX_MAX_SYMBOLS);
    }

    int use_intr = 0;
    if (!lua_isnoneornil(L, 3)) {
        const char *mode_str = luaL_checkstring(L, 3);
        if (strcmp(mode_str, "intr") == 0) {
            use_intr = 1;
        } else if (strcmp(mode_str, "poll") != 0) {
            return luaL_error(L, "ir: send_raw: mode must be 'poll' or 'intr'");
        }
    }

    u32 *ir_buf = (u32 *)lua_newuserdata(L, sizeof(u32) * (size_t)n);

    for (lua_Integer i = 0; i < n; i++) {
        lua_rawgeti(L, 2, i + 1);
        luaL_checktype(L, -1, LUA_TTABLE);

        lua_getfield(L, -1, "level");
        int level = (int)luaL_checkinteger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, -1, "duration_us");
        lua_Integer dur_us = luaL_checkinteger(L, -1);
        lua_pop(L, 1);

        lua_pop(L, 1);

        /* Convert µs → carrier clock cycles */
        u32 count  = (u32)((u64)dur_us * ud->carrier_hz / 1000000UL);
        ir_buf[i]  = (level ? IR_BIT_TX_DATA_TYPE : 0)
                     | (count & (u32)IR_MASK_TX_DATA_TIME);
    }

    /* All hardware access below must be atomic with respect to other Lua tasks
     * (CONC-01): no interleaving of send_raw / receive is allowed on the
     * single IR peripheral.  Buffer is already built above the lock. */
    IR_LOCK();
    ir_init_tx(ud->carrier_hz);
    if (use_intr) {
        ir_tx_send_intr(ir_buf, (u32)n);
    } else {
        ir_tx_send_poll(ir_buf, (u32)n);
    }
    IR_UNLOCK();
    return 0;
}

/* dev:receive([timeout_ms]) → symbols | nil, "timeout"
 * Returns a flat list: { {level=0|1, duration_us=N}, ... }
 * At 1 MHz sample clock, duration count equals duration in microseconds.
 * Requires an external IR source (remote control or another IR transmitter).
 * WARNING: switches hardware to RX mode — do not call concurrently with send_raw().
 */
static int lua_driver_ir_receive(lua_State *L)
{
    lua_driver_ir_ud_t *ud         = ir_get_ud(L, 1);
    lua_Integer         timeout_ms = luaL_optinteger(L, 2, 5000);
    if (ud->rx_pin == NC) {
        return luaL_error(L, "ir: receive called on a TX-only device (no rx_pin configured)");
    }

    /* Hold the peripheral lock for the entire receive transaction so no other
     * Lua task can call send_raw() while we are waiting for the frame.  The
     * ISR only gives s_rx.end_sema — it never tries to take s_ir_lock — so
     * blocking on the semaphore inside the lock cannot deadlock (CONC-01). */
    IR_LOCK();
    IR_Cmd(IR_DEV, IR_MODE_TX, DISABLE);
    IR_Cmd(IR_DEV, IR_MODE_RX, DISABLE);

    IR_InitTypeDef init;
    IR_StructInit(&init);
    init.IR_Clock          = IR_CLOCK_HZ;
    init.IR_Mode           = IR_MODE_RX;
    init.IR_Freq           = (u32)IR_RX_SAMPLE_HZ;
    init.IR_RxFIFOThrLevel = 5;
    init.IR_RxStartMode    = IR_RX_AUTO_MODE;
    init.IR_RxCntThrType   = IR_RX_COUNT_LOW_LEVEL;
    init.IR_RxCntThr       = IR_RX_END_SILENCE_CNT;
    IR_Init(IR_DEV, &init);

    s_rx.len = 0;
    s_rx.carrier_accum = 0;
    IR_ClearRxFIFO(IR_DEV);

    IR_INTConfig(IR_DEV,
                 IR_BIT_RX_FIFO_LEVEL_INT_EN |
                 IR_BIT_RX_FIFO_FULL_INT_EN  |
                 IR_BIT_RX_CNT_THR_INT_EN    |
                 IR_BIT_RX_CNT_OF_INT_EN     |
                 IR_BIT_RX_FIFO_OF_INT_EN,
                 ENABLE);

    IR_Cmd(IR_DEV, IR_MODE_RX, ENABLE);

    int rc = rtos_sema_take(s_rx.end_sema, (u32)timeout_ms);

    IR_Cmd(IR_DEV, IR_MODE_RX, DISABLE);
    IR_INTConfig(IR_DEV, IR_RX_INT_ALL_EN, DISABLE);
    /* Capture length while still inside the lock; hardware is now disabled so
     * the ISR cannot increment s_rx.len after this point. */
    u32 count = (u32)s_rx.len;
    IR_UNLOCK();  /* release before any Lua stack or memory operations */

    if (rc != RTK_SUCCESS) {
        lua_pushnil(L);
        lua_pushstring(L, "timeout");
        return 2;
    }

    lua_createtable(L, (int)count, 0);
    for (u32 i = 0; i < count; i++) {
        u32 raw     = (u32)s_rx.buf[i];
        int level   = (raw & IR_BIT_RX_LEVEL) ? 1 : 0;
        u32 dur_us  = raw & (u32)IR_MASK_RX_CNT;
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, (lua_Integer)level);
        lua_setfield(L, -2, "level");
        lua_pushinteger(L, (lua_Integer)dur_us);
        lua_setfield(L, -2, "duration_us");
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

/* dev:info() → {carrier_hz, tx_pin, rx_pin}  (nil if pin not configured) */
static int lua_driver_ir_info(lua_State *L)
{
    lua_driver_ir_ud_t *ud = ir_get_ud(L, 1);
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, (lua_Integer)ud->carrier_hz);
    lua_setfield(L, -2, "carrier_hz");
    if (ud->tx_pin != NC) {
        lua_pushinteger(L, (lua_Integer)ud->tx_pin);
    } else {
        lua_pushnil(L);
    }
    lua_setfield(L, -2, "tx_pin");
    if (ud->rx_pin != NC) {
        lua_pushinteger(L, (lua_Integer)ud->rx_pin);
    } else {
        lua_pushnil(L);
    }
    lua_setfield(L, -2, "rx_pin");
    return 1;
}

/* dev:close() */
static int lua_driver_ir_close(lua_State *L)
{
    /* luaL_checkudata can longjmp on type mismatch — must be outside lock. */
    lua_driver_ir_ud_t *ud = (lua_driver_ir_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_IR_METATABLE);
    if (!ud->closed) {
        IR_LOCK();
        IR_Cmd(IR_DEV, IR_MODE_TX, DISABLE);
        IR_Cmd(IR_DEV, IR_MODE_RX, DISABLE);
        IR_DeInit();
        RCC_PeriphClockCmd(APBPeriph_IRDA, APBPeriph_IRDA_CLOCK, DISABLE);
        IR_UNLOCK();
        ud->closed = 1;
    }
    return 0;
}

static int lua_driver_ir_gc(lua_State *L)
{
    lua_driver_ir_ud_t *ud = (lua_driver_ir_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_IR_METATABLE);
    if (ud && !ud->closed) {
        IR_LOCK();
        IR_Cmd(IR_DEV, IR_MODE_TX, DISABLE);
        IR_Cmd(IR_DEV, IR_MODE_RX, DISABLE);
        IR_DeInit();
        RCC_PeriphClockCmd(APBPeriph_IRDA, APBPeriph_IRDA_CLOCK, DISABLE);
        IR_UNLOCK();
        ud->closed = 1;
    }
    return 0;
}

/* ---- Driver-level init (called once from lua_module_registry_provision_all,
 *      single-threaded boot phase, before any concurrent Lua execution).
 *      Creates the peripheral mutex and allocates the semaphores used by
 *      the ISR, moving them out of the lazy-init path in new() that would
 *      be unsafe under concurrent callers (CONC-01/02). ---- */
void lua_driver_ir_init(void)
{
    if (s_ir_lock == NULL) {
        rtos_mutex_create(&s_ir_lock);
    }
    if (!s_rx.initialized) {
        rtos_sema_create_binary(&s_rx.end_sema);
        rtos_sema_create_binary(&s_tx.done_sema);
        InterruptRegister((IRQ_FUN)ir_irq_handler, IR_IRQ, (u32)NULL,
                          IR_IRQ_PRIORITY);
        InterruptEn(IR_IRQ, IR_IRQ_PRIORITY);
        s_rx.initialized = 1;
    }
}

/* ---- module entry --------------------------------------------------------- */

int luaopen_ir(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_DRIVER_IR_METATABLE)) {
        lua_pushcfunction(L, lua_driver_ir_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_driver_ir_send_raw);
        lua_setfield(L, -2, "send_raw");
        lua_pushcfunction(L, lua_driver_ir_receive);
        lua_setfield(L, -2, "receive");
        lua_pushcfunction(L, lua_driver_ir_info);
        lua_setfield(L, -2, "info");
        lua_pushcfunction(L, lua_driver_ir_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_driver_ir_new);
    lua_setfield(L, -2, "new");
    return 1;
}

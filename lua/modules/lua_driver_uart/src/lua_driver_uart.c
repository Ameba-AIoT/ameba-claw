/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_driver_uart.h"

#include <string.h>

#include "ameba_soc.h"
#include "lauxlib.h"
#include "luhw.h"
#include "os_wrapper.h"

/* ---- Concurrency design (per concurrency.md STEP 1 classification) ----------
 *
 * Peripheral class : BUS type (UART) — one port is a single exclusive channel;
 *                    reads and writes must not interleave.
 * Called from ISR? : NO — all exported functions run in Lua task context.
 *                    => rtos_mutex is the correct primitive (decision tree).
 *
 * cap_lua allows several lua_run jobs to execute concurrently, so the SAME
 * port handle may be driven by multiple flows.  The driver follows Template A:
 *
 *   1. One rtos_mutex per controller, held for the WHOLE operation (new /
 *      read / read_line / write / available / flush_input / set_loopback).
 *      Created once in lua_driver_uart_init() during single-threaded boot.
 *   2. Exclusive port ownership: only one handle may open a given port at a
 *      time.  new() fails with an error if the port is already in use.
 *   3. Reference counting (0 or 1) drives init/deinit.  close() immediately
 *      drops the reference and deinits the hardware (unlike I2C which defers
 *      to __gc) so the port can be reopened right after close().  __gc is a
 *      safety net for handles that are never explicitly closed.
 *
 * Critical-section purity (CONC-02): all parameter validation, memory
 * allocation (lua_newuserdata), and luaL_check* calls happen BEFORE
 * rtos_mutex_take.  Nothing that can longjmp (luaL_error / luaL_check* /
 * lua_newuserdata) is ever called while the lock is held.
 * ---------------------------------------------------------------------------*/

#define LUA_DRIVER_UART_METATABLE       "uart.port"
#define LUA_DRIVER_UART_MAX_READ        4096
#define LUA_DRIVER_UART_MAX_LINE        1024
#define LUA_DRIVER_UART_LOCK_TIMEOUT_MS 5000

/* Per-controller shared state. */
typedef struct {
    rtos_mutex_t lock;    /* whole-operation guard, one per controller */
    int          refcnt;  /* 0 = free, 1 = one handle owns this port */
    u8           inited;  /* 1 after UART_Init; cleared when refcnt hits 0 */
    u8           tx_pin;  /* configured TX pad while inited */
    u8           rx_pin;  /* configured RX pad while inited */
    u32          baud;    /* configured baud rate while inited */
} lua_driver_uart_ctrl_t;

static lua_driver_uart_ctrl_t s_uart_ctrl[MAX_UART_INDEX];

static const u32 s_uart_tx_pinmux[] = {
    PINMUX_FUNCTION_UART0_TXD,
    PINMUX_FUNCTION_UART1_TXD,
    PINMUX_FUNCTION_UART2_TXD,
    PINMUX_FUNCTION_UART3_TXD,
};

typedef struct {
    UART_TypeDef *dev;
    int           port;     /* index into s_uart_ctrl */
    int           closed;
    int           counted;  /* 1 if this handle holds a controller reference */
} lua_driver_uart_ud_t;

/* ---- Controller lock helpers --------------------------------------------- */

/* Acquire the controller lock with a bounded wait.  On timeout the lock is
 * NOT held, so raising luaL_error here is safe (no give is owed).  Must only
 * be called before entering the critical section. */
static void lua_driver_uart_lock(lua_State *L, int idx)
{
    if (rtos_mutex_take(s_uart_ctrl[idx].lock,
                        LUA_DRIVER_UART_LOCK_TIMEOUT_MS) != RTK_SUCCESS) {
        luaL_error(L, "uart%d: controller busy (lock timeout)", idx);
    }
}

/* Drop one controller reference.  Only called from close() and __gc (Lua
 * task context, never ISR).  Uses RTOS_MAX_DELAY because all I/O paths that
 * hold this lock are bounded (read/read_line: user-supplied timeout;
 * write/available/flush/set_loopback: instantaneous or bounded poll), so
 * the lock is always released within a finite time.
 * Deinits hardware when the last reference is gone.
 * Touches no longjmp-capable API. */
static void lua_driver_uart_ctrl_release(int idx)
{
    lua_driver_uart_ctrl_t *c = &s_uart_ctrl[idx];
    rtos_mutex_take(c->lock, RTOS_MAX_DELAY);
    if (c->refcnt > 0) {
        c->refcnt--;
    }
    if (c->refcnt == 0 && c->inited) {
        /* Drain TX before resetting the peripheral.  write() queues bytes into
         * the HW FIFO and returns immediately; without this guard a close()
         * immediately after write() aborts bytes still being serialised.
         * Poll TX_EMPTY (bit 5 of LSR) — FIFO drains within one byte-time per
         * byte at the configured baud, so 30ms covers the full 16-deep FIFO
         * even at 9600 baud.  rtos_time_delay_ms() is safe here (task ctx). */
        {
            u32 _i;
            for (_i = 0; _i < 30U; _i++) {
                if (UART_DEV_TABLE[idx].UARTx->LSR & RUART_BIT_TX_EMPTY) { break; }
                rtos_time_delay_ms(1);
            }
            /* One extra ms for the shift register to serialise the last byte. */
            rtos_time_delay_ms(1);
        }
        UART_DeInit(UART_DEV_TABLE[idx].UARTx);
        RCC_PeriphClockCmd(APBPeriph_UARTx[idx], APBPeriph_UARTx_CLOCK[idx], DISABLE);
        /* Release pinmux so stale UART function on old pads cannot conflict
         * with a subsequent new() that uses different pins on the same port. */
        Pinmux_Config(c->tx_pin, PINMUX_FUNCTION_GPIO);
        Pinmux_Config(c->rx_pin, PINMUX_FUNCTION_GPIO);
        c->inited = 0;
        c->tx_pin = 0;
        c->rx_pin = 0;
    }
    rtos_mutex_give(c->lock);
}

/* ---- Helpers ---- */

/* May luaL_error — must be called BEFORE any lock take. */
static lua_driver_uart_ud_t *lua_driver_uart_get_ud(lua_State *L, int idx)
{
    lua_driver_uart_ud_t *ud = (lua_driver_uart_ud_t *)luaL_checkudata(
        L, idx, LUA_DRIVER_UART_METATABLE);
    if (!ud || ud->closed) {
        luaL_error(L, "uart: invalid or closed port");
    }
    return ud;
}

/* ---- uart.new(port, tx_pin, rx_pin, baud [, opts]) → handle -------------- */
static int lua_driver_uart_new(lua_State *L)
{
    /* Step 1: validate all params — may luaL_error, no lock held. */
    lua_Integer port_num = luaL_checkinteger(L, 1);
    PinName     tx_pin   = luhw_check_pin(L, 2);
    PinName     rx_pin   = luhw_check_pin(L, 3);
    lua_Integer baud     = luaL_checkinteger(L, 4);

    if (port_num < 0 || port_num >= MAX_UART_INDEX) {
        return luaL_error(L, "uart port must be 0-%d", MAX_UART_INDEX - 1);
    }
    if (baud <= 0) {
        return luaL_error(L, "uart baud must be positive");
    }

    u32 word_len    = RUART_WLS_8BITS;
    u32 parity      = RUART_PARITY_DISABLE;
    u32 parity_type = RUART_ODD_PARITY;
    u32 stop_bit    = RUART_STOP_BIT_1;

    if (!lua_isnoneornil(L, 5)) {
        luaL_checktype(L, 5, LUA_TTABLE);

        lua_getfield(L, 5, "data_bits");
        if (!lua_isnil(L, -1)) {
            int db = (int)luaL_checkinteger(L, -1);
            if (db == 7) {
                word_len = RUART_WLS_7BITS;
            } else if (db == 8) {
                word_len = RUART_WLS_8BITS;
            } else {
                return luaL_error(L, "uart data_bits must be 7 or 8");
            }
        }
        lua_pop(L, 1);

        lua_getfield(L, 5, "parity");
        if (!lua_isnil(L, -1)) {
            const char *ps = luaL_checkstring(L, -1);
            if (strcmp(ps, "none") == 0) {
                parity = RUART_PARITY_DISABLE;
            } else if (strcmp(ps, "odd") == 0) {
                parity      = RUART_PARITY_ENABLE;
                parity_type = RUART_ODD_PARITY;
            } else if (strcmp(ps, "even") == 0) {
                parity      = RUART_PARITY_ENABLE;
                parity_type = RUART_EVEN_PARITY;
            } else {
                return luaL_error(L, "uart parity must be 'none', 'odd', or 'even'");
            }
        }
        lua_pop(L, 1);

        lua_getfield(L, 5, "stop_bits");
        if (!lua_isnil(L, -1)) {
            int sb = (int)luaL_checkinteger(L, -1);
            if (sb == 1) {
                stop_bit = RUART_STOP_BIT_1;
            } else if (sb == 2) {
                stop_bit = RUART_STOP_BIT_2;
            } else {
                return luaL_error(L, "uart stop_bits must be 1 or 2");
            }
        }
        lua_pop(L, 1);
    }

    int idx = (int)port_num;

    /* Step 2: allocate and stamp the handle BEFORE touching the controller
     * lock so no longjmp-capable allocation happens inside the critical
     * section (CONC-02). */
    lua_driver_uart_ud_t *ud = (lua_driver_uart_ud_t *)lua_newuserdata(
        L, sizeof(*ud));
    ud->dev     = UART_DEV_TABLE[idx].UARTx;
    ud->port    = idx;
    ud->closed  = 0;
    ud->counted = 0;
    luaL_getmetatable(L, LUA_DRIVER_UART_METATABLE);
    lua_setmetatable(L, -2);

    /* Step 3: take the controller lock.  lua_driver_uart_lock raises
     * luaL_error on timeout — safe here because the lock is NOT held. */
    lua_driver_uart_lock(L, idx);

    lua_driver_uart_ctrl_t *c = &s_uart_ctrl[idx];

    /* Step 4: exclusive ownership check. */
    if (c->inited && c->refcnt > 0) {
        rtos_mutex_give(c->lock);
        return luaL_error(L, "uart%d: port already in use", idx);
    }

    /* Step 5: first (or sole) user — configure hardware from scratch. */
    RCC_PeriphClockCmd(APBPeriph_UARTx[idx], APBPeriph_UARTx_CLOCK[idx], ENABLE);

    /* On RTL8721F, RXD function code is always TXD+1 for all four UARTs:
     * UART0 TX=95 RX=96, UART1 TX=99 RX=100, UART2 TX=101 RX=102,
     * UART3 TX=103 RX=104 (ameba_pinmux.h lines 265-274). */
    Pinmux_Config((u8)tx_pin, s_uart_tx_pinmux[idx]);
    Pinmux_Config((u8)rx_pin, s_uart_tx_pinmux[idx] + 1);
    PAD_PullCtrl((u8)tx_pin, GPIO_PuPd_UP);
    PAD_PullCtrl((u8)rx_pin, GPIO_PuPd_UP);

    UART_InitTypeDef uart_init;
    UART_StructInit(&uart_init);
    uart_init.WordLen         = word_len;
    uart_init.StopBit         = stop_bit;
    uart_init.Parity          = parity;
    uart_init.ParityType      = parity_type;
    uart_init.RxFifoTrigLevel = UART_RX_FIFOTRIG_LEVEL_1BYTES;

    UART_Init(ud->dev, &uart_init);
    UART_SetBaud(ud->dev, (u32)baud);
    UART_RxCmd(ud->dev, ENABLE);

    /* Step 6: update controller state. */
    c->inited  = 1;
    c->tx_pin  = (u8)tx_pin;
    c->rx_pin  = (u8)rx_pin;
    c->baud    = (u32)baud;
    c->refcnt++;

    rtos_mutex_give(c->lock);

    ud->counted = 1;
    return 1;
}

/* ---- handle:read(len [, timeout_ms]) → string ---------------------------- */
static int lua_driver_uart_read(lua_State *L)
{
    /* All checks and allocations BEFORE the lock (CONC-02). */
    lua_driver_uart_ud_t *ud      = lua_driver_uart_get_ud(L, 1);
    lua_Integer           len     = luaL_checkinteger(L, 2);
    lua_Integer           timeout = luaL_optinteger(L, 3, 0);

    if (len <= 0 || len > LUA_DRIVER_UART_MAX_READ) {
        return luaL_error(L, "uart read length must be 1-%d",
                          LUA_DRIVER_UART_MAX_READ);
    }

    /* Allocate receive buffer BEFORE lock — lua_newuserdata may trigger GC. */
    u8 *buf = (u8 *)lua_newuserdata(L, (size_t)len);

    int port = ud->port;
    lua_driver_uart_lock(L, port);  /* raises on timeout — no lock held yet */

    lua_Integer received = 0;
    lua_Integer elapsed  = 0;
    while (received < len) {
        if (UART_Readable(ud->dev)) {
            UART_CharGet(ud->dev, &buf[received++]);
        } else {
            if (elapsed >= timeout) {
                break;
            }
            rtos_time_delay_ms(1);
            elapsed++;
        }
    }

    rtos_mutex_give(s_uart_ctrl[port].lock);

    /* Push result string AFTER lock released. */
    lua_pushlstring(L, (const char *)buf, (size_t)received);
    return 1;
}

/* ---- handle:read_line([max_len [, timeout_ms]]) → string ----------------- */
static int lua_driver_uart_read_line(lua_State *L)
{
    lua_driver_uart_ud_t *ud      = lua_driver_uart_get_ud(L, 1);
    lua_Integer           max_len = luaL_optinteger(L, 2, LUA_DRIVER_UART_MAX_LINE);
    lua_Integer           timeout = luaL_optinteger(L, 3, 0);

    if (max_len <= 0 || max_len > LUA_DRIVER_UART_MAX_LINE) {
        return luaL_error(L, "uart read_line max_len must be 1-%d",
                          LUA_DRIVER_UART_MAX_LINE);
    }

    u8 *buf = (u8 *)lua_newuserdata(L, (size_t)max_len);

    int port = ud->port;
    lua_driver_uart_lock(L, port);

    lua_Integer received = 0;
    lua_Integer elapsed  = 0;
    while (received < max_len) {
        if (UART_Readable(ud->dev)) {
            u8 byte;
            UART_CharGet(ud->dev, &byte);
            buf[received++] = byte;
            if (byte == '\n') {
                break;
            }
        } else {
            if (elapsed >= timeout) {
                break;
            }
            rtos_time_delay_ms(1);
            elapsed++;
        }
    }

    rtos_mutex_give(s_uart_ctrl[port].lock);

    lua_pushlstring(L, (const char *)buf, (size_t)received);
    return 1;
}

/* ---- handle:write(data) → bytes_sent  data: string or table of bytes ---- */
static int lua_driver_uart_write(lua_State *L)
{
    lua_driver_uart_ud_t *ud = lua_driver_uart_get_ud(L, 1);

    const u8 *data     = NULL;
    size_t    data_len = 0;

    int type = lua_type(L, 2);
    if (type == LUA_TSTRING) {
        data = (const u8 *)lua_tolstring(L, 2, &data_len);
    } else if (type == LUA_TTABLE) {
        lua_Integer n = luaL_len(L, 2);
        if (n < 0 || n > LUA_DRIVER_UART_MAX_READ) {
            return luaL_error(L, "uart write table too large (max %d)",
                              LUA_DRIVER_UART_MAX_READ);
        }
        /* Allocate BEFORE lock — lua_newuserdata may trigger GC. */
        u8 *tmp = (u8 *)lua_newuserdata(L, (size_t)(n > 0 ? n : 1));
        for (lua_Integer i = 0; i < n; i++) {
            lua_rawgeti(L, 2, i + 1);
            lua_Integer byte = luaL_checkinteger(L, -1);  /* may error — no lock */
            if (byte < 0 || byte > 0xFF) {
                return luaL_error(L, "uart write byte #%d out of range 0-255",
                                  (int)(i + 1));
            }
            tmp[i] = (u8)byte;
            lua_pop(L, 1);
        }
        data     = tmp;
        data_len = (size_t)n;
    } else {
        return luaL_error(L, "uart write expects a string or table");
    }

    int port = ud->port;
    lua_driver_uart_lock(L, port);
    if (data_len > 0) {
        UART_SendData(ud->dev, (u8 *)data, (u32)data_len);
    }
    rtos_mutex_give(s_uart_ctrl[port].lock);

    lua_pushinteger(L, (lua_Integer)data_len);
    return 1;
}

/* ---- handle:available() → 0 or 1 ---------------------------------------- */
static int lua_driver_uart_available(lua_State *L)
{
    lua_driver_uart_ud_t *ud = lua_driver_uart_get_ud(L, 1);
    int port = ud->port;
    lua_driver_uart_lock(L, port);
    int readable = (int)UART_Readable(ud->dev);
    rtos_mutex_give(s_uart_ctrl[port].lock);
    lua_pushinteger(L, (lua_Integer)readable);
    return 1;
}

/* ---- handle:flush_input() ------------------------------------------------ */
static int lua_driver_uart_flush_input(lua_State *L)
{
    lua_driver_uart_ud_t *ud = lua_driver_uart_get_ud(L, 1);
    int port = ud->port;
    lua_driver_uart_lock(L, port);
    UART_ClearRxFifo(ud->dev);
    rtos_mutex_give(s_uart_ctrl[port].lock);
    return 0;
}

/* ---- handle:set_loopback(enable) ----------------------------------------- */
static int lua_driver_uart_set_loopback(lua_State *L)
{
    lua_driver_uart_ud_t *ud     = lua_driver_uart_get_ud(L, 1);
    int                   enable = lua_toboolean(L, 2);  /* never errors */
    int port = ud->port;
    lua_driver_uart_lock(L, port);
    if (enable) {
        ud->dev->MCR |= RUART_BIT_LOOP_EN;
    } else {
        ud->dev->MCR &= ~RUART_BIT_LOOP_EN;
    }
    rtos_mutex_give(s_uart_ctrl[port].lock);
    return 0;
}

/* ---- handle:close() ------------------------------------------------------ */
static int lua_driver_uart_close(lua_State *L)
{
    lua_driver_uart_ud_t *ud = (lua_driver_uart_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_UART_METATABLE);
    if (!ud->closed) {
        ud->closed = 1;
        if (ud->counted) {
            lua_driver_uart_ctrl_release(ud->port);
            ud->counted = 0;
        }
    }
    return 0;
}

/* ---- __gc: safety net for handles never explicitly closed ---------------- */
static int lua_driver_uart_gc(lua_State *L)
{
    lua_driver_uart_ud_t *ud = (lua_driver_uart_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_UART_METATABLE);
    if (ud) {
        if (ud->counted) {
            lua_driver_uart_ctrl_release(ud->port);
            ud->counted = 0;
        }
        ud->closed = 1;
    }
    return 0;
}

/* ---- Driver-level init ---------------------------------------------------- */

/* Called once from lua_module_registry_provision_all() in the single-threaded
 * boot phase, before any concurrent Lua execution can start. */
void lua_driver_uart_init(void)
{
    for (int i = 0; i < MAX_UART_INDEX; i++) {
        if (s_uart_ctrl[i].lock == NULL) {
            rtos_mutex_create(&s_uart_ctrl[i].lock);
        }
        s_uart_ctrl[i].refcnt = 0;
        s_uart_ctrl[i].inited = 0;
        s_uart_ctrl[i].tx_pin = 0;
        s_uart_ctrl[i].rx_pin = 0;
        s_uart_ctrl[i].baud   = 0;
    }
}

/* ---- Module open ---- */
int luaopen_uart(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_DRIVER_UART_METATABLE)) {
        lua_pushcfunction(L, lua_driver_uart_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_driver_uart_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_driver_uart_read_line);
        lua_setfield(L, -2, "read_line");
        lua_pushcfunction(L, lua_driver_uart_write);
        lua_setfield(L, -2, "write");
        lua_pushcfunction(L, lua_driver_uart_available);
        lua_setfield(L, -2, "available");
        lua_pushcfunction(L, lua_driver_uart_flush_input);
        lua_setfield(L, -2, "flush_input");
        lua_pushcfunction(L, lua_driver_uart_set_loopback);
        lua_setfield(L, -2, "set_loopback");
        lua_pushcfunction(L, lua_driver_uart_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_driver_uart_new);
    lua_setfield(L, -2, "new");
    return 1;
}

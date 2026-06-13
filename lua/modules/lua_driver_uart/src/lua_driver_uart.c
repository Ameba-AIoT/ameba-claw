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

#define LUA_DRIVER_UART_METATABLE   "uart.port"
#define LUA_DRIVER_UART_MAX_READ    4096
#define LUA_DRIVER_UART_MAX_LINE    1024

typedef struct {
    UART_TypeDef *dev;
    int           port;
    int           closed;
} lua_driver_uart_ud_t;

static const u32 s_uart_tx_pinmux[] = {
    PINMUX_FUNCTION_UART0_TXD,
    PINMUX_FUNCTION_UART1_TXD,
    PINMUX_FUNCTION_UART2_TXD,
    PINMUX_FUNCTION_UART3_TXD,
};

static lua_driver_uart_ud_t *lua_driver_uart_get_ud(lua_State *L, int idx)
{
    lua_driver_uart_ud_t *ud = (lua_driver_uart_ud_t *)luaL_checkudata(
        L, idx, LUA_DRIVER_UART_METATABLE);
    if (!ud || ud->closed) {
        luaL_error(L, "uart: invalid or closed port");
    }
    return ud;
}

static int lua_driver_uart_new(lua_State *L)
{
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

    RCC_PeriphClockCmd(APBPeriph_UARTx[idx], APBPeriph_UARTx_CLOCK[idx], ENABLE);

    // /* PA_18 (SWD_CLK) and PA_19 (SWD_DAT) are shared with the SWD debug port.
    //  * Disable SWD before reconfiguring them as UART pins. */
    // if ((u8)tx_pin == SWD_CLK || (u8)tx_pin == SWD_DAT ||
    //     (u8)rx_pin == SWD_CLK || (u8)rx_pin == SWD_DAT) {
    //     Pinmux_Swdoff();
    // }

    Pinmux_Config((u8)tx_pin, s_uart_tx_pinmux[idx]);
    /* On RTL8721F, RXD function code is always TXD+1 for all four UARTs:
     * UART0 TX=95 RX=96, UART1 TX=99 RX=100, UART2 TX=101 RX=102, UART3 TX=103 RX=104
     * (ameba_pinmux.h lines 265-274). This is a hardware constant, not an assumption. */
    Pinmux_Config((u8)rx_pin, s_uart_tx_pinmux[idx] + 1);
    PAD_PullCtrl((u8)tx_pin, GPIO_PuPd_UP);
    PAD_PullCtrl((u8)rx_pin, GPIO_PuPd_UP);

    UART_InitTypeDef uart_init;
    UART_StructInit(&uart_init);
    uart_init.WordLen          = word_len;
    uart_init.StopBit          = stop_bit;
    uart_init.Parity           = parity;
    uart_init.ParityType       = parity_type;
    uart_init.RxFifoTrigLevel  = UART_RX_FIFOTRIG_LEVEL_1BYTES;

    UART_TypeDef *dev = UART_DEV_TABLE[idx].UARTx;
    UART_Init(dev, &uart_init);
    UART_SetBaud(dev, (u32)baud);
    UART_RxCmd(dev, ENABLE);

    lua_driver_uart_ud_t *ud = (lua_driver_uart_ud_t *)lua_newuserdata(
        L, sizeof(*ud));
    ud->dev    = dev;
    ud->port   = idx;
    ud->closed = 0;
    luaL_getmetatable(L, LUA_DRIVER_UART_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

static int lua_driver_uart_read(lua_State *L)
{
    lua_driver_uart_ud_t *ud      = lua_driver_uart_get_ud(L, 1);
    lua_Integer           len     = luaL_checkinteger(L, 2);
    lua_Integer           timeout = luaL_optinteger(L, 3, 0);

    if (len <= 0 || len > LUA_DRIVER_UART_MAX_READ) {
        return luaL_error(L, "uart read length must be 1-%d",
                          LUA_DRIVER_UART_MAX_READ);
    }

    luaL_Buffer b;
    luaL_buffinit(L, &b);

    lua_Integer received = 0;
    lua_Integer elapsed  = 0;

    while (received < len) {
        if (UART_Readable(ud->dev)) {
            u8 byte;
            UART_CharGet(ud->dev, &byte);
            luaL_addchar(&b, (char)byte);
            received++;
        } else {
            if (elapsed >= timeout) {
                break;
            }
            rtos_time_delay_ms(1);
            elapsed++;
        }
    }

    luaL_pushresult(&b);
    return 1;
}

static int lua_driver_uart_read_line(lua_State *L)
{
    lua_driver_uart_ud_t *ud      = lua_driver_uart_get_ud(L, 1);
    lua_Integer           max_len = luaL_optinteger(L, 2, LUA_DRIVER_UART_MAX_LINE);
    lua_Integer           timeout = luaL_optinteger(L, 3, 0);

    if (max_len <= 0 || max_len > LUA_DRIVER_UART_MAX_LINE) {
        return luaL_error(L, "uart read_line max_len must be 1-%d",
                          LUA_DRIVER_UART_MAX_LINE);
    }

    luaL_Buffer b;
    luaL_buffinit(L, &b);

    lua_Integer received = 0;
    lua_Integer elapsed  = 0;

    while (received < max_len) {
        if (UART_Readable(ud->dev)) {
            u8 byte;
            UART_CharGet(ud->dev, &byte);
            luaL_addchar(&b, (char)byte);
            received++;
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

    luaL_pushresult(&b);
    return 1;
}

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
        u8 *tmp = (u8 *)lua_newuserdata(L, (size_t)(n > 0 ? n : 1));
        for (lua_Integer i = 0; i < n; i++) {
            lua_rawgeti(L, 2, i + 1);
            lua_Integer byte = luaL_checkinteger(L, -1);
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

    if (data_len > 0) {
        UART_SendData(ud->dev, (u8 *)data, (u32)data_len);
    }

    lua_pushinteger(L, (lua_Integer)data_len);
    return 1;
}

static int lua_driver_uart_available(lua_State *L)
{
    lua_driver_uart_ud_t *ud = lua_driver_uart_get_ud(L, 1);
    lua_pushinteger(L, (lua_Integer)UART_Readable(ud->dev));
    return 1;
}

static int lua_driver_uart_flush_input(lua_State *L)
{
    lua_driver_uart_ud_t *ud = lua_driver_uart_get_ud(L, 1);
    UART_ClearRxFifo(ud->dev);
    return 0;
}

static int lua_driver_uart_set_loopback(lua_State *L)
{
    lua_driver_uart_ud_t *ud     = lua_driver_uart_get_ud(L, 1);
    int                   enable = lua_toboolean(L, 2);
    if (enable) {
        ud->dev->MCR |= RUART_BIT_LOOP_EN;
    } else {
        ud->dev->MCR &= ~RUART_BIT_LOOP_EN;
    }
    return 0;
}

static int lua_driver_uart_close(lua_State *L)
{
    lua_driver_uart_ud_t *ud = (lua_driver_uart_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_UART_METATABLE);
    if (!ud->closed) {
        UART_DeInit(ud->dev);
        RCC_PeriphClockCmd(APBPeriph_UARTx[ud->port],
                           APBPeriph_UARTx_CLOCK[ud->port], DISABLE);
        ud->closed = 1;
    }
    return 0;
}

static int lua_driver_uart_gc(lua_State *L)
{
    lua_driver_uart_ud_t *ud = (lua_driver_uart_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_UART_METATABLE);
    if (ud && !ud->closed) {
        UART_DeInit(ud->dev);
        RCC_PeriphClockCmd(APBPeriph_UARTx[ud->port],
                           APBPeriph_UARTx_CLOCK[ud->port], DISABLE);
        ud->closed = 1;
    }
    return 0;
}

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

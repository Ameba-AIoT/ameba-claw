/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_uart_test_provision.c — Writes UART test scripts to VFS on every boot.
**
** Two scripts are provisioned:
**   test_uart_at.lua       — AT-style test (PA_18 TX / PA_19 RX), auto-run on boot.
**                            Source-of-truth: uart_at.lua (embedded via uart_at_lua.h).
**   test_uart_loopback.lua — hardware loopback self-test (PA_12/PA_13), NOT auto-run
**
** Kept separate from lua_driver_uart.c so the driver stays free of test code.
** Overwrites on every boot so the scripts always match the current firmware.
*/

#include <stdio.h>
#include <string.h>

/* Hardware loopback self-test — NOT auto-run on boot.
 * Load manually from the REPL: dofile("vfs:test_uart_loopback.lua") */
static const char s_loopback_script[] =
    "local uart = require('uart')\n"
    "local sys  = require('sys')\n"
    "\n"
    "local PORT     = 0\n"
    "local TX_PIN   = 'PA_12'\n"
    "local RX_PIN   = 'PA_13'\n"
    "local BAUD     = 115200\n"
    "local TEST_STR = 'Hello UART loopback!'\n"
    "\n"
    "local function close_uart(u)\n"
    "    if not u then return end\n"
    "    local ok, err = pcall(u.close, u)\n"
    "    if not ok then\n"
    "        print('[uart_loopback] WARN: close failed: ' .. tostring(err))\n"
    "    end\n"
    "end\n"
    "\n"
    "local function test()\n"
    "    print(string.format(\n"
    "        '[uart_loopback] open UART%d tx=%s rx=%s baud=%d',\n"
    "        PORT, TX_PIN, RX_PIN, BAUD))\n"
    "\n"
    "    local ok, u = pcall(uart.new, PORT, TX_PIN, RX_PIN, BAUD)\n"
    "    if not ok then\n"
    "        print('[uart_loopback] ERROR: uart.new failed: ' .. tostring(u))\n"
    "        return false\n"
    "    end\n"
    "\n"
    "    u:set_loopback(true)\n"
    "    u:flush_input()\n"
    "\n"
    "    local ok_w, sent = pcall(u.write, u, TEST_STR)\n"
    "    if not ok_w then\n"
    "        print('[uart_loopback] ERROR: write failed: ' .. tostring(sent))\n"
    "        close_uart(u)\n"
    "        return false\n"
    "    end\n"
    "    print(string.format('[uart_loopback] sent %d bytes', sent))\n"
    "\n"
    "    sys.sleep_ms(10)\n"
    "\n"
    "    local ok_r, rx = pcall(u.read, u, #TEST_STR, 200)\n"
    "    if not ok_r then\n"
    "        print('[uart_loopback] ERROR: read failed: ' .. tostring(rx))\n"
    "        close_uart(u)\n"
    "        return false\n"
    "    end\n"
    "\n"
    "    u:set_loopback(false)\n"
    "    close_uart(u)\n"
    "\n"
    "    if rx == TEST_STR then\n"
    "        print('[uart_loopback] success')\n"
    "        return true\n"
    "    else\n"
    "        print('[uart_loopback] FAIL: expected=' .. TEST_STR\n"
    "              .. ' got=' .. tostring(rx))\n"
    "        return false\n"
    "    end\n"
    "end\n"
    "\n"
    "local ok, result = pcall(test)\n"
    "if not ok then\n"
    "    print('[uart_loopback] ERROR: ' .. tostring(result))\n"
    "end\n";

/* s_uart_at_script is auto-generated from uart_at.lua at cmake configure time. */
#include "uart_at_lua.h"

void lua_driver_uart_provision(void)
{
    const char *uart_at_path = "vfs:test_uart_at.lua";
    { FILE *_ck = fopen(uart_at_path, "r"); if (_ck) { fclose(_ck); return; } }

    FILE *f = fopen(uart_at_path, "w");
    if (f) {
        fwrite(s_uart_at_script, 1, strlen(s_uart_at_script), f);
        fclose(f);
    }

    const char *loopback_path = "vfs:test_uart_loopback.lua";
    f = fopen(loopback_path, "w");
    if (f) {
        fwrite(s_loopback_script, 1, strlen(s_loopback_script), f);
        fclose(f);
    }
}

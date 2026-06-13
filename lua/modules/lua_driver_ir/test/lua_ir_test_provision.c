/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_ir_test_provision.c — Writes IR test scripts to VFS on boot.
**
** Scripts are written to VFS but NOT auto-run on boot.
** Trigger via AT command:
**   AT+CLAW=ir,tx        — NEC TX cross-test, polling mode (backward compat)
**   AT+CLAW=ir,tx,poll   — NEC TX cross-test, polling mode (explicit)
**   AT+CLAW=ir,tx,intr   — NEC TX cross-test, interrupt mode
**   AT+CLAW=ir,rx        — NEC RX cross-test (Dplus TX PA_26 → PA_26, 15 s timeout)
**
** NEC encoding is implemented in pure Lua (TX side).
*/

#include <stdio.h>
#include <string.h>

#include "ameba_soc.h"
#include "os_wrapper.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* TX poll test: NEC frame via PA_25 → Dplus RX PA_27, polling mode. */
static const char s_ir_tx_poll_script[] =
    "-- IR TX cross-test (polling mode): sends NEC frame via PA_25 -> Dplus RX PA_27.\n"
    "-- TX FIFO is refilled by the task itself (busy-poll on IR_GetTxFIFOFreeLen).\n"
    "local ir = require(\"ir\")\n"
    "local TX_PIN = \"PA_25\"\n"
    "local ADDR   = 0x12\n"
    "local CMD    = 0x34\n"
    "\n"
    "local function nec_encode(addr, cmd)\n"
    "    local syms = {}\n"
    "    local function push(lv, dur)\n"
    "        syms[#syms + 1] = {level = lv, duration_us = dur}\n"
    "    end\n"
    "    local function encode_byte(b)\n"
    "        for bit = 0, 7 do\n"
    "            push(1, 560)\n"
    "            if ((b >> bit) & 1) == 1 then push(0, 1690) else push(0, 560) end\n"
    "        end\n"
    "    end\n"
    "    push(1, 9000) push(0, 4500)\n"
    "    encode_byte(addr & 0xFF)\n"
    "    encode_byte((~addr) & 0xFF)\n"
    "    encode_byte(cmd & 0xFF)\n"
    "    encode_byte((~cmd) & 0xFF)\n"
    "    push(1, 560)\n"
    "    return syms\n"
    "end\n"
    "\n"
    "print(\"[ir_tx_poll] test start\")\n"
    "local dev\n"
    "local ok, result = pcall(ir.new, TX_PIN, nil)\n"
    "if not ok then\n"
    "    print(\"[ir_tx_poll] ERROR: ir.new: \" .. tostring(result))\n"
    "    return\n"
    "end\n"
    "dev = result\n"
    "\n"
    "local info = dev:info()\n"
    "print(string.format(\"[ir_tx_poll] tx=%s carrier=%dHz mode=poll\", TX_PIN, info.carrier_hz))\n"
    "\n"
    "local symbols = nec_encode(ADDR, CMD)\n"
    "local ok_raw, err_raw = pcall(dev.send_raw, dev, symbols, \"poll\")\n"
    "if ok_raw then\n"
    "    print(string.format(\"[ir_tx_poll] NEC send addr=0x%02X cmd=0x%02X done (%d symbols)\",\n"
    "        ADDR, CMD, #symbols))\n"
    "else\n"
    "    print(\"[ir_tx_poll] ERROR: send_raw: \" .. tostring(err_raw))\n"
    "    dev:close()\n"
    "    return\n"
    "end\n"
    "\n"
    "dev:close()\n"
    "print(\"[ir_tx_poll] success\")\n";

/* TX intr test: NEC frame via PA_25 → Dplus RX PA_27, interrupt mode. */
static const char s_ir_tx_intr_script[] =
    "-- IR TX cross-test (interrupt mode): sends NEC frame via PA_25 -> Dplus RX PA_27.\n"
    "-- TX FIFO is refilled by the ISR on IR_BIT_TX_FIFO_LEVEL_INT; Lua task blocks on\n"
    "-- RTOS semaphore until IR_BIT_TX_FIFO_EMPTY_INT fires (all data consumed from FIFO).\n"
    "local ir = require(\"ir\")\n"
    "local TX_PIN = \"PA_25\"\n"
    "local ADDR   = 0x12\n"
    "local CMD    = 0x34\n"
    "\n"
    "local function nec_encode(addr, cmd)\n"
    "    local syms = {}\n"
    "    local function push(lv, dur)\n"
    "        syms[#syms + 1] = {level = lv, duration_us = dur}\n"
    "    end\n"
    "    local function encode_byte(b)\n"
    "        for bit = 0, 7 do\n"
    "            push(1, 560)\n"
    "            if ((b >> bit) & 1) == 1 then push(0, 1690) else push(0, 560) end\n"
    "        end\n"
    "    end\n"
    "    push(1, 9000) push(0, 4500)\n"
    "    encode_byte(addr & 0xFF)\n"
    "    encode_byte((~addr) & 0xFF)\n"
    "    encode_byte(cmd & 0xFF)\n"
    "    encode_byte((~cmd) & 0xFF)\n"
    "    push(1, 560)\n"
    "    return syms\n"
    "end\n"
    "\n"
    "print(\"[ir_tx_intr] test start\")\n"
    "local dev\n"
    "local ok, result = pcall(ir.new, TX_PIN, nil)\n"
    "if not ok then\n"
    "    print(\"[ir_tx_intr] ERROR: ir.new: \" .. tostring(result))\n"
    "    return\n"
    "end\n"
    "dev = result\n"
    "\n"
    "local info = dev:info()\n"
    "print(string.format(\"[ir_tx_intr] tx=%s carrier=%dHz mode=intr\", TX_PIN, info.carrier_hz))\n"
    "\n"
    "local symbols = nec_encode(ADDR, CMD)\n"
    "local ok_raw, err_raw = pcall(dev.send_raw, dev, symbols, \"intr\")\n"
    "if ok_raw then\n"
    "    print(string.format(\"[ir_tx_intr] NEC send addr=0x%02X cmd=0x%02X done (%d symbols)\",\n"
    "        ADDR, CMD, #symbols))\n"
    "else\n"
    "    print(\"[ir_tx_intr] ERROR: send_raw: \" .. tostring(err_raw))\n"
    "    dev:close()\n"
    "    return\n"
    "end\n"
    "\n"
    "dev:close()\n"
    "print(\"[ir_tx_intr] success\")\n";

/* RX test: receive raw symbols from Dplus TX PA_26 (PINMUX_S1), decode NEC frame. */
static const char s_ir_rx_script[] =
    "-- IR RX cross-test with NEC decode: receives on PA_26 from Dplus TX PA_26.\n"
    "local ir = require(\"ir\")\n"
    "local RX_PIN     = \"PA_26\"\n"
    "local TIMEOUT_MS = 15000\n"
    "\n"
    "-- Decode NEC frame from raw symbol array.\n"
    "-- Returns addr, cmd on success; nil, errmsg on failure.\n"
    "local function nec_decode(syms)\n"
    "    local i = 1\n"
    "    while i <= #syms and syms[i].level == 0 do i = i + 1 end\n"
    "    if i > #syms then return nil, \"no carrier\" end\n"
    "    if math.abs(syms[i].duration_us - 9000) > 2000 then\n"
    "        return nil, string.format(\"lead carrier %d us\", syms[i].duration_us)\n"
    "    end\n"
    "    i = i + 1\n"
    "    if i > #syms or math.abs(syms[i].duration_us - 4500) > 1000 then\n"
    "        return nil, string.format(\"lead space %d us\", syms[i] and syms[i].duration_us or 0)\n"
    "    end\n"
    "    i = i + 1\n"
    "    local bits = 0\n"
    "    for bit = 0, 31 do\n"
    "        if i > #syms then return nil, \"short frame at bit \" .. bit end\n"
    "        i = i + 1\n"
    "        if i > #syms then return nil, \"short frame at bit \" .. bit end\n"
    "        local b = (syms[i].duration_us > 1000) and 1 or 0\n"
    "        bits = bits | (b << bit)\n"
    "        i = i + 1\n"
    "    end\n"
    "    local addr     = bits & 0xFF\n"
    "    local addr_inv = (bits >> 8) & 0xFF\n"
    "    local cmd      = (bits >> 16) & 0xFF\n"
    "    local cmd_inv  = (bits >> 24) & 0xFF\n"
    "    if addr + addr_inv ~= 0xFF then\n"
    "        return nil, string.format(\"addr complement fail: 0x%02X + 0x%02X\", addr, addr_inv)\n"
    "    end\n"
    "    if cmd + cmd_inv ~= 0xFF then\n"
    "        return nil, string.format(\"cmd complement fail: 0x%02X + 0x%02X\", cmd, cmd_inv)\n"
    "    end\n"
    "    return addr, cmd\n"
    "end\n"
    "\n"
    "print(\"[ir_rx] test start - waiting for IR signal (\" .. TIMEOUT_MS / 1000 .. \"s timeout)\")\n"
    "local dev\n"
    "local ok, result = pcall(ir.new, nil, RX_PIN)\n"
    "if not ok then\n"
    "    print(\"[ir_rx] ERROR: ir.new failed: \" .. tostring(result))\n"
    "    return\n"
    "end\n"
    "dev = result\n"
    "\n"
    "local symbols, err = dev:receive(TIMEOUT_MS)\n"
    "if not symbols then\n"
    "    print(\"[ir_rx] receive: \" .. tostring(err))\n"
    "    dev:close()\n"
    "    return\n"
    "end\n"
    "\n"
    "print(string.format(\"[ir_rx] received %d symbols\", #symbols))\n"
    "local addr, cmd = nec_decode(symbols)\n"
    "if addr then\n"
    "    print(string.format(\"[ir_rx] NEC addr=0x%02X cmd=0x%02X\", addr, cmd))\n"
    "    print(\"[ir_rx] success\")\n"
    "else\n"
    "    print(\"[ir_rx] decode error: \" .. tostring(cmd))\n"
    "    for i = 1, math.min(#symbols, 8) do\n"
    "        local s = symbols[i]\n"
    "        print(string.format(\"  [%02d] level=%d duration_us=%d\", i, s.level, s.duration_us))\n"
    "    end\n"
    "end\n"
    "dev:close()\n";

static void write_vfs(const char *path, const char *content)
{
    { FILE *_ck = fopen(path, "r"); if (_ck) { fclose(_ck); return; } }

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

void lua_driver_ir_provision(void)
{
    /* Write scripts to VFS for manual use (REPL / AT command).
     * Do NOT write main.lua — IR tests are not auto-run on boot. */
    write_vfs("vfs:ir_tx_poll.lua", s_ir_tx_poll_script);
    write_vfs("vfs:ir_tx_intr.lua", s_ir_tx_intr_script);
    write_vfs("vfs:ir_rx.lua",      s_ir_rx_script);
}

/* ---- On-demand execution via AT+CLAW=ir,<mode> ---- */

typedef struct {
    const char       *script;
    SemaphoreHandle_t done;
} ir_task_arg_t;

static void ir_lua_task(void *param)
{
    ir_task_arg_t *arg = (ir_task_arg_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[ir] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        if (luaL_loadstring(L, arg->script) != LUA_OK) {
            printf("[ir] parse error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            printf("[ir] runtime error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_close(L);
    }

    xSemaphoreGive(arg->done);
    rtos_task_delete(NULL);
}

/* mode: "tx" or "tx,poll" → poll TX; "tx,intr" → interrupt TX; "rx" → RX. */
void lua_ir_run(const char *mode)
{
    const char *script = NULL;

    if (strcmp(mode, "tx") == 0 || strcmp(mode, "tx,poll") == 0) {
        script = s_ir_tx_poll_script;
    } else if (strcmp(mode, "tx,intr") == 0) {
        script = s_ir_tx_intr_script;
    } else if (strcmp(mode, "rx") == 0) {
        script = s_ir_rx_script;
    } else {
        printf("[ir] unknown mode: %s (use tx, tx,poll, tx,intr, rx)\n", mode);
        return;
    }

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        printf("[ir] semaphore create failed\n");
        return;
    }

    ir_task_arg_t arg = { .script = script, .done = done };

    if (rtos_task_create(NULL, "ir_lua", ir_lua_task, &arg,
                         16384, 1) != RTK_SUCCESS) {
        printf("[ir] task create failed\n");
        vSemaphoreDelete(done);
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}

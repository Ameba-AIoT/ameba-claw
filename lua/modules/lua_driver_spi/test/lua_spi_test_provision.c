/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_spi_test_provision.c — Writes SPI test scripts to VFS on boot.
**
** Scripts are written to VFS but NOT auto-run on boot.
** Trigger via AT command:
**   AT+CLAW=spi,poll    — polling mode test
**   AT+CLAW=spi,intr    — interrupt mode test
**   AT+CLAW=spi,dma     — DMA mode test
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

static const char s_spi_polling_script[] =
    "local spi = require(\"spi\")\n"
    "\n"
    "print(\"[SPI Polling] test start\")\n"
    "\n"
    "local master = spi.new(1, \"PB_8\", \"PB_9\", \"PB_7\", \"PB_10\", 1, {speed = 1000000, bits = 8, mode = 0})\n"
    "local slave  = spi.new(0, \"PA_30\", \"PA_31\", \"PA_29\", \"PB_0\",  0, {speed = 1000000, bits = 8, mode = 0})\n"
    "\n"
    "local master_tx = {0xAA, 0xBB, 0xCC, 0xDD}\n"
    "local slave_tx  = {0x11, 0x22, 0x33, 0x44}\n"
    "\n"
    "slave:write(slave_tx)\n"
    "local master_rx = master:transfer(master_tx, 1000)\n"
    "local slave_rx  = slave:read(4, 100)\n"
    "\n"
    "print(string.format(\"[SPI Polling] master tx: 0x%02X 0x%02X 0x%02X 0x%02X\","
    " master_tx[1], master_tx[2], master_tx[3], master_tx[4]))\n"
    "print(string.format(\"[SPI Polling] slave  tx: 0x%02X 0x%02X 0x%02X 0x%02X\","
    " slave_tx[1], slave_tx[2], slave_tx[3], slave_tx[4]))\n"
    "print(string.format(\"[SPI Polling] master rx: 0x%02X 0x%02X 0x%02X 0x%02X\","
    " master_rx:byte(1) or 0, master_rx:byte(2) or 0,"
    " master_rx:byte(3) or 0, master_rx:byte(4) or 0))\n"
    "print(string.format(\"[SPI Polling] slave  rx: 0x%02X 0x%02X 0x%02X 0x%02X\","
    " slave_rx:byte(1) or 0, slave_rx:byte(2) or 0,"
    " slave_rx:byte(3) or 0, slave_rx:byte(4) or 0))\n"
    "\n"
    "local ok = true\n"
    "for i = 1, 4 do\n"
    "    local m = master_rx:byte(i) or 0\n"
    "    local s = slave_rx:byte(i) or 0\n"
    "    if m ~= slave_tx[i] then\n"
    "        ok = false\n"
    "        print(string.format(\"[FAIL] master rx[%d]: got 0x%02X, expected 0x%02X\","
    " i, m, slave_tx[i]))\n"
    "    end\n"
    "    if s ~= master_tx[i] then\n"
    "        ok = false\n"
    "        print(string.format(\"[FAIL] slave rx[%d]: got 0x%02X, expected 0x%02X\","
    " i, s, master_tx[i]))\n"
    "    end\n"
    "end\n"
    "\n"
    "master:close()\n"
    "slave:close()\n"
    "\n"
    "if ok then\n"
    "    print(\"[SPI Polling] success\")\n"
    "else\n"
    "    print(\"[SPI Polling] fail\")\n"
    "end\n";

static const char s_spi_dma_script[] =
    "local spi = require(\"spi\")\n"
    "\n"
    "print(\"[SPI DMA] test start (256 bytes)\")\n"
    "\n"
    "local master = spi.new(1, \"PB_8\", \"PB_9\", \"PB_7\", \"PB_10\", 1, {speed = 1000000, bits = 8, mode = 0})\n"
    "local slave  = spi.new(0, \"PA_30\", \"PA_31\", \"PA_29\", \"PB_0\",  0, {speed = 1000000, bits = 8, mode = 0})\n"
    "\n"
    "local tx_data = {}\n"
    "for i = 1, 256 do\n"
    "    tx_data[i] = (i - 1) % 256\n"
    "end\n"
    "\n"
    "slave:recv_dma_start(256)\n"
    "master:send_dma(tx_data, 2000)\n"
    "local slave_rx = slave:recv_dma_finish(2000)\n"
    "\n"
    "local fail_cnt = 0\n"
    "for i = 1, 256 do\n"
    "    local s = slave_rx:byte(i) or 0\n"
    "    if s ~= tx_data[i] then\n"
    "        fail_cnt = fail_cnt + 1\n"
    "        if fail_cnt <= 4 then\n"
    "            print(string.format(\"[FAIL] slave rx[%d]: got 0x%02X, expected 0x%02X\","
    " i, s, tx_data[i]))\n"
    "        end\n"
    "    end\n"
    "end\n"
    "\n"
    "master:close()\n"
    "slave:close()\n"
    "\n"
    "if fail_cnt == 0 then\n"
    "    print(\"[SPI DMA] success (256 bytes)\")\n"
    "else\n"
    "    print(string.format(\"[SPI DMA] fail (%d/256 bytes mismatched)\", fail_cnt))\n"
    "end\n";

static const char s_spi_interrupt_script[] =
    "local spi = require(\"spi\")\n"
    "\n"
    "print(\"[SPI Interrupt] test start (256 bytes)\")\n"
    "\n"
    "local master = spi.new(1, \"PB_8\", \"PB_9\", \"PB_7\", \"PB_10\", 1, {speed = 1000000, bits = 8, mode = 0})\n"
    "local slave  = spi.new(0, \"PA_30\", \"PA_31\", \"PA_29\", \"PB_0\",  0, {speed = 1000000, bits = 8, mode = 0})\n"
    "\n"
    "local tx_data = {}\n"
    "for i = 1, 256 do\n"
    "    tx_data[i] = (i - 1) % 256\n"
    "end\n"
    "\n"
    "slave:recv_it_start(256)\n"
    "master:write(tx_data, 2000)\n"
    "local slave_rx = slave:recv_it_finish(2000)\n"
    "\n"
    "local fail_cnt = 0\n"
    "for i = 1, 256 do\n"
    "    local s = slave_rx:byte(i) or 0\n"
    "    if s ~= tx_data[i] then\n"
    "        fail_cnt = fail_cnt + 1\n"
    "        if fail_cnt <= 4 then\n"
    "            print(string.format(\"[FAIL] slave rx[%d]: got 0x%02X, expected 0x%02X\","
    " i, s, tx_data[i]))\n"
    "        end\n"
    "    end\n"
    "end\n"
    "\n"
    "master:close()\n"
    "slave:close()\n"
    "\n"
    "if fail_cnt == 0 then\n"
    "    print(\"[SPI Interrupt] success (256 bytes)\")\n"
    "else\n"
    "    print(string.format(\"[SPI Interrupt] fail (%d/256 bytes mismatched)\", fail_cnt))\n"
    "end\n";

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

void lua_driver_spi_provision(void)
{
    /* Write scripts to VFS for manual use (REPL / AT command).
     * Do NOT write main.lua — SPI tests are not auto-run on boot. */
    write_vfs("vfs:spi_polling.lua",   s_spi_polling_script);
    write_vfs("vfs:spi_dma.lua",       s_spi_dma_script);
    write_vfs("vfs:spi_interrupt.lua", s_spi_interrupt_script);
}

/* ---- On-demand execution via AT+CLAW=spi,<mode> ---- */

typedef struct {
    const char       *script;
    SemaphoreHandle_t done;
} spi_task_arg_t;

static void spi_lua_task(void *param)
{
    spi_task_arg_t *arg = (spi_task_arg_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[spi] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        if (luaL_loadstring(L, arg->script) != LUA_OK) {
            printf("[spi] parse error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            printf("[spi] runtime error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_close(L);
    }

    xSemaphoreGive(arg->done);
    rtos_task_delete(NULL);
}

void lua_spi_run(const char *mode)
{
    const char *script = NULL;

    if (strcmp(mode, "poll") == 0) {
        script = s_spi_polling_script;
    } else if (strcmp(mode, "intr") == 0) {
        script = s_spi_interrupt_script;
    } else if (strcmp(mode, "dma") == 0) {
        script = s_spi_dma_script;
    } else {
        printf("[spi] unknown mode: %s (use poll/intr/dma)\n", mode);
        return;
    }

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        printf("[spi] semaphore create failed\n");
        return;
    }

    spi_task_arg_t arg = { .script = script, .done = done };

    if (rtos_task_create(NULL, "spi_lua_task", spi_lua_task, &arg,
                         16384, 1) != RTK_SUCCESS) {
        printf("[spi] task create failed\n");
        vSemaphoreDelete(done);
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}

/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_spi_test_provision.c — SPI test runners and VFS script provisioning.
**
** Exported:
**   lua_driver_spi_provision() — writes ST7789 library + test script to VFS
**   lua_spi_run(mode)          — AT+CLAW=spi,<mode>
**
** AT commands:
**   AT+CLAW=spi,poll    — polling + slave:write_intr + master:read_intr (single task)
**   AT+CLAW=spi,intr    — master:write_intr + slave:read_intr (two tasks)
**   AT+CLAW=spi,dma     — master:write_dma + slave:read_dma, then slave:write_dma + master:read_dma
**   AT+CLAW=spi,st7789  — ST7789 vertical bars ↔ concentric rects
**
** Hardware (single-board loopback):
**   SPI1 master: SCLK=PB_7, MOSI=PB_8, MISO=PB_9, CS=PB_10
**   SPI0 slave:  SCLK=PA_29, MOSI=PA_30, MISO=PA_31, CS=PB_0
**   ST7789 LCD:  SPI1 SCLK=PB_7, MOSI=PB_8, MISO=nil, CS=PB_10
**                DC=PB_9, RES=PA_25, BLK=PA_26
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

/* Auto-generated C string headers (produced by lua_to_cstr.py at configure time). */
/* NOTE: the pure-Lua lcd_spi_st7789 driver was retired — the on-board ST7789 SPI
 * LCD is driven by the native C `display` module (require("display")). */

/* ------------------------------------------------------------------ */
/* Utilities                                                            */
/* ------------------------------------------------------------------ */

/*
** Register a Lua source chunk into package.preload[name] so a script run
** via luaL_loadstring (which has no filesystem searcher) can require() it.
** Leaves the stack as it found it.
*/
static void preload_lua_lib(lua_State *L, const char *name, const char *src)
{
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");
    if (luaL_loadstring(L, src) != LUA_OK) {
        printf("[spi] preload '%s' parse error: %s\n", name, lua_tostring(L, -1));
        lua_pop(L, 1);
    } else {
        lua_setfield(L, -2, name); /* preload[name] = chunk */
    }
    lua_pop(L, 2); /* preload, package */
}

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

/* ------------------------------------------------------------------ */
/* Provision: write VFS scripts on boot                                 */
/* ------------------------------------------------------------------ */

void lua_driver_spi_provision(void)
{
    /* Nothing to provision: the raw SPI poll/intr/dma tests embed their own
     * scripts; the retired ST7789 LCD test scripts are no longer bundled. */
}

/* ------------------------------------------------------------------ */
/* Poll test — AT+CLAW=spi,poll                                         */
/* Single Lua state, single task.                                       */
/* Tests: write, read (polling FIFO), plus write_intr/read_intr.        */
/* ------------------------------------------------------------------ */

static const char s_spi_poll_script[] =
    "-- AT+CLAW=spi,poll -- single board loopback, polling + partial intr test\n"
    "local MASTER_TX = \"\\x01\\x02\\x03\\x04\\x05\\x06\\x07\\x08\\x09\\x0A\\x0B\\x0C\\x0D\\x0E\\x0F\\x10\"\n"
    "local SLAVE_TX  = \"\\xA1\\xA2\\xA3\\xA4\\xA5\\xA6\\xA7\\xA8\"\n"
    "\n"
    "local m = spi.new(1, \"PB_7\", \"PB_8\", \"PB_9\", \"PB_10\", {div=20})\n"
    "local s = spi.new_slave(0, \"PA_29\", \"PA_30\", \"PA_31\", \"PB_0\")\n"
    "\n"
    "local ok, err = pcall(function()\n"
    "    -- 1. master:write + slave:read (polling FIFO)\n"
    "    m:write(MASTER_TX)\n"
    "    local rx1 = s:read(#MASTER_TX)\n"
    "    assert(rx1 == MASTER_TX, string.format(\"[poll] master->slave mismatch: got %d bytes\", #rx1))\n"
    "    print(\"[poll] 1. write + read OK\")\n"
    "\n"
    "    -- 2. slave:write + master:read (polling preload)\n"
    "    s:write(SLAVE_TX)\n"
    "    local rx2 = m:read(#SLAVE_TX)\n"
    "    assert(rx2 == SLAVE_TX, string.format(\"[poll] slave->master mismatch: got %d bytes\", #rx2))\n"
    "    print(\"[poll] 2. write + read (reverse) OK\")\n"
    "\n"
    "    -- 3. slave:write_intr (preload via ISR) + master:read_intr\n"
    "    s:write_intr(SLAVE_TX)\n"
    "    local rx3 = m:read_intr(#SLAVE_TX)\n"
    "    assert(rx3 == SLAVE_TX, string.format(\"[poll] write_intr+read_intr mismatch: got %d bytes\", #rx3))\n"
    "    print(\"[poll] 3. write_intr + read_intr OK\")\n"
    "\n"
    "    -- 4. re-open: verify resource released\n"
    "    m:close()\n"
    "    s:close()\n"
    "    local m2 = spi.new(1, \"PB_7\", \"PB_8\", \"PB_9\", \"PB_10\")\n"
    "    m2:close()\n"
    "    print(\"[poll] 4. re-open OK\")\n"
    "\n"
    "    -- 5. fully-programmable pinmux + miso=nil (_PNC guard)\n"
    "    -- Force GC so __gc releases all prior refcnts before reopening with different config\n"
    "    m = nil; s = nil; m2 = nil; collectgarbage(\"collect\")\n"
    "    local mfp = spi.new(1, \"PB_7\", \"PB_8\", nil, \"PB_10\", {pinmux=\"full\"})\n"
    "    local sfp = spi.new_slave(0, \"PA_29\", \"PA_30\", \"PA_31\", \"PB_0\", {pinmux=\"full\"})\n"
    "    mfp:write(MASTER_TX)\n"
    "    local rx5 = sfp:read(#MASTER_TX)\n"
    "    assert(rx5 == MASTER_TX, string.format(\"[poll] fp mismatch: got %d bytes\", #rx5))\n"
    "    mfp:close()\n"
    "    sfp:close()\n"
    "    print(\"[poll] 5. fully-prog pinmux + miso=nil OK\")\n"
    "end)\n"
    "\n"
    "if s and not (pcall(function() s:close() end)) then end\n"
    "if m and not (pcall(function() m:close() end)) then end\n"
    "\n"
    "if ok then\n"
    "    print(\"[spi_poll] ALL PASS\")\n"
    "else\n"
    "    print(\"[spi_poll] FAIL: \" .. tostring(err))\n"
    "end\n";

/* ------------------------------------------------------------------ */
/* Intr test — AT+CLAW=spi,intr                                         */
/* Two Lua states, two tasks: master:write_intr + slave:read_intr.      */
/* ------------------------------------------------------------------ */

/* Slave task: waits for master to clock in 16 bytes via write_intr. */
static const char s_spi_intr_slave_script[] =
    "-- SPI0 slave intr rx test\n"
    "local EXPECTED = \"\\x01\\x02\\x03\\x04\\x05\\x06\\x07\\x08\\x09\\x0A\\x0B\\x0C\\x0D\\x0E\\x0F\\x10\"\n"
    "local s = spi.new_slave(0, \"PA_29\", \"PA_30\", \"PA_31\", \"PB_0\")\n"
    "local ok, err = pcall(function()\n"
    "    local rx = s:read_intr(16)\n"
    "    assert(rx == EXPECTED, string.format(\"[intr_slave] got %d bytes, mismatch\", #rx))\n"
    "    print(\"[intr_slave] read_intr OK\")\n"
    "    s:close()\n"
    "end)\n"
    "if ok then print(\"[intr_slave] PASS\") else print(\"[intr_slave] FAIL: \" .. tostring(err)) end\n";

/* Master task: give the slave a moment to arm, then write_intr 16 bytes. */
static const char s_spi_intr_master_script[] =
    "-- SPI1 master intr tx test (start after slave ready)\n"
    "sys.sleep_ms(300)\n"
    "local DATA = \"\\x01\\x02\\x03\\x04\\x05\\x06\\x07\\x08\\x09\\x0A\\x0B\\x0C\\x0D\\x0E\\x0F\\x10\"\n"
    "local m = spi.new(1, \"PB_7\", \"PB_8\", \"PB_9\", \"PB_10\")\n"
    "local ok, err = pcall(function()\n"
    "    m:write_intr(DATA)\n"
    "    print(\"[intr_master] write_intr OK\")\n"
    "    m:close()\n"
    "end)\n"
    "if ok then print(\"[intr_master] PASS\") else print(\"[intr_master] FAIL: \" .. tostring(err)) end\n";

/* Single task: slave:write_intr preload + master:read_intr (slave->master). */
static const char s_spi_intr_single_script[] =
    "-- ISR single-task: slave:write_intr preload + master:read_intr\n"
    "local SLAVE_TX = \"\\xB1\\xB2\\xB3\\xB4\\xB5\\xB6\\xB7\\xB8\\xB9\\xBA\\xBB\\xBC\\xBD\\xBE\\xBF\\xC0\"\n"
    "local m = spi.new(1, \"PB_7\", \"PB_8\", \"PB_9\", \"PB_10\")\n"
    "local s = spi.new_slave(0, \"PA_29\", \"PA_30\", \"PA_31\", \"PB_0\")\n"
    "local ok, err = pcall(function()\n"
    "    s:write_intr(SLAVE_TX)\n"
    "    local rx = m:read_intr(#SLAVE_TX)\n"
    "    assert(rx == SLAVE_TX, string.format(\"[intr_single] mismatch: got %d bytes\", #rx))\n"
    "    print(\"[intr_single] write_intr+read_intr OK\")\n"
    "    m:close()\n"
    "    s:close()\n"
    "end)\n"
    "if ok then print(\"[intr_single] PASS\") else print(\"[intr_single] FAIL: \" .. tostring(err)) end\n";

/* ------------------------------------------------------------------ */
/* DMA test — AT+CLAW=spi,dma                                           */
/* Part A (two tasks): master:write_dma + slave:read_dma (256 bytes).   */
/* Part B (single task): slave:write_dma preload + master:read_dma.     */
/* ------------------------------------------------------------------ */

/* Part A — slave: receives 256 bytes via DMA. */
static const char s_spi_dma_slave_script[] =
    "-- SPI0 slave DMA rx test (256 bytes)\n"
    "local s = spi.new_slave(0, \"PA_29\", \"PA_30\", \"PA_31\", \"PB_0\")\n"
    "local ok, err = pcall(function()\n"
    "    local rx = s:read_dma(256)\n"
    "    local fail = 0\n"
    "    for i = 1, 256 do\n"
    "        if rx:byte(i) ~= (i-1) % 256 then fail = fail + 1 end\n"
    "    end\n"
    "    s:close()\n"
    "    assert(fail == 0, string.format(\"[dma_slave] %d/256 mismatches\", fail))\n"
    "    print(\"[dma_slave] read_dma OK (256 bytes)\")\n"
    "end)\n"
    "if ok then print(\"[dma_slave] PASS\") else print(\"[dma_slave] FAIL: \" .. tostring(err)) end\n";

/* Part A — master: sends 256 bytes via DMA. */
static const char s_spi_dma_master_script[] =
    "-- SPI1 master DMA tx test (256 bytes)\n"
    "sys.sleep_ms(300)\n"
    "local parts = {}\n"
    "for i = 1, 256 do parts[i] = string.char((i-1) % 256) end\n"
    "local tx = table.concat(parts)\n"
    "local m = spi.new(1, \"PB_7\", \"PB_8\", \"PB_9\", \"PB_10\")\n"
    "local ok, err = pcall(function()\n"
    "    m:write_dma(tx)\n"
    "    print(\"[dma_master] write_dma OK (256 bytes)\")\n"
    "    m:close()\n"
    "end)\n"
    "if ok then print(\"[dma_master] PASS\") else print(\"[dma_master] FAIL: \" .. tostring(err)) end\n";

/* Part B — single task: slave preloads TX via DMA, master reads via DMA. */
static const char s_spi_dma_single_script[] =
    "-- DMA single-task: slave:write_dma preload + master:read_dma\n"
    "local SLAVE_TX = \"\\xD1\\xD2\\xD3\\xD4\\xD5\\xD6\\xD7\\xD8\"\n"
    "local m = spi.new(1, \"PB_7\", \"PB_8\", \"PB_9\", \"PB_10\")\n"
    "local s = spi.new_slave(0, \"PA_29\", \"PA_30\", \"PA_31\", \"PB_0\")\n"
    "local ok, err = pcall(function()\n"
    "    s:write_dma(SLAVE_TX)\n"
    "    local rx = m:read_dma(#SLAVE_TX)\n"
    "    assert(rx == SLAVE_TX, string.format(\"[dma_single] mismatch: got %d bytes\", #rx))\n"
    "    print(\"[dma_single] write_dma+read_dma OK\")\n"
    "    m:close()\n"
    "    s:close()\n"
    "end)\n"
    "if ok then print(\"[dma_single] PASS\") else print(\"[dma_single] FAIL: \" .. tostring(err)) end\n";

/* ------------------------------------------------------------------ */
/* C task infrastructure                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    const char       *script;
    const char       *lib_name;   /* if non-NULL, preload this lib before running */
    const char       *lib_src;
    SemaphoreHandle_t done;
} spi_lua_task_arg_t;

static void spi_lua_task(void *param)
{
    spi_lua_task_arg_t *arg = (spi_lua_task_arg_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[spi] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        if (arg->lib_name && arg->lib_src) {
            preload_lua_lib(L, arg->lib_name, arg->lib_src);
        }
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

/*
** Run a single script in a dedicated task and block until done.
** lib_name/lib_src may be NULL if no preload is needed.
*/
static void run_single_task(const char *script,
                             const char *lib_name, const char *lib_src,
                             const char *task_name)
{
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        printf("[spi] semaphore create failed\n");
        return;
    }

    spi_lua_task_arg_t arg = {
        .script   = script,
        .lib_name = lib_name,
        .lib_src  = lib_src,
        .done     = done,
    };

    if (rtos_task_create(NULL, task_name, spi_lua_task, &arg,
                         16384, 1) != RTK_SUCCESS) {
        printf("[spi] task create failed (%s)\n", task_name);
        vSemaphoreDelete(done);
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}

/*
** Run two scripts concurrently (slave first, then master) and block
** until both are done.
*/
static void run_two_tasks(const char *slave_script, const char *master_script)
{
    SemaphoreHandle_t slave_done  = xSemaphoreCreateBinary();
    SemaphoreHandle_t master_done = xSemaphoreCreateBinary();

    if (!slave_done || !master_done) {
        printf("[spi] semaphore create failed\n");
        if (slave_done) {
            vSemaphoreDelete(slave_done);
        }
        if (master_done) {
            vSemaphoreDelete(master_done);
        }
        return;
    }

    spi_lua_task_arg_t slave_arg = {
        .script   = slave_script,
        .lib_name = NULL,
        .lib_src  = NULL,
        .done     = slave_done,
    };
    spi_lua_task_arg_t master_arg = {
        .script   = master_script,
        .lib_name = NULL,
        .lib_src  = NULL,
        .done     = master_done,
    };

    if (rtos_task_create(NULL, "spi_slave_lua", spi_lua_task, &slave_arg,
                         16384, 1) != RTK_SUCCESS) {
        printf("[spi] slave task create failed\n");
        vSemaphoreDelete(slave_done);
        vSemaphoreDelete(master_done);
        return;
    }

    if (rtos_task_create(NULL, "spi_master_lua", spi_lua_task, &master_arg,
                         16384, 1) != RTK_SUCCESS) {
        printf("[spi] master task create failed\n");
        /* slave was already created; wait for it to finish before freeing */
        xSemaphoreTake(slave_done, portMAX_DELAY);
        vSemaphoreDelete(slave_done);
        vSemaphoreDelete(master_done);
        return;
    }

    xSemaphoreTake(slave_done,  portMAX_DELAY);
    xSemaphoreTake(master_done, portMAX_DELAY);

    vSemaphoreDelete(slave_done);
    vSemaphoreDelete(master_done);
}

/* ------------------------------------------------------------------ */
/* AT+CLAW=spi,<mode> dispatcher                                        */
/* ------------------------------------------------------------------ */

void lua_spi_run(const char *mode)
{
    if (strcmp(mode, "poll") == 0) {
        /* Single task: polling write/read + write_intr/read_intr */
        run_single_task(s_spi_poll_script, NULL, NULL, "spi_poll_lua");

    } else if (strcmp(mode, "intr") == 0) {
        /* Part A: two tasks — master:write_intr + slave:read_intr */
        printf("[spi_intr] part A: two-task intr test (master->slave)\n");
        run_two_tasks(s_spi_intr_slave_script, s_spi_intr_master_script);
        printf("[spi_intr] part A done\n");

        /* Part B: single task — slave:write_intr preload + master:read_intr */
        printf("[spi_intr] part B: single-task intr test (slave->master)\n");
        run_single_task(s_spi_intr_single_script, NULL, NULL, "spi_intr_lua");
        printf("[spi_intr] part B done\n");

    } else if (strcmp(mode, "dma") == 0) {
        /* Part A: two tasks — master:write_dma + slave:read_dma (256 bytes) */
        printf("[spi_dma] part A: two-task DMA test (256 bytes)\n");
        run_two_tasks(s_spi_dma_slave_script, s_spi_dma_master_script);
        printf("[spi_dma] part A done\n");

        /* Part B: single task — slave:write_dma preload + master:read_dma */
        printf("[spi_dma] part B: single-task DMA test\n");
        run_single_task(s_spi_dma_single_script, NULL, NULL, "spi_dma_lua");
        printf("[spi_dma] part B done\n");

    } else {
        printf("[spi] unknown mode: %s (use poll/intr/dma)\n", mode);
    }
}

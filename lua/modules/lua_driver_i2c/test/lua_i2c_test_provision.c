/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** lua_i2c_test_provision.c — I2C test runners.
**
** Exported functions:
**   lua_driver_i2c_provision() — writes the sh1106 VFS script on boot
**   lua_i2c_run_sh1106(sx, sy) — AT+CLAW=i2c,sh1106[,sx[,sy]]
**   lua_i2c_run_rw()           — AT+CLAW=i2c,rw  (master, COM6)
**   lua_i2c_run_slave()        — AT+CLAW=i2c,slave (slave, COM13)
**
** Two-board rw test wiring:
**   Both boards: PA_25 = SCL (I2C0), PA_26 = SDA (I2C0)
**   Slave addr: 0x50
**   Step 1: run AT+CLAW=i2c,slave on COM13
**   Step 2: run AT+CLAW=i2c,rw   on COM6
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

/* s_i2c_sh1106_script is auto-generated from i2c_sh1106.lua at cmake configure time. */
#include "i2c_sh1106_lua.h"
/* s_oled_sh1106_lib is the shared SH1106 driver (lib/oled_sh1106.lua). */
#include "oled_sh1106_lua.h"

/* ------------------------------------------------------------------ */
/* SH1106 OLED test (existing)                                         */
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
        printf("[i2c] preload '%s' parse error: %s\n", name, lua_tostring(L, -1));
        lua_pop(L, 1);              /* error message */
    } else {
        lua_setfield(L, -2, name); /* preload[name] = chunk */
    }
    lua_pop(L, 2);                 /* preload, package */
}

void lua_driver_i2c_provision(void)
{
    size_t script_len = strlen(s_i2c_sh1106_script);
    printf("[i2c_provision] script len=%d\n", (int)script_len);

    const char *sh1106_path = "vfs:test_i2c_sh1106.lua";
    { FILE *_ck = fopen(sh1106_path, "r"); if (_ck) { fclose(_ck); return; } }

    FILE *f = fopen(sh1106_path, "w");
    printf("[i2c_provision] fopen(%s) = %p\n", sh1106_path, (void *)f);
    if (f) {
        size_t written = fwrite(s_i2c_sh1106_script, 1, script_len, f);
        printf("[i2c_provision] fwrite sh1106 = %d/%d\n", (int)written, (int)script_len);
        fclose(f);
    }
}

/* Font scale passed from the AT command into the Lua script. 0 = caller did
** not specify that axis; the script applies its own defaults. */
typedef struct {
    SemaphoreHandle_t done;
    int sx;
    int sy;
} sh1106_args_t;

static void sh1106_lua_task(void *param)
{
    sh1106_args_t *a = (sh1106_args_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[i2c] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        preload_lua_lib(L, "oled_sh1106", s_oled_sh1106_lib);
        /* Expose the requested font scale to the script as globals. */
        lua_pushinteger(L, a->sx);
        lua_setglobal(L, "SH1106_SX");
        lua_pushinteger(L, a->sy);
        lua_setglobal(L, "SH1106_SY");
        if (luaL_loadstring(L, s_i2c_sh1106_script) != LUA_OK) {
            printf("[i2c] script parse error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            printf("[i2c] script runtime error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_close(L);
    }

    xSemaphoreGive(a->done);
    rtos_task_delete(NULL);
}

/* sx/sy: requested font scale (0 = unspecified, script picks the default). */
void lua_i2c_run_sh1106(int sx, int sy)
{
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        printf("[i2c] failed to create semaphore\n");
        return;
    }

    /* args lives on this stack frame; safe because we block on `done` below
    ** until the task is finished using it. */
    sh1106_args_t args = { done, sx, sy };

    if (rtos_task_create(NULL, "sh1106_lua", sh1106_lua_task, &args,
                         16384, 1) != RTK_SUCCESS) {
        printf("[i2c] failed to create task\n");
        vSemaphoreDelete(done);
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}

/* ------------------------------------------------------------------ */
/* rw master test — AT+CLAW=i2c,rw (run on COM6)                      */
/* ------------------------------------------------------------------ */
/*
** Verifies the master write / write_byte APIs by closed-loop echo:
** whatever the master puts on the wire is read back from the slave and
** compared byte-for-byte. Neither the driver nor this test touches any
** low-level I2C register — the slave runs entirely on the new
** i2c.new_slave Lua interface (see s_i2c_slave_script below).
**
** Echo handshake (the 0x6969 flag):
**   1) master write({0x69,0x69,n})  -> "echo test: a payload of n bytes
**                                        follows; store it and serve it
**                                        back on my next read"
**   2) master <api-under-test>       -> the real write_byte/write call,
**                                        on the wire as exactly n bytes
**   3) master read(n) / read_byte()  -> slave returns the stored payload
**   4) master asserts received == sent
**
** Putting the length in the flag header lets the slave read exactly n
** bytes and reply immediately — no STOP_DET polling, no FIFO guessing.
**
** Two echo sub-protocols, selected by the op byte in the 4-byte header
** {0x69, 0x69, op, n}:
**   op=0 write-echo : master writes n bytes, slave stores then serves them
**                     back; master reads them with a NON-mem read. Verifies
**                     the write_* APIs and the plain (no-mem) read APIs.
**   op=1 read-mem   : master issues read_byte(mem) / read(n,mem) — the
**                     I2C_MasterRepeatRead path (write mem byte, repeated
**                     START, read n). The slave reads the 1 mem byte then
**                     serves the deterministic pattern (mem+i)&0xFF, which
**                     the master asserts. Verifies the mem-addressed reads.
**   op=2 sentinel   : end of test.
**
** APIs covered (every cfunction registered in lua_driver_i2c.c):
**   module : i2c.new / i2c.new_slave
**   bus    : scan / device / close
**   dev    : address / close
**            write_byte / write_byte(val,mem)
**            write(table) / write(string) / write(table,mem) / write(string,mem)
**            read_byte() / read(len) / read_byte(mem) / read(len,mem)
**   slave  : address / read(len) / read(len,timeout) / write / close
*/
static const char s_i2c_rw_script[] =
    "-- AT+CLAW=i2c,rw\n"
    "-- Master test: I2C0, PA_26=SDA, PA_25=SCL, slave at 0x50\n"
    "-- Run slave board first: AT+CLAW=i2c,slave on COM13\n"
    "-- Header {0x69,0x69,op,n}:  op=0 write-echo  op=1 read-mem  op=2 stop\n"
    "local SLAVE = 0x50\n"
    "\n"
    "local ok, err = pcall(function()\n"
    "    print(\"[i2c_rw] start: PA_26=SDA PA_25=SCL slave=0x50\")\n"
    "\n"
    "    -- 1. new\n"
    "    local bus = i2c.new(0, \"PA_26\", \"PA_25\", 100000)\n"
    "    print(\"[i2c_rw] 1. new OK\")\n"
    "\n"
    "    -- 2. scan\n"
    "    local found = bus:scan()\n"
    "    local found_slave = false\n"
    "    for i = 1, #found do\n"
    "        if found[i] == SLAVE then found_slave = true end\n"
    "    end\n"
    "    assert(found_slave, string.format(\"scan: slave 0x%02X not found\", SLAVE))\n"
    "    print(string.format(\"[i2c_rw] 2. scan OK: found 0x%02X\", SLAVE))\n"
    "\n"
    "    -- 3. device + address\n"
    "    local dev = bus:device(SLAVE)\n"
    "    assert(dev:address() == SLAVE, \"address() mismatch\")\n"
    "    print(\"[i2c_rw] 3. device + address OK\")\n"
    "\n"
    "    -- arm(op,n): 4-byte header announcing the next sub-transaction.\n"
    "    local function arm(op, n) dev:write({0x69, 0x69, op, n}) end\n"
    "\n"
    "    -- ===== write-echo cases (op=0): read back with a NON-mem read =====\n"
    "    -- 4. write_byte(0xAB) -> wire {0xAB} -> echo read_byte() -> assert\n"
    "    arm(0, 1)\n"
    "    dev:write_byte(0xAB)\n"
    "    local e4 = dev:read_byte()\n"
    "    assert(e4 == 0xAB,\n"
    "        string.format(\"echo write_byte: got 0x%02X expected 0xAB\", e4))\n"
    "    print(string.format(\"[i2c_rw] 4. write_byte(0xAB) echo=0x%02X OK\", e4))\n"
    "\n"
    "    -- 5. write_byte(0xCD, mem=0x10) -> wire {0x10,0xCD} -> echo read(2)\n"
    "    arm(0, 2)\n"
    "    dev:write_byte(0xCD, 0x10)\n"
    "    local e5 = dev:read(2)\n"
    "    assert(#e5 == 2 and e5:byte(1) == 0x10 and e5:byte(2) == 0xCD,\n"
    "        string.format(\"echo write_byte+mem: got 0x%02X 0x%02X\",\n"
    "            e5:byte(1), e5:byte(2)))\n"
    "    print(string.format(\"[i2c_rw] 5. write_byte(0xCD,mem=0x10) echo=[0x%02X,0x%02X] OK\",\n"
    "        e5:byte(1), e5:byte(2)))\n"
    "\n"
    "    -- 6. write(table) -> wire {0x11,0x22,0x33} -> echo read(3)\n"
    "    arm(0, 3)\n"
    "    dev:write({0x11, 0x22, 0x33})\n"
    "    local e6 = dev:read(3)\n"
    "    assert(#e6 == 3 and e6:byte(1) == 0x11\n"
    "           and e6:byte(2) == 0x22 and e6:byte(3) == 0x33,\n"
    "        string.format(\"echo write(table): got 0x%02X 0x%02X 0x%02X\",\n"
    "            e6:byte(1), e6:byte(2), e6:byte(3)))\n"
    "    print(\"[i2c_rw] 6. write(table) echo=[0x11,0x22,0x33] OK\")\n"
    "\n"
    "    -- 7. write(string) -> wire 'Hello' -> echo read(5)\n"
    "    arm(0, 5)\n"
    "    dev:write(\"Hello\")\n"
    "    local e7 = dev:read(5)\n"
    "    assert(e7 == \"Hello\",\n"
    "        string.format(\"echo write(string): got %q\", e7))\n"
    "    print(\"[i2c_rw] 7. write(string) echo=Hello OK\")\n"
    "\n"
    "    -- 8. write(table,mem=0x20) -> wire {0x20,0xAA,0xBB} -> echo read(3)\n"
    "    arm(0, 3)\n"
    "    dev:write({0xAA, 0xBB}, 0x20)\n"
    "    local e8 = dev:read(3)\n"
    "    assert(#e8 == 3 and e8:byte(1) == 0x20\n"
    "           and e8:byte(2) == 0xAA and e8:byte(3) == 0xBB,\n"
    "        string.format(\"echo write(data,mem): got 0x%02X 0x%02X 0x%02X\",\n"
    "            e8:byte(1), e8:byte(2), e8:byte(3)))\n"
    "    print(\"[i2c_rw] 8. write(table,mem=0x20) echo=[0x20,0xAA,0xBB] OK\")\n"
    "\n"
    "    -- 9. write(string,mem=0x40) -> wire {0x40,'H','i'} -> echo read(3)\n"
    "    arm(0, 3)\n"
    "    dev:write(\"Hi\", 0x40)\n"
    "    local e9 = dev:read(3)\n"
    "    assert(#e9 == 3 and e9:byte(1) == 0x40\n"
    "           and e9:byte(2) == string.byte(\"H\")\n"
    "           and e9:byte(3) == string.byte(\"i\"),\n"
    "        string.format(\"echo write(string,mem): got 0x%02X 0x%02X 0x%02X\",\n"
    "            e9:byte(1), e9:byte(2), e9:byte(3)))\n"
    "    print(\"[i2c_rw] 9. write(string,mem=0x40) echo=[0x40,'H','i'] OK\")\n"
    "\n"
    "    -- ===== read-mem cases (op=1): I2C_MasterRepeatRead path =====\n"
    "    -- slave serves pattern (mem+i)&0xFF; master asserts.\n"
    "    -- 10. read_byte(mem=0x55) -> wire write{0x55}+read(1) -> {0x55}\n"
    "    arm(1, 1)\n"
    "    local r10 = dev:read_byte(0x55)\n"
    "    assert(r10 == 0x55,\n"
    "        string.format(\"read_byte(mem): got 0x%02X expected 0x55\", r10))\n"
    "    print(string.format(\"[i2c_rw] 10. read_byte(mem=0x55)=0x%02X OK\", r10))\n"
    "\n"
    "    -- 11. read(4,mem=0x30) -> wire write{0x30}+read(4) -> {0x30..0x33}\n"
    "    arm(1, 4)\n"
    "    local r11 = dev:read(4, 0x30)\n"
    "    assert(#r11 == 4 and r11:byte(1) == 0x30 and r11:byte(2) == 0x31\n"
    "           and r11:byte(3) == 0x32 and r11:byte(4) == 0x33,\n"
    "        string.format(\"read(len,mem): got 0x%02X 0x%02X 0x%02X 0x%02X\",\n"
    "            r11:byte(1), r11:byte(2), r11:byte(3), r11:byte(4)))\n"
    "    print(\"[i2c_rw] 11. read(4,mem=0x30) echo=[0x30,0x31,0x32,0x33] OK\")\n"
    "\n"
    "    -- 12. tell the slave the test is over (op=2 sentinel)\n"
    "    arm(2, 0)\n"
    "    print(\"[i2c_rw] 12. end-of-test sentinel sent\")\n"
    "\n"
    "    -- 13. close\n"
    "    dev:close()\n"
    "    bus:close()\n"
    "    print(\"[i2c_rw] 13. close OK\")\n"
    "\n"
    "    -- 14. re-open: verify resource released\n"
    "    local bus2 = i2c.new(0, \"PA_26\", \"PA_25\", 100000)\n"
    "    local dev2 = bus2:device(0x48)\n"
    "    assert(dev2:address() == 0x48, \"re-open: address() mismatch\")\n"
    "    dev2:close()\n"
    "    bus2:close()\n"
    "    print(\"[i2c_rw] 14. re-open OK\")\n"
    "end)\n"
    "\n"
    "if ok then\n"
    "    print(\"[i2c_rw] ALL PASS\")\n"
    "else\n"
    "    print(\"[i2c_rw] FAIL: \" .. tostring(err))\n"
    "end\n";

typedef struct {
    const char       *script;
    SemaphoreHandle_t done;
} i2c_lua_task_arg_t;

static void i2c_rw_lua_task(void *param)
{
    i2c_lua_task_arg_t *arg = (i2c_lua_task_arg_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[i2c_rw] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        if (luaL_loadstring(L, arg->script) != LUA_OK) {
            printf("[i2c_rw] parse error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            printf("[i2c_rw] runtime error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_close(L);
    }

    xSemaphoreGive(arg->done);
    rtos_task_delete(NULL);
}

void lua_i2c_run_rw(void)
{
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        printf("[i2c_rw] semaphore create failed\n");
        return;
    }

    i2c_lua_task_arg_t arg = { .script = s_i2c_rw_script, .done = done };

    if (rtos_task_create(NULL, "i2c_rw_lua", i2c_rw_lua_task, &arg,
                         16384, 1) != RTK_SUCCESS) {
        printf("[i2c_rw] task create failed\n");
        vSemaphoreDelete(done);
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}

/* ------------------------------------------------------------------ */
/* slave — AT+CLAW=i2c,slave (run on COM13 FIRST)                     */
/* ------------------------------------------------------------------ */
/*
** I2C slave at address 0x50, I2C0, PA_25=SCL, PA_26=SDA, driven purely by
** the i2c.new_slave Lua interface — no IC_STATUS / IC_DATA_CMD / STOP_DET
** register access anywhere.
**
** Echo service loop (the 0x6969 flag, see s_i2c_rw_script for the master
** side):
**   s:read(4) -> {0x69, 0x69, op, n}   4-byte header
**     op == 2  -> end-of-test sentinel, stop
**     op == 0  -> write-echo: s:read(n) the payload (the master's write
**                 under test), then s:write(payload) so the master can read
**                 it back and compare.
**     op == 1  -> read-mem: s:read(1) the mem byte, then s:write() the
**                 deterministic pattern (mem+i)&0xFF for the master to assert.
**   The op/length in the header tell the slave exactly what to do — no
**   transaction-type guessing, no register polling.
*/
static const char s_i2c_slave_script[] =
    "-- AT+CLAW=i2c,slave\n"
    "-- Echo slave: I2C0, PA_26=SDA, PA_25=SCL, addr 0x50\n"
    "local SLAVE = 0x50\n"
    "local s = i2c.new_slave(0, \"PA_26\", \"PA_25\", SLAVE, 100000)\n"
    "assert(s:address() == SLAVE,\n"
    "    string.format(\"slave address() got 0x%02X expected 0x%02X\", s:address(), SLAVE))\n"
    "print(string.format(\"[i2c_slave] ready: addr=0x%02X PA_25=SCL PA_26=SDA (lua slave api)\",\n"
    "    s:address()))\n"
    "print(\"[i2c_slave] now run AT+CLAW=i2c,rw on the master board\")\n"
    "\n"
    "local ok, err = pcall(function()\n"
    "    while true do\n"
    "        -- wait up to 60 s for the next 4-byte {0x69,0x69,op,n} header\n"
    "        local hdr = s:read(4, 60000)\n"
    "        if #hdr < 4 then\n"
    "            print(\"[i2c_slave] idle timeout, stop\")\n"
    "            break\n"
    "        end\n"
    "        if hdr:byte(1) ~= 0x69 or hdr:byte(2) ~= 0x69 then\n"
    "            print(string.format(\"[i2c_slave] bad header 0x%02X 0x%02X (skip)\",\n"
    "                hdr:byte(1), hdr:byte(2)))\n"
    "        else\n"
    "            local op = hdr:byte(3)\n"
    "            local n  = hdr:byte(4)\n"
    "            if op == 2 then\n"
    "                print(\"[i2c_slave] end-of-test sentinel, stop\")\n"
    "                break\n"
    "            elseif op == 0 then\n"
    "                -- write-echo: read n bytes from master, serve them back\n"
    "                local payload = s:read(n)\n"
    "                local sent = s:write(payload)\n"
    "                print(string.format(\"[i2c_slave] echo %d/%d bytes\", sent, n))\n"
    "            elseif op == 1 then\n"
    "                -- read-mem: master writes 1 mem byte then reads n bytes;\n"
    "                -- serve deterministic pattern (mem+i)&0xFF for assert.\n"
    "                local mem = s:read(1):byte(1)\n"
    "                local t = {}\n"
    "                for i = 1, n do t[i] = (mem + (i - 1)) % 256 end\n"
    "                local sent = s:write(t)\n"
    "                print(string.format(\"[i2c_slave] read-mem mem=0x%02X served %d/%d bytes\",\n"
    "                    mem, sent, n))\n"
    "            else\n"
    "                print(string.format(\"[i2c_slave] unknown op %d (skip)\", op))\n"
    "            end\n"
    "        end\n"
    "    end\n"
    "end)\n"
    "\n"
    "s:close()\n"
    "if ok then\n"
    "    print(\"[i2c_slave] done\")\n"
    "else\n"
    "    print(\"[i2c_slave] error: \" .. tostring(err))\n"
    "end\n";

void lua_i2c_run_slave(void)
{
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        printf("[i2c_slave] semaphore create failed\n");
        return;
    }

    i2c_lua_task_arg_t arg = { .script = s_i2c_slave_script, .done = done };

    if (rtos_task_create(NULL, "i2c_slave_lua", i2c_rw_lua_task, &arg,
                         16384, 1) != RTK_SUCCESS) {
        printf("[i2c_slave] task create failed\n");
        vSemaphoreDelete(done);
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}

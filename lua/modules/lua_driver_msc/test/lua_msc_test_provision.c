/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 * SPDX-License-Identifier: Apache-2.0
 */
/*
** lua_msc_test_provision.c — USB MSC FAT32 file operation test for ameba_claw.
**
** Trigger via AT command:
**   AT+CLAW=usb,msc    — init MSC host, mount FAT32 U-disk, create/write/read/delete a file
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

LUAMOD_API int luaopen_usb_msc(lua_State *L);

static const char s_msc_test_script[] =
    "local msc = require(\"usb_msc\")\n"
    "\n"
    "print(\"[MSC] initializing USB MSC host...\")\n"
    "local ok, err = msc.init()\n"
    "if not ok then\n"
    "    print(\"[FAIL] init: \" .. tostring(err))\n"
    "    return\n"
    "end\n"
    "print(\"[MSC] init ok, waiting for U-disk ready (10s)...\")\n"
    "\n"
    "local ready, werr = msc.wait_ready(10000)\n"
    "if not ready then\n"
    "    print(\"[FAIL] U-disk not ready: \" .. tostring(werr))\n"
    "    msc.deinit()\n"
    "    return\n"
    "end\n"
    "print(\"[MSC] U-disk ready\")\n"
    "\n"
    "print(\"[MSC] mounting FAT32...\")\n"
    "local drive, merr = msc.mount()\n"
    "if not drive then\n"
    "    print(\"[FAIL] mount: \" .. tostring(merr))\n"
    "    msc.deinit()\n"
    "    return\n"
    "end\n"
    "print(\"[MSC] mounted at \" .. drive)\n"
    "\n"
    "local test_path = drive .. \"ameba_test.txt\"\n"
    "local test_data = \"Hello from Ameba RTL8721F!\\nUSB MSC FAT32 test OK.\\n\"\n"
    "\n"
    "-- step 1: write\n"
    "print(\"[MSC] creating and writing \" .. test_path .. \" ...\")\n"
    "local wok, werr = msc.write_file(test_path, test_data)\n"
    "if not wok then\n"
    "    print(\"[FAIL] write: \" .. tostring(werr))\n"
    "    msc.umount()\n"
    "    msc.deinit()\n"
    "    return\n"
    "end\n"
    "print(\"[MSC] write ok (\" .. #test_data .. \" bytes)\")\n"
    "\n"
    "-- step 2: read back and verify\n"
    "print(\"[MSC] reading \" .. test_path .. \" ...\")\n"
    "local rdata, rerr = msc.read_file(test_path)\n"
    "if not rdata then\n"
    "    print(\"[FAIL] read: \" .. tostring(rerr))\n"
    "    msc.umount()\n"
    "    msc.deinit()\n"
    "    return\n"
    "end\n"
    "print(\"[MSC] read ok (\" .. #rdata .. \" bytes)\")\n"
    "if rdata == test_data then\n"
    "    print(\"[MSC] content verified OK\")\n"
    "else\n"
    "    print(\"[FAIL] content mismatch!\")\n"
    "    print(\"  expected: \" .. test_data)\n"
    "    print(\"  got:      \" .. rdata)\n"
    "    msc.umount()\n"
    "    msc.deinit()\n"
    "    return\n"
    "end\n"
    "\n"
    "-- step 3: delete\n"
    "print(\"[MSC] deleting \" .. test_path .. \" ...\")\n"
    "local dok, derr = msc.remove(test_path)\n"
    "if not dok then\n"
    "    print(\"[FAIL] remove: \" .. tostring(derr))\n"
    "    msc.umount()\n"
    "    msc.deinit()\n"
    "    return\n"
    "end\n"
    "print(\"[MSC] delete ok\")\n"
    "\n"
    "msc.umount()\n"
    "msc.deinit()\n"
    "print(\"[MSC] PASS\")\n";

typedef struct {
    const char       *script;
    SemaphoreHandle_t done;
} msc_task_arg_t;

static void msc_lua_task(void *param)
{
    msc_task_arg_t *arg = (msc_task_arg_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[msc] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        luaL_requiref(L, "usb_msc", luaopen_usb_msc, 1); lua_pop(L, 1);
        if (luaL_loadstring(L, arg->script) != LUA_OK) {
            printf("[msc] parse error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            printf("[msc] runtime error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_close(L);
    }

    xSemaphoreGive(arg->done);
    rtos_task_delete(NULL);
}

void lua_msc_run(void)
{
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        printf("[msc] semaphore create failed\n");
        return;
    }

    msc_task_arg_t arg = { .script = s_msc_test_script, .done = done };

    /* 16KB stack: USB MSC host + Lua + FatFS buffers */
    if (rtos_task_create(NULL, "msc_lua_task", msc_lua_task, &arg,
                         16384, 1) != RTK_SUCCESS) {
        printf("[msc] task create failed\n");
        vSemaphoreDelete(done);
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}

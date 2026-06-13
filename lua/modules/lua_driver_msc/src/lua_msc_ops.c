/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 * SPDX-License-Identifier: Apache-2.0
 */
/*
** lua_msc_ops.c — Individual USB MSC file operations for AT+CLAW=usb,<op> commands.
**
**   AT+CLAW=usb,list[,<path>]          List directory (default: root of U-disk)
**   AT+CLAW=usb,write,<path>,<data>    Create/overwrite file with data
**   AT+CLAW=usb,read,<path>            Read and print file content
**   AT+CLAW=usb,delete,<path>          Delete file
**
** Each runner reuses the persistent C-level MSC state in luusb_msc.c:
** init/mount are no-ops on subsequent calls so operations are fast after first use.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ameba_soc.h"
#include "os_wrapper.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

LUAMOD_API int luaopen_usb_msc(lua_State *L);

/* Shared init+mount preamble injected before every operation script */
#define MSC_PREAMBLE \
    "local msc = require(\"usb_msc\")\n" \
    "local ok, err = msc.init()\n" \
    "if not ok then print(\"[MSC] init error: \" .. tostring(err)) return end\n" \
    "local rdy, rerr = msc.wait_ready(10000)\n" \
    "if not rdy then print(\"[MSC] not ready: \" .. tostring(rerr)) return end\n" \
    "local drv, merr = msc.mount()\n" \
    "if not drv then print(\"[MSC] mount error: \" .. tostring(merr)) return end\n"

/* ---- list ---- */
static const char s_list_script[] =
    MSC_PREAMBLE
    "local path = _path ~= \"\" and _path or drv\n"
    "local entries, err2 = msc.list_dir(path)\n"
    "if not entries then print(\"[MSC] list error: \" .. tostring(err2)) return end\n"
    "print(string.format(\"[MSC] list %s (%d entries)\", path, #entries))\n"
    "for _, e in ipairs(entries) do\n"
    "    local t = e.is_dir and \"DIR \" or \"FILE\"\n"
    "    print(string.format(\"  %s  %-32s  %d\", t, e.name, e.size))\n"
    "end\n"
    "print(\"[MSC] list OK\")\n";

/* ---- write ---- */
static const char s_write_script[] =
    MSC_PREAMBLE
    "local wok, werr = msc.write_file(_path, _data)\n"
    "if not wok then\n"
    "    print(\"[MSC] write error: \" .. tostring(werr))\n"
    "else\n"
    "    print(string.format(\"[MSC] write OK: %s (%d bytes)\", _path, #_data))\n"
    "end\n";

/* ---- read ---- */
static const char s_read_script[] =
    MSC_PREAMBLE
    "local data, rerr = msc.read_file(_path)\n"
    "if not data then\n"
    "    print(\"[MSC] read error: \" .. tostring(rerr))\n"
    "else\n"
    "    print(string.format(\"[MSC] read OK: %s (%d bytes)\", _path, #data))\n"
    "    print(\"[MSC] content:\")\n"
    "    print(data)\n"
    "end\n";

/* ---- delete ---- */
static const char s_delete_script[] =
    MSC_PREAMBLE
    "local dok, derr = msc.remove(_path)\n"
    "if not dok then\n"
    "    print(\"[MSC] delete error: \" .. tostring(derr))\n"
    "else\n"
    "    print(\"[MSC] delete OK: \" .. _path)\n"
    "end\n";

/* ---- runner infrastructure ---- */

typedef struct {
    const char       *script;
    const char       *path;
    const char       *data;   /* NULL for non-write ops */
    SemaphoreHandle_t done;
} msc_op_arg_t;

static void msc_op_task(void *param)
{
    msc_op_arg_t *arg = (msc_op_arg_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[msc] lua state alloc failed\n");
    } else {
        luaL_openlibs(L);
        luaL_requiref(L, "usb_msc", luaopen_usb_msc, 1); lua_pop(L, 1);

        lua_pushstring(L, arg->path ? arg->path : "");
        lua_setglobal(L, "_path");
        lua_pushstring(L, arg->data ? arg->data : "");
        lua_setglobal(L, "_data");

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

static void msc_op_run(const char *script, const char *path, const char *data)
{
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) { printf("[msc] semaphore alloc failed\n"); return; }

    msc_op_arg_t arg = { .script = script, .path = path, .data = data, .done = done };

    if (rtos_task_create(NULL, "msc_op", msc_op_task, &arg, 16384, 1) != RTK_SUCCESS) {
        printf("[msc] task create failed\n");
        vSemaphoreDelete(done);
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}

/* ---- public entry points ---- */

void lua_msc_list_run(const char *path)
{
    msc_op_run(s_list_script, path ? path : "", NULL);
}

void lua_msc_write_run(const char *path, const char *data)
{
    msc_op_run(s_write_script, path, data);
}

void lua_msc_read_run(const char *path)
{
    msc_op_run(s_read_script, path, NULL);
}

void lua_msc_delete_run(const char *path)
{
    msc_op_run(s_delete_script, path, NULL);
}

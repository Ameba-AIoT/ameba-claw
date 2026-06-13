/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 * SPDX-License-Identifier: Apache-2.0
 */
/*
** lua_uvc_test_provision.c — USB UVC capture test for ameba_claw.
**
** Trigger via AT command:
**   AT+CLAW=usb,uvc    — init UVC, capture one JPEG frame, save to vfs:capture.jpg
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

LUAMOD_API int luaopen_usb_uvc(lua_State *L);

static const char s_uvc_test_script[] =
    "local uvc = require(\"usb_uvc\")\n"
    "\n"
    "print(\"[UVC] initializing USB UVC host...\")\n"
    "local ok, err = uvc.init()\n"
    "if not ok then\n"
    "    print(\"[FAIL] init: \" .. tostring(err))\n"
    "    return\n"
    "end\n"
    "print(\"[UVC] init ok, waiting for camera ready (10s)...\")\n"
    "\n"
    "local ready, reason = uvc.wait_ready(10000)\n"
    "if not ready then\n"
    "    print(\"[FAIL] camera not ready: \" .. tostring(reason))\n"
    "    uvc.deinit()\n"
    "    return\n"
    "end\n"
    "print(\"[UVC] camera ready\")\n"
    "\n"
    "print(\"[UVC] set_param 640x480 MJPEG 15fps...\")\n"
    "local ok2, err2 = uvc.set_param({width=640, height=480, fps=15,\n"
    "                                  format=\"mjpeg\", buf_size=153600})\n"
    "if not ok2 then\n"
    "    print(\"[FAIL] set_param: \" .. tostring(err2))\n"
    "    uvc.deinit()\n"
    "    return\n"
    "end\n"
    "print(\"[UVC] params ok\")\n"
    "\n"
    "print(\"[UVC] stream on...\")\n"
    "local ok3, err3 = uvc.stream_on()\n"
    "if not ok3 then\n"
    "    print(\"[FAIL] stream_on: \" .. tostring(err3))\n"
    "    uvc.deinit()\n"
    "    return\n"
    "end\n"
    "\n"
    "print(\"[UVC] capturing frame...\")\n"
    "local frame, ferr = uvc.get_frame(5000)\n"
    "if not frame then\n"
    "    print(\"[FAIL] get_frame: \" .. tostring(ferr))\n"
    "    uvc.stream_off()\n"
    "    uvc.deinit()\n"
    "    return\n"
    "end\n"
    "print(string.format(\"[UVC] frame: %d bytes\", #frame))\n"
    "local f = io.open(\"vfs:capture.jpg\", \"wb\")\n"
    "if not f then\n"
    "    print(\"[FAIL] cannot open vfs:capture.jpg for writing\")\n"
    "    uvc.stream_off()\n"
    "    uvc.deinit()\n"
    "    return\n"
    "end\n"
    "f:write(frame)\n"
    "f:close()\n"
    "print(\"[UVC] frame saved to vfs:capture.jpg\")\n"
    "\n"
    "uvc.stream_off()\n"
    "uvc.deinit()\n"
    "print(\"[UVC] PASS\")\n";

typedef struct {
    const char       *script;
    SemaphoreHandle_t done;
} uvc_task_arg_t;

static void uvc_lua_task(void *param)
{
    uvc_task_arg_t *arg = (uvc_task_arg_t *)param;

    lua_State *L = luaL_newstate();
    if (!L) {
        printf("[uvc] failed to create Lua state\n");
    } else {
        luaL_openlibs(L);
        /* Ensure usb_uvc is available regardless of stored module_mask */
        luaL_requiref(L, "usb_uvc", luaopen_usb_uvc, 1); lua_pop(L, 1);
        if (luaL_loadstring(L, arg->script) != LUA_OK) {
            printf("[uvc] parse error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            printf("[uvc] runtime error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_close(L);
    }

    xSemaphoreGive(arg->done);
    rtos_task_delete(NULL);
}

void lua_uvc_run(void)
{
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        printf("[uvc] semaphore create failed\n");
        return;
    }

    uvc_task_arg_t arg = { .script = s_uvc_test_script, .done = done };

    /* 20KB stack: USB UVC host + Lua + JPEG frame buffer references */
    if (rtos_task_create(NULL, "uvc_lua_task", uvc_lua_task, &arg,
                         20480, 1) != RTK_SUCCESS) {
        printf("[uvc] task create failed\n");
        vSemaphoreDelete(done);
        return;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}

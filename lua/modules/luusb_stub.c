/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * luusb_stub.c — stub implementations for USB Lua modules when
 * CONFIG_USB_DRD_EN is not set.  Provides luaopen_usb_uvc and
 * luaopen_usb_msc so linit.c links without the real USB drivers.
 */

#include "lua.h"
#include "lauxlib.h"

int luaopen_usb_uvc(lua_State *L)
{
    lua_newtable(L);
    return 1;
}

int luaopen_usb_msc(lua_State *L)
{
    lua_newtable(L);
    return 1;
}

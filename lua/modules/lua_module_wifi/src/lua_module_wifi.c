/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua.h"
#include "lauxlib.h"
#include "wifi_api.h"
#include "lwip_netconf.h"
#include <string.h>
#include <stdio.h>

static int lua_wifi_connect(lua_State *L)
{
    const char *ssid     = luaL_checkstring(L, 1);
    const char *password = luaL_checkstring(L, 2);

    struct rtw_network_info param = {0};

    size_t ssid_len = strlen(ssid);
    if (ssid_len > 32) ssid_len = 32;
    memcpy(param.ssid.val, ssid, ssid_len);
    param.ssid.len      = (u8)ssid_len;
    param.password      = (u8 *)password;
    param.password_len  = (s32)strlen(password);

    printf("[wifi] Connecting to '%s' ...\n", ssid);
    s32 ret = wifi_connect(&param, 1);
    if (ret != RTK_SUCCESS) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "wifi_connect failed: %d", (int)ret);
        return 2;
    }

    printf("[wifi] Connected. Requesting DHCP...\n");
    int dhcp = lwip_dhcp(NETIF_WLAN_STA_INDEX, DHCP_START);
    if (dhcp != DHCP_ADDRESS_ASSIGNED) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "DHCP failed");
        return 2;
    }

    printf("[wifi] IP assigned.\n");
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_wifi_status(lua_State *L)
{
    int ok = (lwip_check_connectivity(NETIF_WLAN_STA_INDEX) == CONNECTION_VALID);
    lua_pushboolean(L, ok);
    return 1;
}

static const luaL_Reg wifi_lib[] = {
    {"connect", lua_wifi_connect},
    {"status",  lua_wifi_status},
    {NULL, NULL}
};

LUAMOD_API int luaopen_wifi(lua_State *L)
{
    luaL_newlib(L, wifi_lib);
    return 1;
}

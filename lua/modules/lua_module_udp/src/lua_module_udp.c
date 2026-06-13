/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
** luudp.c — Lua UDP socket module for Ameba RTOS.
**
** Provides require("udp"):
**   sock = udp.open(host, port)   → integer fd, or nil, errmsg
**   ok   = udp.send(sock, data)   → true, or false, errmsg
**          udp.close(sock)
**
** The returned sock is just the lwIP socket fd (an integer), so it can be
** embedded directly in timer code strings passed to timer.start().
**
** UDP socket is "connected" via lwip_connect() so plain lwip_send() works
** without needing to pass a destination address on every call.
*/

#define LUA_LIB
#include "lua.h"
#include "lauxlib.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "ameba_soc.h"
#include <string.h>
#include <errno.h>

/* ---- udp.open(host, port) → sock_id | nil, errmsg ----------------------- */

static int ludp_open(lua_State *L)
{
    const char *host = luaL_checkstring(L, 1);
    int         port = (int)luaL_checkinteger(L, 2);

    int fd = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "udp.open: socket() failed");
        return 2;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_aton(host, &addr.sin_addr) == 0) {
        lwip_close(fd);
        lua_pushnil(L);
        lua_pushfstring(L, "udp.open: invalid address '%s'", host);
        return 2;
    }

    if (lwip_connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        lwip_close(fd);
        lua_pushnil(L);
        lua_pushstring(L, "udp.open: connect() failed");
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)fd);
    return 1;
}

/* ---- udp.send(sock_id, data) → true | false, errmsg --------------------- */

static int ludp_send(lua_State *L)
{
    int         fd  = (int)luaL_checkinteger(L, 1);
    size_t      len;
    const char *data = luaL_checklstring(L, 2, &len);

    if (lwip_send(fd, data, len, 0) < 0) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "udp.send: failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* ---- udp.close(sock_id) -------------------------------------------------- */

static int ludp_close(lua_State *L)
{
    int fd = (int)luaL_checkinteger(L, 1);
    lwip_close(fd);
    lua_pushboolean(L, 1);
    return 1;
}

/* ---- udp.bind(port) → sock_id | nil, errmsg ----------------------------- */

static int ludp_bind(lua_State *L)
{
    int port = (int)luaL_checkinteger(L, 1);

    int fd = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "udp.bind: socket() failed");
        return 2;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (lwip_bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        lwip_close(fd);
        lua_pushnil(L);
        lua_pushfstring(L, "udp.bind: bind() failed on port %d", port);
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)fd);
    return 1;
}

/* ---- udp.recv(sock_id [, maxlen [, timeout_ms]]) → data, ip, port | nil - */
/* timeout_ms: -1 (default) = block forever; 0 = non-blocking; >0 = ms wait  */

/* Max UDP payload per recv. User may request less via the maxlen argument. */
#define LUUDP_RECV_BUF 1472  /* 1 Ethernet MTU - IP/UDP headers */

static int ludp_recv(lua_State *L)
{
    int fd      = (int)luaL_checkinteger(L, 1);
    int maxlen  = (int)luaL_optinteger(L, 2, LUUDP_RECV_BUF);
    int timeout = (int)luaL_optinteger(L, 3, -1);

    if (maxlen <= 0 || maxlen > LUUDP_RECV_BUF) {
        maxlen = LUUDP_RECV_BUF;
    }

    char *buf = (char *)malloc((size_t)maxlen);
    if (!buf) {
        lua_pushnil(L);
        lua_pushstring(L, "udp.recv: out of memory");
        return 2;
    }

    /* Set SO_RCVTIMEO (or use MSG_DONTWAIT for timeout=0).
     * lwIP convention: tv={0,0} means block indefinitely, so timeout=0
     * (documented as non-blocking) must use MSG_DONTWAIT instead. */
    int recv_flags = 0;
    if (timeout == 0) {
        recv_flags = MSG_DONTWAIT;
        /* Still reset SO_RCVTIMEO to block-indefinitely so a subsequent
         * blocking call on the same fd is not affected by a previous timeout. */
        struct timeval tv = {0, 0};
        lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    } else {
        struct timeval tv;
        if (timeout > 0) {
            tv.tv_sec  = timeout / 1000;
            tv.tv_usec = (timeout % 1000) * 1000;
        } else {
            tv.tv_sec  = 0;   /* timeout < 0 → block indefinitely */
            tv.tv_usec = 0;
        }
        if (lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
            free(buf);
            lua_pushnil(L);
            lua_pushstring(L, "udp.recv: setsockopt failed");
            return 2;
        }
    }

    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    int n = lwip_recvfrom(fd, buf, (size_t)maxlen, recv_flags,
                          (struct sockaddr *)&from, &fromlen);

    if (n < 0) {
        int e = errno;
        free(buf);
        if (e == EAGAIN || e == EWOULDBLOCK) {
            /* Timeout — expected when no data arrives; caller treats nil as "no data" */
            lua_pushnil(L);
            return 1;
        }
        lua_pushnil(L);
        lua_pushfstring(L, "udp.recv: recvfrom error %d", e);
        return 2;
    }

    lua_pushlstring(L, buf, (size_t)n);  /* Lua copies the string internally */
    free(buf);
    char ip_str[16];
    inet_ntoa_r(from.sin_addr, ip_str, sizeof(ip_str));
    lua_pushstring(L, ip_str);
    lua_pushinteger(L, (lua_Integer)ntohs(from.sin_port));
    return 3;
}

/* ---- module registration ------------------------------------------------- */

static const luaL_Reg ludp_funcs[] = {
    {"open",  ludp_open},
    {"send",  ludp_send},
    {"close", ludp_close},
    {"bind",  ludp_bind},
    {"recv",  ludp_recv},
    {NULL, NULL}
};

LUAMOD_API int luaopen_udp(lua_State *L)
{
    luaL_newlib(L, ludp_funcs);
    return 1;
}

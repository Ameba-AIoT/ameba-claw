/*
 * lucjson.c — minimal cjson-compatible Lua binding using cJSON.
 *
 * Provides require("cjson"):
 *   cjson.decode(str)  → Lua table / string / number / boolean / nil
 *   cjson.encode(val)  → JSON string
 *
 * Only basic types are supported (no userdata, no cycles).
 */

#define LUA_LIB
#include "lua.h"
#include "lauxlib.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

/* ---- decode: cJSON → Lua ---- */

static void cjson_to_lua(lua_State *L, cJSON *item)
{
    if (!item) { lua_pushnil(L); return; }

    switch (item->type & 0xFF) {
    case cJSON_NULL:   lua_pushnil(L); break;
    case cJSON_True:   lua_pushboolean(L, 1); break;
    case cJSON_False:  lua_pushboolean(L, 0); break;
    case cJSON_Number:
        if (item->valuedouble == (double)(long long)item->valuedouble)
            lua_pushinteger(L, (lua_Integer)item->valuedouble);
        else
            lua_pushnumber(L, (lua_Number)item->valuedouble);
        break;
    case cJSON_String:
        lua_pushstring(L, item->valuestring ? item->valuestring : "");
        break;
    case cJSON_Array: {
        lua_newtable(L);
        int i = 1;
        for (cJSON *c = item->child; c; c = c->next, i++) {
            cjson_to_lua(L, c);
            lua_rawseti(L, -2, i);
        }
        break;
    }
    case cJSON_Object: {
        lua_newtable(L);
        for (cJSON *c = item->child; c; c = c->next) {
            lua_pushstring(L, c->string ? c->string : "");
            cjson_to_lua(L, c);
            lua_rawset(L, -3);
        }
        break;
    }
    default: lua_pushnil(L); break;
    }
}

static int lcjson_decode(lua_State *L)
{
    const char *str = luaL_checkstring(L, 1);
    cJSON *root = cJSON_Parse(str);
    if (!root) {
        lua_pushnil(L);
        lua_pushstring(L, "cjson.decode: parse error");
        return 2;
    }
    cjson_to_lua(L, root);
    cJSON_Delete(root);
    return 1;
}

/* ---- encode: Lua → cJSON ---- */

static cJSON *lua_to_cjson(lua_State *L, int idx, int depth)
{
    if (depth > 16) return cJSON_CreateString("...(too deep)");

    int t = lua_type(L, idx);
    switch (t) {
    case LUA_TNIL:     return cJSON_CreateNull();
    case LUA_TBOOLEAN: return lua_toboolean(L, idx) ? cJSON_CreateTrue() : cJSON_CreateFalse();
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx))
            return cJSON_CreateNumber((double)lua_tointeger(L, idx));
        return cJSON_CreateNumber((double)lua_tonumber(L, idx));
    case LUA_TSTRING:  return cJSON_CreateString(lua_tostring(L, idx));
    case LUA_TTABLE: {
        /* Determine array vs object.
         * Sparse tables (e.g. {[1]="a",[100]="b"}) are treated as objects
         * to avoid generating a 100-element array with 98 nulls. */
        int is_array = 1;
        int maxn  = 0;
        int count = 0;
        lua_pushnil(L);
        while (lua_next(L, idx) != 0) {
            if (lua_type(L, -2) != LUA_TNUMBER || !lua_isinteger(L, -2)) {
                is_array = 0; lua_pop(L, 2); break;
            }
            int k = (int)lua_tointeger(L, -2);
            if (k < 1) { is_array = 0; lua_pop(L, 2); break; }
            if (k > maxn) maxn = k;
            count++;
            lua_pop(L, 1);
        }
        if (is_array && maxn != count) is_array = 0;
        if (is_array) {
            cJSON *arr = cJSON_CreateArray();
            for (int i = 1; i <= maxn; i++) {
                lua_rawgeti(L, idx, i);
                cJSON_AddItemToArray(arr, lua_to_cjson(L, lua_gettop(L), depth + 1));
                lua_pop(L, 1);
            }
            return arr;
        } else {
            cJSON *obj = cJSON_CreateObject();
            lua_pushnil(L);
            while (lua_next(L, idx) != 0) {
                const char *key = lua_tostring(L, -2);
                if (key) {
                    cJSON_AddItemToObject(obj, key,
                        lua_to_cjson(L, lua_gettop(L), depth + 1));
                }
                lua_pop(L, 1);
            }
            return obj;
        }
    }
    default: return cJSON_CreateString("(unsupported)");
    }
}

static int lcjson_encode(lua_State *L)
{
    cJSON *node = lua_to_cjson(L, 1, 0);
    char *str = cJSON_PrintUnformatted(node);
    cJSON_Delete(node);
    if (!str) { lua_pushstring(L, "null"); return 1; }
    lua_pushstring(L, str);
    free(str);
    return 1;
}

/* ---- module ---- */

static const luaL_Reg lcjson_funcs[] = {
    {"decode", lcjson_decode},
    {"encode", lcjson_encode},
    {NULL, NULL}
};

LUAMOD_API int luaopen_cjson(lua_State *L)
{
    luaL_newlib(L, lcjson_funcs);
    return 1;
}

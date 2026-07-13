/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * lvgl_widgets.c — widget factories (`lv.<widget>.create`) + per-widget-type
 * methods.  Lifecycle/generic-object/style/layout/events live in lvgl_lua.c;
 * lvgl_internal.h is the seam.
 *
 * Deviations from phase5_lvgl_full.md §7a.6, decided while checking the
 * vendored LVGL 9.3 headers against the plan's function-mapping table:
 *   - `spinner`: this LVGL version's lv_spinner_create() takes ONLY `parent`
 *     (speed/arc-length are a separate lv_spinner_set_anim_params() call, not
 *     constructor args) — dropped the optional create() args, no methods.
 *   - `tabview`: likewise lv_tabview_create() takes only `parent`; the
 *     optional tab_bar_pos/tab_bar_size constructor args are dropped (not
 *     re-exposed as setters either — not in the plan's own method list).
 *   - `label:set_text_fmt` dropped: it is a C varargs function with no clean
 *     Lua binding; `lbl:set_text(string.format(...))` in Lua is equivalent
 *     and keeps this file smaller.
 *   - `image`/`animimg` sources are Lua strings only (built-in LV_SYMBOL_*
 *     glyphs or short text), matching the plan's "阶段一暂只支持内置
 *     symbol/font glyph" note — no bitmap image support.
 */

#include <string.h>

#include "lvgl_internal.h"
#include "lauxlib.h"
#include "os_wrapper.h"
#include "ameba_soc.h"

/* ========================================================================= */
/* small shared helpers                                                      */
/* ========================================================================= */

/* rtos_mem_malloc-paired strdup, so this file never mixes allocators (house
 * rule — see AGENTS.md "内存分配器配对"). */
static char *lv_dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)rtos_mem_malloc((u32)n);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

typedef struct {
    const char *name;
    uint32_t    value;
} nv_t;

static uint32_t nv_lookup(lua_State *L, const nv_t *tbl, size_t n, const char *what, const char *name)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(tbl[i].name, name) == 0) {
            return tbl[i].value;
        }
    }
    luaL_error(L, "lvgl: unknown %s '%s'", what, name);
    return 0;
}
#define NV_LOOKUP(L, tbl, what, name) nv_lookup((L), (tbl), sizeof(tbl) / sizeof((tbl)[0]), (what), (name))

/* ========================================================================= */
/* label                                                                     */
/* ========================================================================= */

static const nv_t s_label_long_tbl[] = {
    { "wrap",             LV_LABEL_LONG_MODE_WRAP },
    { "dot",              LV_LABEL_LONG_MODE_DOTS },
    { "scroll",           LV_LABEL_LONG_MODE_SCROLL },
    { "scroll_circular",   LV_LABEL_LONG_MODE_SCROLL_CIRCULAR },
    { "clip",             LV_LABEL_LONG_MODE_CLIP },
};

static int l_label_set_text(lua_State *L)
{
    lv_label_set_text(lvgl_check_obj(L, 1), luaL_checkstring(L, 2));
    return 0;
}
static int l_label_set_long_mode(lua_State *L)
{
    uint32_t m = NV_LOOKUP(L, s_label_long_tbl, "label long mode", luaL_checkstring(L, 2));
    lv_label_set_long_mode(lvgl_check_obj(L, 1), (lv_label_long_mode_t)m);
    return 0;
}
static int l_label_set_recolor(lua_State *L)
{
    lv_label_set_recolor(lvgl_check_obj(L, 1), (bool)lua_toboolean(L, 2));
    return 0;
}
static const luaL_Reg s_label_methods[] = {
    { "set_text",      l_label_set_text },
    { "set_long_mode",  l_label_set_long_mode },
    { "set_recolor",   l_label_set_recolor },
    { NULL, NULL },
};

/* ========================================================================= */
/* bar / slider (share the mode name table)                                  */
/* ========================================================================= */

static const nv_t s_bar_mode_tbl[] = {
    { "normal",      LV_BAR_MODE_NORMAL },
    { "symmetrical", LV_BAR_MODE_SYMMETRICAL },
    { "range",       LV_BAR_MODE_RANGE },
};

static int l_bar_set_value(lua_State *L)
{
    lv_bar_set_value(lvgl_check_obj(L, 1), (int32_t)lvgl_check_int(L, 2),
                     lua_toboolean(L, 3) ? LV_ANIM_ON : LV_ANIM_OFF);
    return 0;
}
static int l_bar_set_start_value(lua_State *L)
{
    lv_bar_set_start_value(lvgl_check_obj(L, 1), (int32_t)lvgl_check_int(L, 2),
                           lua_toboolean(L, 3) ? LV_ANIM_ON : LV_ANIM_OFF);
    return 0;
}
static int l_bar_set_range(lua_State *L)
{
    lv_bar_set_range(lvgl_check_obj(L, 1), (int32_t)lvgl_check_int(L, 2), (int32_t)lvgl_check_int(L, 3));
    return 0;
}
static int l_bar_set_mode(lua_State *L)
{
    uint32_t m = NV_LOOKUP(L, s_bar_mode_tbl, "bar mode", luaL_checkstring(L, 2));
    lv_bar_set_mode(lvgl_check_obj(L, 1), (lv_bar_mode_t)m);
    return 0;
}
static int l_bar_get_value(lua_State *L)
{
    lua_pushinteger(L, lv_bar_get_value(lvgl_check_obj(L, 1)));
    return 1;
}
static const luaL_Reg s_bar_methods[] = {
    { "set_value",       l_bar_set_value },
    { "set_start_value",  l_bar_set_start_value },
    { "set_range",        l_bar_set_range },
    { "set_mode",         l_bar_set_mode },
    { "get_value",        l_bar_get_value },
    { NULL, NULL },
};

static int l_slider_set_value(lua_State *L)
{
    lv_slider_set_value(lvgl_check_obj(L, 1), (int32_t)lvgl_check_int(L, 2),
                        lua_toboolean(L, 3) ? LV_ANIM_ON : LV_ANIM_OFF);
    return 0;
}
static int l_slider_set_range(lua_State *L)
{
    lv_slider_set_range(lvgl_check_obj(L, 1), (int32_t)lvgl_check_int(L, 2), (int32_t)lvgl_check_int(L, 3));
    return 0;
}
static int l_slider_set_mode(lua_State *L)
{
    uint32_t m = NV_LOOKUP(L, s_bar_mode_tbl, "slider mode", luaL_checkstring(L, 2));
    lv_slider_set_mode(lvgl_check_obj(L, 1), (lv_slider_mode_t)m);
    return 0;
}
static int l_slider_get_value(lua_State *L)
{
    lua_pushinteger(L, lv_slider_get_value(lvgl_check_obj(L, 1)));
    return 1;
}
static const luaL_Reg s_slider_methods[] = {
    { "set_value", l_slider_set_value },
    { "set_range",  l_slider_set_range },
    { "set_mode",   l_slider_set_mode },
    { "get_value",  l_slider_get_value },
    { NULL, NULL },
};

/* ========================================================================= */
/* checkbox                                                                  */
/* ========================================================================= */

static int l_checkbox_set_text(lua_State *L)
{
    lv_checkbox_set_text(lvgl_check_obj(L, 1), luaL_checkstring(L, 2));
    return 0;
}
static const luaL_Reg s_checkbox_methods[] = {
    { "set_text", l_checkbox_set_text },
    { NULL, NULL },
};

/* ========================================================================= */
/* arc                                                                       */
/* ========================================================================= */

static const nv_t s_arc_mode_tbl[] = {
    { "normal",      LV_ARC_MODE_NORMAL },
    { "reverse",     LV_ARC_MODE_REVERSE },
    { "symmetrical", LV_ARC_MODE_SYMMETRICAL },
};

static int l_arc_set_value(lua_State *L)
{
    lv_arc_set_value(lvgl_check_obj(L, 1), (int32_t)lvgl_check_int(L, 2));
    return 0;
}
static int l_arc_set_range(lua_State *L)
{
    lv_arc_set_range(lvgl_check_obj(L, 1), (int32_t)lvgl_check_int(L, 2), (int32_t)lvgl_check_int(L, 3));
    return 0;
}
static int l_arc_set_angles(lua_State *L)
{
    lv_arc_set_angles(lvgl_check_obj(L, 1), (lv_value_precise_t)lvgl_check_int(L, 2),
                      (lv_value_precise_t)lvgl_check_int(L, 3));
    return 0;
}
static int l_arc_set_bg_angles(lua_State *L)
{
    lv_arc_set_bg_angles(lvgl_check_obj(L, 1), (lv_value_precise_t)lvgl_check_int(L, 2),
                         (lv_value_precise_t)lvgl_check_int(L, 3));
    return 0;
}
static int l_arc_set_rotation(lua_State *L)
{
    lv_arc_set_rotation(lvgl_check_obj(L, 1), (int32_t)lvgl_check_int(L, 2));
    return 0;
}
static int l_arc_set_mode(lua_State *L)
{
    uint32_t m = NV_LOOKUP(L, s_arc_mode_tbl, "arc mode", luaL_checkstring(L, 2));
    lv_arc_set_mode(lvgl_check_obj(L, 1), (lv_arc_mode_t)m);
    return 0;
}
static int l_arc_get_value(lua_State *L)
{
    lua_pushinteger(L, lv_arc_get_value(lvgl_check_obj(L, 1)));
    return 1;
}
static const luaL_Reg s_arc_methods[] = {
    { "set_value",     l_arc_set_value },
    { "set_range",      l_arc_set_range },
    { "set_angles",     l_arc_set_angles },
    { "set_bg_angles",   l_arc_set_bg_angles },
    { "set_rotation",   l_arc_set_rotation },
    { "set_mode",       l_arc_set_mode },
    { "get_value",      l_arc_get_value },
    { NULL, NULL },
};

/* ========================================================================= */
/* image                                                                     */
/* ========================================================================= */

static int l_image_set_src(lua_State *L)
{
    /* Phase-one scope: built-in symbol / short text only (see file banner).
     * lv_image_set_src keeps only the pointer, so the string must outlive the
     * widget — duplicated once and intentionally leaked (freed only at
     * session teardown), same trade-off as grid dsc arrays. */
    char *dup = lv_dup_str(luaL_checkstring(L, 2));
    if (!dup) {
        return luaL_error(L, "lvgl: out of memory duplicating image src");
    }
    lv_image_set_src(lvgl_check_obj(L, 1), dup);
    return 0;
}
static int l_image_set_rotation(lua_State *L)
{
    lv_image_set_rotation(lvgl_check_obj(L, 1), (int32_t)lvgl_check_int(L, 2));
    return 0;
}
static int l_image_set_scale(lua_State *L)
{
    lv_image_set_scale(lvgl_check_obj(L, 1), (uint32_t)lvgl_check_int(L, 2));
    return 0;
}
static int l_image_set_offset(lua_State *L)
{
    lv_obj_t *obj = lvgl_check_obj(L, 1);
    lv_image_set_offset_x(obj, (int32_t)lvgl_check_int(L, 2));
    lv_image_set_offset_y(obj, (int32_t)lvgl_check_int(L, 3));
    return 0;
}
static const luaL_Reg s_image_methods[] = {
    { "set_src",       l_image_set_src },
    { "set_rotation",   l_image_set_rotation },
    { "set_scale",      l_image_set_scale },
    { "set_offset",     l_image_set_offset },
    { NULL, NULL },
};

/* ========================================================================= */
/* line                                                                      */
/* ========================================================================= */

static int l_line_set_points(lua_State *L)
{
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_Integer n = luaL_len(L, 2);
    lua_Integer npts = n / 2;
    if (npts < 2) {
        return luaL_error(L, "lvgl: set_points needs at least 2 points (4 numbers)");
    }
    lv_point_precise_t *pts = (lv_point_precise_t *)rtos_mem_malloc((u32)(npts * sizeof(*pts)));
    if (!pts) {
        return luaL_error(L, "lvgl: out of memory building line points");
    }
    for (lua_Integer i = 0; i < npts; i++) {
        lua_rawgeti(L, 2, i * 2 + 1);
        pts[i].x = (lv_value_precise_t)lua_tonumber(L, -1);
        lua_rawgeti(L, 2, i * 2 + 2);
        pts[i].y = (lv_value_precise_t)lua_tonumber(L, -1);
        lua_pop(L, 2);
    }
    /* lv_line_set_points keeps only the pointer — leaked until session end,
     * same trade-off noted on grid dsc / image src above. */
    lv_line_set_points(lvgl_check_obj(L, 1), pts, (uint32_t)npts);
    return 0;
}
static int l_line_set_y_invert(lua_State *L)
{
    lv_line_set_y_invert(lvgl_check_obj(L, 1), (bool)lua_toboolean(L, 2));
    return 0;
}
static const luaL_Reg s_line_methods[] = {
    { "set_points",   l_line_set_points },
    { "set_y_invert",  l_line_set_y_invert },
    { NULL, NULL },
};

/* ========================================================================= */
/* chart (+ chart_series sub-object)                                        */
/* ========================================================================= */

static const nv_t s_chart_type_tbl[] = {
    { "line",    LV_CHART_TYPE_LINE },
    { "bar",     LV_CHART_TYPE_BAR },
    { "scatter", LV_CHART_TYPE_SCATTER },
};
static const nv_t s_chart_axis_tbl[] = {
    { "primary_y",   LV_CHART_AXIS_PRIMARY_Y },
    { "secondary_y",  LV_CHART_AXIS_SECONDARY_Y },
    { "primary_x",   LV_CHART_AXIS_PRIMARY_X },
    { "secondary_x",  LV_CHART_AXIS_SECONDARY_X },
};

static int l_chart_set_type(lua_State *L)
{
    uint32_t t = NV_LOOKUP(L, s_chart_type_tbl, "chart type", luaL_checkstring(L, 2));
    lv_chart_set_type(lvgl_check_obj(L, 1), (lv_chart_type_t)t);
    return 0;
}
static int l_chart_set_point_count(lua_State *L)
{
    lv_chart_set_point_count(lvgl_check_obj(L, 1), (uint32_t)lvgl_check_int(L, 2));
    return 0;
}
static int l_chart_set_range(lua_State *L)
{
    uint32_t axis = NV_LOOKUP(L, s_chart_axis_tbl, "chart axis", luaL_checkstring(L, 2));
    lv_chart_set_axis_range(lvgl_check_obj(L, 1), (lv_chart_axis_t)axis,
                            (int32_t)lvgl_check_int(L, 3), (int32_t)lvgl_check_int(L, 4));
    return 0;
}
static int l_chart_refresh(lua_State *L)
{
    lv_chart_refresh(lvgl_check_obj(L, 1));
    return 0;
}

static int l_series_set_next_value(lua_State *L)
{
    lvgl_series_ud_t *s = (lvgl_series_ud_t *)luaL_checkudata(L, 1, LVGL_SERIES_MT);
    lv_chart_set_next_value(s->chart, s->ser, (int32_t)lvgl_check_int(L, 2));
    return 0;
}
static int l_series_set_all_values(lua_State *L)
{
    lvgl_series_ud_t *s = (lvgl_series_ud_t *)luaL_checkudata(L, 1, LVGL_SERIES_MT);
    lv_chart_set_all_values(s->chart, s->ser, (int32_t)lvgl_check_int(L, 2));
    return 0;
}
static int l_series_set_color(lua_State *L)
{
    lvgl_series_ud_t *s = (lvgl_series_ud_t *)luaL_checkudata(L, 1, LVGL_SERIES_MT);
    lv_chart_set_series_color(s->chart, s->ser, lvgl_check_color(L, 2));
    return 0;
}
static int l_series_set_values(lua_State *L)
{
    lvgl_series_ud_t *s = (lvgl_series_ud_t *)luaL_checkudata(L, 1, LVGL_SERIES_MT);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_Integer n = luaL_len(L, 2);
    if (n < 1) {
        return luaL_error(L, "lvgl: set_values needs at least one value");
    }
    int32_t *vals = (int32_t *)rtos_mem_malloc((u32)(n * sizeof(int32_t)));
    if (!vals) {
        return luaL_error(L, "lvgl: out of memory building series values");
    }
    for (lua_Integer i = 0; i < n; i++) {
        lua_rawgeti(L, 2, i + 1);
        vals[i] = (int32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    /* values_cnt must match the chart's :set_point_count(n) — LVGL asserts
     * on mismatch. */
    lv_chart_set_series_values(s->chart, s->ser, vals, (size_t)n);
    rtos_mem_free(vals);
    return 0;
}
static const luaL_Reg s_series_methods[] = {
    { "set_next_value",  l_series_set_next_value },
    { "set_all_values",  l_series_set_all_values },
    { "set_values",      l_series_set_values },
    { "set_color",       l_series_set_color },
    { NULL, NULL },
};

static int l_chart_add_series(lua_State *L)
{
    lv_obj_t *chart = lvgl_check_obj(L, 1);
    lv_color_t c = lvgl_check_color(L, 2);
    uint32_t axis = lua_isstring(L, 3) ? NV_LOOKUP(L, s_chart_axis_tbl, "chart axis", lua_tostring(L, 3))
                                       : LV_CHART_AXIS_PRIMARY_Y;
    lv_chart_series_t *ser = lv_chart_add_series(chart, c, (lv_chart_axis_t)axis);
    if (!ser) {
        return luaL_error(L, "lvgl: chart:add_series failed");
    }
    lvgl_series_ud_t *ud = (lvgl_series_ud_t *)lua_newuserdata(L, sizeof(*ud));
    ud->chart = chart;
    ud->ser   = ser;
    if (luaL_newmetatable(L, LVGL_SERIES_MT)) {
        lua_newtable(L);
        lvgl_install_locked(L, s_series_methods);
        lua_setfield(L, -2, "__index");
    }
    lua_setmetatable(L, -2);
    return 1;
}
static const luaL_Reg s_chart_methods[] = {
    { "set_type",         l_chart_set_type },
    { "set_point_count",   l_chart_set_point_count },
    { "set_range",         l_chart_set_range },
    { "add_series",        l_chart_add_series },
    { "refresh",           l_chart_refresh },
    { NULL, NULL },
};

/* ========================================================================= */
/* dropdown                                                                  */
/* ========================================================================= */

static int l_dropdown_set_options(lua_State *L)
{
    lv_dropdown_set_options(lvgl_check_obj(L, 1), luaL_checkstring(L, 2));
    return 0;
}
static int l_dropdown_add_option(lua_State *L)
{
    lv_dropdown_add_option(lvgl_check_obj(L, 1), luaL_checkstring(L, 2), (uint32_t)lvgl_check_int(L, 3));
    return 0;
}
static int l_dropdown_set_selected(lua_State *L)
{
    lv_dropdown_set_selected(lvgl_check_obj(L, 1), (uint32_t)lvgl_check_int(L, 2));
    return 0;
}
static int l_dropdown_get_selected(lua_State *L)
{
    lua_pushinteger(L, lv_dropdown_get_selected(lvgl_check_obj(L, 1)));
    return 1;
}
static int l_dropdown_get_options(lua_State *L)
{
    lua_pushstring(L, lv_dropdown_get_options(lvgl_check_obj(L, 1)));
    return 1;
}
static int l_dropdown_set_dir(lua_State *L)
{
    lv_dropdown_set_dir(lvgl_check_obj(L, 1), lvgl_dir_from_name(L, luaL_checkstring(L, 2)));
    return 0;
}
static int l_dropdown_set_symbol(lua_State *L)
{
    /* lv_dropdown_set_symbol keeps only the pointer — duplicate + leak until
     * session teardown, same trade-off as image src / line points / grid dsc.
     * A nil/omitted arg clears it (symbol = NULL is a valid, documented no-op). */
    const char *sym = lua_isstring(L, 2) ? lua_tostring(L, 2) : NULL;
    char *dup = NULL;
    if (sym) {
        dup = lv_dup_str(sym);
        if (!dup) {
            return luaL_error(L, "lvgl: out of memory duplicating dropdown symbol");
        }
    }
    lv_dropdown_set_symbol(lvgl_check_obj(L, 1), dup);
    return 0;
}
static const luaL_Reg s_dropdown_methods[] = {
    { "set_options",    l_dropdown_set_options },
    { "add_option",     l_dropdown_add_option },
    { "set_selected",   l_dropdown_set_selected },
    { "get_selected",   l_dropdown_get_selected },
    { "get_options",    l_dropdown_get_options },
    { "set_dir",        l_dropdown_set_dir },
    { "set_symbol",     l_dropdown_set_symbol },
    { NULL, NULL },
};

/* ========================================================================= */
/* roller                                                                    */
/* ========================================================================= */

static const nv_t s_roller_mode_tbl[] = {
    { "normal",   LV_ROLLER_MODE_NORMAL },
    { "infinite", LV_ROLLER_MODE_INFINITE },
};

static int l_roller_set_options(lua_State *L)
{
    uint32_t m = lua_isstring(L, 3) ? NV_LOOKUP(L, s_roller_mode_tbl, "roller mode", lua_tostring(L, 3))
                                    : LV_ROLLER_MODE_NORMAL;
    lv_roller_set_options(lvgl_check_obj(L, 1), luaL_checkstring(L, 2), (lv_roller_mode_t)m);
    return 0;
}
static int l_roller_set_selected(lua_State *L)
{
    lv_roller_set_selected(lvgl_check_obj(L, 1), (uint32_t)lvgl_check_int(L, 2),
                           lua_toboolean(L, 3) ? LV_ANIM_ON : LV_ANIM_OFF);
    return 0;
}
static int l_roller_get_selected(lua_State *L)
{
    lua_pushinteger(L, lv_roller_get_selected(lvgl_check_obj(L, 1)));
    return 1;
}
static int l_roller_get_selected_str(lua_State *L)
{
    char buf[64];
    lv_roller_get_selected_str(lvgl_check_obj(L, 1), buf, sizeof(buf));
    lua_pushstring(L, buf);
    return 1;
}
static int l_roller_set_visible_row_count(lua_State *L)
{
    lv_roller_set_visible_row_count(lvgl_check_obj(L, 1), (uint32_t)lvgl_check_int(L, 2));
    return 0;
}
static const luaL_Reg s_roller_methods[] = {
    { "set_options",           l_roller_set_options },
    { "set_selected",          l_roller_set_selected },
    { "get_selected",          l_roller_get_selected },
    { "get_selected_str",      l_roller_get_selected_str },
    { "set_visible_row_count",  l_roller_set_visible_row_count },
    { NULL, NULL },
};

/* ========================================================================= */
/* list                                                                      */
/* ========================================================================= */

static int l_list_add_text(lua_State *L)
{
    lv_obj_t *o = lv_list_add_text(lvgl_check_obj(L, 1), luaL_checkstring(L, 2));
    lvgl_push_obj(L, o, "label");
    return 1;
}
/* txt first, icon optional-second: a single string arg is unambiguously the
 * (far more common) text-only case. The original (icon, txt) order — icon
 * optional-FIRST, txt required-second — meant list:add_button("Clear logs")
 * silently took "Clear logs" as the icon and then errored on the missing
 * (never-supplied) txt argument; every plain-text call broke. */
static int l_list_add_button(lua_State *L)
{
    const char *icon = lua_isstring(L, 3) ? lua_tostring(L, 3) : NULL;
    lv_obj_t *o = lv_list_add_button(lvgl_check_obj(L, 1), icon, luaL_checkstring(L, 2));
    lvgl_push_obj(L, o, "button");
    return 1;
}
static const luaL_Reg s_list_methods[] = {
    { "add_text",   l_list_add_text },
    { "add_button",  l_list_add_button },
    { NULL, NULL },
};

/* ========================================================================= */
/* menu                                                                      */
/* ========================================================================= */

static int l_menu_page_create(lua_State *L)
{
    lv_obj_t *menu = lvgl_check_obj(L, 1);
    const char *title = lua_isstring(L, 2) ? lua_tostring(L, 2) : NULL;
    lv_obj_t *page = lv_menu_page_create(menu, NULL);
    if (title) {
        char *dup = lv_dup_str(title);   /* lv_menu_set_page_title keeps the pointer */
        if (dup) {
            lv_menu_set_page_title(page, dup);
        }
    }
    lvgl_push_obj(L, page, "obj");
    return 1;
}
static int l_menu_set_page(lua_State *L)
{
    lv_menu_set_page(lvgl_check_obj(L, 1), lvgl_check_obj(L, 2));
    return 0;
}
static int l_menu_set_sidebar_page(lua_State *L)
{
    lv_menu_set_sidebar_page(lvgl_check_obj(L, 1), lvgl_check_obj(L, 2));
    return 0;
}
static const luaL_Reg s_menu_methods[] = {
    { "page_create",       l_menu_page_create },
    { "set_page",          l_menu_set_page },
    { "set_sidebar_page",   l_menu_set_sidebar_page },
    { NULL, NULL },
};

/* ========================================================================= */
/* msgbox                                                                    */
/* ========================================================================= */

static int l_msgbox_add_title(lua_State *L)
{
    lv_msgbox_add_title(lvgl_check_obj(L, 1), luaL_checkstring(L, 2));
    return 0;
}
static int l_msgbox_add_text(lua_State *L)
{
    lv_msgbox_add_text(lvgl_check_obj(L, 1), luaL_checkstring(L, 2));
    return 0;
}
static int l_msgbox_add_close_button(lua_State *L)
{
    lv_msgbox_add_close_button(lvgl_check_obj(L, 1));
    return 0;
}
static int l_msgbox_add_footer_button(lua_State *L)
{
    lv_obj_t *b = lv_msgbox_add_footer_button(lvgl_check_obj(L, 1), luaL_checkstring(L, 2));
    lvgl_push_obj(L, b, "button");
    return 1;
}
static int l_msgbox_close(lua_State *L)
{
    lv_msgbox_close(lvgl_check_obj(L, 1));
    return 0;
}
static const luaL_Reg s_msgbox_methods[] = {
    { "add_title",         l_msgbox_add_title },
    { "add_text",          l_msgbox_add_text },
    { "add_close_button",   l_msgbox_add_close_button },
    { "add_footer_button",  l_msgbox_add_footer_button },
    { "close",             l_msgbox_close },
    { NULL, NULL },
};

/* ========================================================================= */
/* table                                                                     */
/* ========================================================================= */

static int l_table_set_cell_value(lua_State *L)
{
    lv_table_set_cell_value(lvgl_check_obj(L, 1), (uint32_t)lvgl_check_int(L, 2),
                            (uint32_t)lvgl_check_int(L, 3), luaL_checkstring(L, 4));
    return 0;
}
static int l_table_set_row_count(lua_State *L)
{
    lv_table_set_row_count(lvgl_check_obj(L, 1), (uint32_t)lvgl_check_int(L, 2));
    return 0;
}
static int l_table_set_column_count(lua_State *L)
{
    lv_table_set_column_count(lvgl_check_obj(L, 1), (uint32_t)lvgl_check_int(L, 2));
    return 0;
}
static int l_table_set_column_width(lua_State *L)
{
    lv_table_set_column_width(lvgl_check_obj(L, 1), (uint32_t)lvgl_check_int(L, 2), (int32_t)lvgl_check_int(L, 3));
    return 0;
}
static int l_table_get_cell_value(lua_State *L)
{
    lua_pushstring(L, lv_table_get_cell_value(lvgl_check_obj(L, 1), (uint32_t)lvgl_check_int(L, 2),
                                              (uint32_t)lvgl_check_int(L, 3)));
    return 1;
}
static const luaL_Reg s_table_methods[] = {
    { "set_cell_value",   l_table_set_cell_value },
    { "set_row_count",     l_table_set_row_count },
    { "set_column_count",  l_table_set_column_count },
    { "set_column_width",  l_table_set_column_width },
    { "get_cell_value",   l_table_get_cell_value },
    { NULL, NULL },
};

/* ========================================================================= */
/* tabview                                                                   */
/* ========================================================================= */

static int l_tabview_add_tab(lua_State *L)
{
    lv_obj_t *c = lv_tabview_add_tab(lvgl_check_obj(L, 1), luaL_checkstring(L, 2));
    lvgl_push_obj(L, c, "obj");
    return 1;
}
static int l_tabview_set_active(lua_State *L)
{
    lv_tabview_set_active(lvgl_check_obj(L, 1), (uint32_t)lvgl_check_int(L, 2),
                          lua_toboolean(L, 3) ? LV_ANIM_ON : LV_ANIM_OFF);
    return 0;
}
static int l_tabview_get_tab_active(lua_State *L)
{
    lua_pushinteger(L, lv_tabview_get_tab_active(lvgl_check_obj(L, 1)));
    return 1;
}
static const luaL_Reg s_tabview_methods[] = {
    { "add_tab",         l_tabview_add_tab },
    { "set_active",       l_tabview_set_active },
    { "get_tab_active",    l_tabview_get_tab_active },
    { NULL, NULL },
};

/* ========================================================================= */
/* textarea                                                                  */
/* ========================================================================= */

static int l_textarea_set_text(lua_State *L)
{
    lv_textarea_set_text(lvgl_check_obj(L, 1), luaL_checkstring(L, 2));
    return 0;
}
static int l_textarea_add_text(lua_State *L)
{
    lv_textarea_add_text(lvgl_check_obj(L, 1), luaL_checkstring(L, 2));
    return 0;
}
static int l_textarea_get_text(lua_State *L)
{
    lua_pushstring(L, lv_textarea_get_text(lvgl_check_obj(L, 1)));
    return 1;
}
static int l_textarea_set_placeholder_text(lua_State *L)
{
    lv_textarea_set_placeholder_text(lvgl_check_obj(L, 1), luaL_checkstring(L, 2));
    return 0;
}
static int l_textarea_set_password_mode(lua_State *L)
{
    lv_textarea_set_password_mode(lvgl_check_obj(L, 1), (bool)lua_toboolean(L, 2));
    return 0;
}
static int l_textarea_set_one_line(lua_State *L)
{
    lv_textarea_set_one_line(lvgl_check_obj(L, 1), (bool)lua_toboolean(L, 2));
    return 0;
}
static int l_textarea_set_max_length(lua_State *L)
{
    lv_textarea_set_max_length(lvgl_check_obj(L, 1), (uint32_t)lvgl_check_int(L, 2));
    return 0;
}
static const luaL_Reg s_textarea_methods[] = {
    { "set_text",              l_textarea_set_text },
    { "add_text",              l_textarea_add_text },
    { "get_text",              l_textarea_get_text },
    { "set_placeholder_text",   l_textarea_set_placeholder_text },
    { "set_password_mode",      l_textarea_set_password_mode },
    { "set_one_line",           l_textarea_set_one_line },
    { "set_max_length",         l_textarea_set_max_length },
    { NULL, NULL },
};

/* ========================================================================= */
/* keyboard                                                                  */
/* ========================================================================= */

static const nv_t s_kb_mode_tbl[] = {
    { "text_lower", LV_KEYBOARD_MODE_TEXT_LOWER },
    { "text_upper", LV_KEYBOARD_MODE_TEXT_UPPER },
    { "special",    LV_KEYBOARD_MODE_SPECIAL },
    { "number",     LV_KEYBOARD_MODE_NUMBER },
};

static int l_keyboard_set_textarea(lua_State *L)
{
    lv_keyboard_set_textarea(lvgl_check_obj(L, 1), lvgl_check_obj(L, 2));
    return 0;
}
static int l_keyboard_set_mode(lua_State *L)
{
    uint32_t m = NV_LOOKUP(L, s_kb_mode_tbl, "keyboard mode", luaL_checkstring(L, 2));
    lv_keyboard_set_mode(lvgl_check_obj(L, 1), (lv_keyboard_mode_t)m);
    return 0;
}
static const luaL_Reg s_keyboard_methods[] = {
    { "set_textarea",  l_keyboard_set_textarea },
    { "set_mode",      l_keyboard_set_mode },
    { NULL, NULL },
};

/* ========================================================================= */
/* animimg                                                                   */
/* ========================================================================= */

static int l_animimg_set_src(lua_State *L)
{
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_Integer n = luaL_len(L, 2);
    if (n < 1) {
        return luaL_error(L, "lvgl: animimg:set_src needs at least one source");
    }
    const void **dsc = (const void **)rtos_mem_malloc((u32)(n * sizeof(void *)));
    if (!dsc) {
        return luaL_error(L, "lvgl: out of memory building animimg src list");
    }
    for (lua_Integer i = 0; i < n; i++) {
        lua_rawgeti(L, 2, i + 1);
        char *dup = lv_dup_str(luaL_checkstring(L, -1));
        lua_pop(L, 1);
        if (!dup) {
            rtos_mem_free(dsc);
            return luaL_error(L, "lvgl: out of memory duplicating animimg src");
        }
        dsc[i] = dup;
    }
    /* lv_animimg_set_src keeps only the array/strings — leaked until session
     * teardown, same trade-off as image src / line points / grid dsc. */
    lv_obj_t *obj = lvgl_check_obj(L, 1);
    lv_animimg_set_src(obj, dsc, (size_t)n);
    if (lua_isinteger(L, 3)) {
        lv_animimg_set_duration(obj, (uint32_t)lvgl_check_int(L, 3));
    }
    if (lua_isinteger(L, 4)) {
        lv_animimg_set_repeat_count(obj, (uint32_t)lvgl_check_int(L, 4));
    }
    return 0;
}
static int l_animimg_start(lua_State *L)
{
    lv_animimg_start(lvgl_check_obj(L, 1));
    return 0;
}
static const luaL_Reg s_animimg_methods[] = {
    { "set_src",  l_animimg_set_src },
    { "start",    l_animimg_start },
    { NULL, NULL },
};

/* ========================================================================= */
/* spec table + factory dispatch                                            */
/* ========================================================================= */

static const lvgl_widget_spec_t s_specs[] = {
    { "obj",       lv_obj_create,       NULL },
    { "button",    lv_button_create,    NULL },
    { "label",     lv_label_create,     s_label_methods },
    { "bar",       lv_bar_create,       s_bar_methods },
    { "slider",    lv_slider_create,    s_slider_methods },
    { "switch",    lv_switch_create,    NULL },
    { "checkbox",  lv_checkbox_create,  s_checkbox_methods },
    { "arc",       lv_arc_create,       s_arc_methods },
    { "spinner",   lv_spinner_create,   NULL },
    { "image",     lv_image_create,     s_image_methods },
    { "line",      lv_line_create,      s_line_methods },
    { "chart",     lv_chart_create,     s_chart_methods },
    { "dropdown",  lv_dropdown_create,  s_dropdown_methods },
    { "roller",    lv_roller_create,    s_roller_methods },
    { "list",      lv_list_create,      s_list_methods },
    { "menu",      lv_menu_create,      s_menu_methods },
    { "msgbox",    lv_msgbox_create,    s_msgbox_methods },
    { "table",     lv_table_create,     s_table_methods },
    { "tabview",   lv_tabview_create,   s_tabview_methods },
    { "textarea",  lv_textarea_create,  s_textarea_methods },
    { "keyboard",  lv_keyboard_create,  s_keyboard_methods },
    { "animimg",   lv_animimg_create,   s_animimg_methods },
};

const lvgl_widget_spec_t *lvgl_widget_specs(size_t *count)
{
    if (count) {
        *count = sizeof(s_specs) / sizeof(s_specs[0]);
    }
    return s_specs;
}

static int l_widget_create(lua_State *L)
{
    const lvgl_widget_spec_t *spec =
        (const lvgl_widget_spec_t *)lua_touserdata(L, lua_upvalueindex(1));
    /* lvgl_check_obj() may luaL_error() (longjmp) — resolve the parent arg
     * BEFORE taking the lock, never while it's held (see lvgl_lock()'s
     * comment in lvgl_internal.h). */
    int has_parent = !lua_isnoneornil(L, 1);
    lv_obj_t *parent_arg = has_parent ? lvgl_check_obj(L, 1) : NULL;

    lvgl_lock();
    lv_obj_t *parent = has_parent ? parent_arg : lv_screen_active();
    lv_obj_t *obj = spec->create1(parent);
    lvgl_unlock();

    if (!obj) {
        return luaL_error(L, "lvgl: %s.create failed (out of memory?)", spec->name);
    }
    lvgl_push_obj(L, obj, spec->name);
    return 1;
}

void lvgl_widgets_install(lua_State *L, int lv_module_tbl)
{
    size_t n = 0;
    const lvgl_widget_spec_t *specs = lvgl_widget_specs(&n);
    for (size_t i = 0; i < n; i++) {
        lvgl_register_class(L, specs[i].name, specs[i].methods);

        lua_newtable(L);                                       /* lv.<name> = {}   */
        lua_pushlightuserdata(L, (void *)&specs[i]);
        lua_pushcclosure(L, l_widget_create, 1);
        lua_setfield(L, -2, "create");
        lua_setfield(L, lv_module_tbl, specs[i].name);
    }
}

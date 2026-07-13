/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * lvgl_lua.c — the "lvgl" Lua module: lifecycle, the lv_timer_handler driver
 * task, generic object/style/layout methods, and the event dispatch queue.
 * Widget factories + per-widget-type methods live in lvgl_widgets.c
 * (lvgl_internal.h is the seam between the two).
 *
 * Design: design_spec/display/{lvgl_display_two_layer.md, phase5_lvgl_full.md}.
 *   - Reuses display_lua.c's ownership arbiter (display_ownership.h) so
 *     `display` and `lvgl` can never be simultaneously active, and its
 *     lv_init()/backend selection (display_backend.h) so panel bring-up is
 *     not duplicated.
 *   - Deviations from the original plan, decided while implementing (see also
 *     lvgl_widgets.c and docs/lvgl.md):
 *     1. lv.start(display_id[, touch_id]) takes explicit board.json device
 *        ids, unlike the plan's argument-less lv.start().  display.init(id)
 *        and touch.init(id) both need one; lvgl.start() brings up the same
 *        two devices for the same reason, so it needs the same two ids.
 *     2. be->init() always creates an lv_canvas child (the `display` module's
 *        drawing surface) on the screen — this module deletes it immediately;
 *        `lvgl` builds its widget tree directly on the (now empty) screen.
 *     3. The LCDC backend's lv_display_t is, by design, a "display"-mode
 *        formality only (a dummy few-line buffer, no flush_cb — see
 *        CLAW_DISPLAY_LCDC_DUMMY_LINES) because `display` mode never lets
 *        LVGL's own render pipeline run. `lvgl` mode needs that pipeline, so
 *        lvgl_lcdc_flush_cb()/l_start() reprogram the SAME lv_display_t with
 *        real (page-flip) buffers + a flush_cb — a software-side
 *        reconfiguration of LVGL's buffer wiring, not a rewrite of panel
 *        bring-up (the ld->deinit and the electrical bring-up are untouched).
 *     4. Widget lifetime safety: a generation counter (`epoch`), bumped once
 *        per lv.start(), lets every already-issued Lua wrapper from a torn-
 *        down session detect that fact (see lvgl_internal.h's comment on
 *        lvgl_obj_ud_t) without walking a live-object list at teardown time.
 *        Bulk teardown does not proactively luaL_unref per-widget callback
 *        closures (bounded, self-healing leak — reclaimed at lua_close());
 *        walking every widget just to unref is extra complexity phase5 does
 *        not ask for, and the leak cannot grow past
 *        (widgets created) x CLAW_LVGL_EVENT_CB_MAX before the script ends.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "lua_module_lvgl.h"
#include "lvgl_internal.h"

#include "lauxlib.h"
#include "lualib.h"
#include "os_wrapper.h"
#include "ameba_soc.h"

#include "display_backend.h"
#include "display_ownership.h"
#include "lua_driver_touch.h"

#define LVGL_LOG "lvgl"
#define LVGL_SENTINEL_MT "lvgl.sentinel"

/* ========================================================================= */
/* Module state (single active session — enforced by the ownership arbiter) */
/* ========================================================================= */

static struct {
    uint32_t                  epoch;         /* bumped once per lv.start()   */
    uint32_t                  token;         /* disp_own_acquire() token     */
    volatile int              running;       /* lvgl_timer_task run flag     */
    rtos_sema_t                stop_done;     /* task -> teardown ack         */
    const display_backend_t   *be;
    display_surface_t          surf;
    lv_indev_t                *indev;
    lv_obj_t                  *screen;
    int                        touch_active;
} s_lvgl;

/* ========================================================================= */
/* Global LVGL call lock — see the comment on lvgl_lock()/lvgl_install_locked */
/* in lvgl_internal.h for why this exists.                                   */
/* ========================================================================= */

static rtos_mutex_t s_lv_lock;

void lvgl_lock(void)
{
    rtos_mutex_take(s_lv_lock, RTOS_MAX_DELAY);
}

void lvgl_unlock(void)
{
    rtos_mutex_give(s_lv_lock);
}

/* Runs the wrapped function (stashed as the upvalue, a plain lua_CFunction)
 * through lua_pcall *while holding the lock*, so a luaL_error()/luaL_check*
 * failure inside it is caught by lua_pcall's own protection instead of
 * longjmp-ing past lvgl_unlock() below — which would leave the lock held
 * forever and wedge lvgl_timer_task (and every subsequent lvgl call) the
 * next time a script mis-calls a widget method. Only after unlocking do we
 * re-raise (lua_error) if the wrapped call actually failed. */
static int lvgl_locked_call(lua_State *L)
{
    lua_CFunction real = lua_tocfunction(L, lua_upvalueindex(1));
    int nargs = lua_gettop(L);
    lua_pushcfunction(L, real);
    lua_insert(L, 1);
    lvgl_lock();
    int status = lua_pcall(L, nargs, LUA_MULTRET, 0);
    lvgl_unlock();
    if (status != LUA_OK) {
        return lua_error(L);
    }
    return lua_gettop(L);
}

void lvgl_install_locked(lua_State *L, const luaL_Reg *funcs)
{
    for (; funcs->name; funcs++) {
        lua_pushcfunction(L, funcs->func);
        lua_pushcclosure(L, lvgl_locked_call, 1);
        lua_setfield(L, -2, funcs->name);
    }
}

/* ========================================================================= */
/* Argument / colour helpers (mirrors display_lua.c's conventions)           */
/* ========================================================================= */

int lvgl_check_int(lua_State *L, int idx)
{
    if (lua_type(L, idx) != LUA_TNUMBER) {
        return (int)luaL_error(L, "arg %d: expected number", idx);
    }
    lua_Number v = lua_tonumber(L, idx);
    int i = (int)v;
    if (v < 0 && (lua_Number)i != v) {
        i--;
    }
    return i;
}

lv_color_t lvgl_check_color(lua_State *L, int idx)
{
    if (lua_type(L, idx) != LUA_TNUMBER || !lua_isinteger(L, idx)) {
        luaL_error(L, "arg %d: colour must be an integer 0xRRGGBB", idx);
    }
    uint32_t rgb = (uint32_t)lua_tointeger(L, idx) & 0xFFFFFFu;
    return lv_color_hex(rgb);
}

/* ========================================================================= */
/* String <-> enum lookup tables (phase5 §7a.9)                              */
/* ========================================================================= */

typedef struct {
    const char *name;
    uint32_t    value;
} name_val_t;

static uint32_t lookup_or_error(lua_State *L, const name_val_t *tbl, size_t n,
                                const char *what, const char *name)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(tbl[i].name, name) == 0) {
            return tbl[i].value;
        }
    }
    luaL_error(L, "lvgl: unknown %s '%s'", what, name);
    return 0;
}

static const name_val_t s_align_tbl[] = {
    { "top_left",     LV_ALIGN_TOP_LEFT },
    { "top_mid",      LV_ALIGN_TOP_MID },
    { "top_right",    LV_ALIGN_TOP_RIGHT },
    { "left_mid",     LV_ALIGN_LEFT_MID },
    { "center",       LV_ALIGN_CENTER },
    { "right_mid",    LV_ALIGN_RIGHT_MID },
    { "bottom_left",  LV_ALIGN_BOTTOM_LEFT },
    { "bottom_mid",   LV_ALIGN_BOTTOM_MID },
    { "bottom_right", LV_ALIGN_BOTTOM_RIGHT },
};

lv_align_t lvgl_align_from_name(lua_State *L, const char *name)
{
    return (lv_align_t)lookup_or_error(L, s_align_tbl,
        sizeof(s_align_tbl) / sizeof(s_align_tbl[0]), "align", name);
}

static const name_val_t s_flag_tbl[] = {
    { "hidden",          LV_OBJ_FLAG_HIDDEN },
    { "clickable",       LV_OBJ_FLAG_CLICKABLE },
    { "scrollable",      LV_OBJ_FLAG_SCROLLABLE },
    { "click_focusable",  LV_OBJ_FLAG_CLICK_FOCUSABLE },
    { "checkable",        LV_OBJ_FLAG_CHECKABLE },
    { "ignore_layout",    LV_OBJ_FLAG_IGNORE_LAYOUT },
};

lv_obj_flag_t lvgl_flag_from_name(lua_State *L, const char *name)
{
    return (lv_obj_flag_t)lookup_or_error(L, s_flag_tbl,
        sizeof(s_flag_tbl) / sizeof(s_flag_tbl[0]), "obj flag", name);
}

static const name_val_t s_state_tbl[] = {
    { "checked",  LV_STATE_CHECKED },
    { "disabled", LV_STATE_DISABLED },
    { "focused",  LV_STATE_FOCUSED },
    { "pressed",  LV_STATE_PRESSED },
    { "edited",   LV_STATE_EDITED },
};

lv_state_t lvgl_state_from_name(lua_State *L, const char *name)
{
    return (lv_state_t)lookup_or_error(L, s_state_tbl,
        sizeof(s_state_tbl) / sizeof(s_state_tbl[0]), "obj state", name);
}

static const name_val_t s_part_tbl[] = {
    { "main",      LV_PART_MAIN },
    { "indicator", LV_PART_INDICATOR },
    { "knob",      LV_PART_KNOB },
    { "items",     LV_PART_ITEMS },
};

lv_style_selector_t lvgl_selector_from_names(lua_State *L, const char *part, const char *state)
{
    uint32_t p = lookup_or_error(L, s_part_tbl, sizeof(s_part_tbl) / sizeof(s_part_tbl[0]),
                                 "style part", part);
    uint32_t s = state ? (uint32_t)lvgl_state_from_name(L, state) : LV_STATE_DEFAULT;
    return (lv_style_selector_t)(p | s);
}

/* LV_DIR_* is a bitmask, not a plain enum — "hor"/"ver"/"all" are the OR of
 * two-or-more bits, same table serves :set_scroll_dir() and dropdown's
 * :set_dir() (both take a single lv_dir_t). */
static const name_val_t s_dir_tbl[] = {
    { "none",   LV_DIR_NONE },
    { "left",   LV_DIR_LEFT },
    { "right",  LV_DIR_RIGHT },
    { "top",    LV_DIR_TOP },
    { "bottom", LV_DIR_BOTTOM },
    { "hor",    LV_DIR_HOR },
    { "ver",    LV_DIR_VER },
    { "all",    LV_DIR_ALL },
};

lv_dir_t lvgl_dir_from_name(lua_State *L, const char *name)
{
    return (lv_dir_t)lookup_or_error(L, s_dir_tbl,
        sizeof(s_dir_tbl) / sizeof(s_dir_tbl[0]), "dir", name);
}

static const name_val_t s_event_tbl[] = {
    { "clicked",      LV_EVENT_CLICKED },
    { "value_changed", LV_EVENT_VALUE_CHANGED },
    { "pressed",       LV_EVENT_PRESSED },
    { "released",      LV_EVENT_RELEASED },
    { "long_pressed",  LV_EVENT_LONG_PRESSED },
    { "focused",       LV_EVENT_FOCUSED },
    { "defocused",     LV_EVENT_DEFOCUSED },
    { "ready",         LV_EVENT_READY },
    { "cancel",        LV_EVENT_CANCEL },
};

lv_event_code_t lvgl_event_code_from_name(lua_State *L, const char *name)
{
    return (lv_event_code_t)lookup_or_error(L, s_event_tbl,
        sizeof(s_event_tbl) / sizeof(s_event_tbl[0]), "event name", name);
}

const char *lvgl_event_name_from_code(lv_event_code_t code)
{
    for (size_t i = 0; i < sizeof(s_event_tbl) / sizeof(s_event_tbl[0]); i++) {
        if (s_event_tbl[i].value == (uint32_t)code) {
            return s_event_tbl[i].name;
        }
    }
    return "?";
}

/* ========================================================================= */
/* Object wrapping / class registration                                      */
/* ========================================================================= */

uint32_t lvgl_current_epoch(void)
{
    return s_lvgl.epoch;
}

int lvgl_obj_gc(lua_State *L);   /* fwd, installed as every class's __gc */

#define LVGL_GENERIC_KEY "lvgl.generic_methods"

static int l_obj_set_size(lua_State *L);
static int l_obj_set_width(lua_State *L);
static int l_obj_set_height(lua_State *L);
static int l_obj_set_pos(lua_State *L);
static int l_obj_set_x(lua_State *L);
static int l_obj_set_y(lua_State *L);
static int l_obj_set_align(lua_State *L);
static int l_obj_align(lua_State *L);
static int l_obj_align_to(lua_State *L);
static int l_obj_center(lua_State *L);
static int l_obj_add_flag(lua_State *L);
static int l_obj_clear_flag(lua_State *L);
static int l_obj_has_flag(lua_State *L);
static int l_obj_add_state(lua_State *L);
static int l_obj_clear_state(lua_State *L);
static int l_obj_has_state(lua_State *L);
static int l_obj_delete(lua_State *L);
static int l_obj_clean(lua_State *L);
static int l_obj_set_parent(lua_State *L);
static int l_obj_move_foreground(lua_State *L);
static int l_obj_move_background(lua_State *L);
static int l_obj_get_size(lua_State *L);
static int l_obj_get_pos(lua_State *L);
static int l_obj_get_parent(lua_State *L);
static int l_obj_get_child_count(lua_State *L);
static int l_obj_get_child(lua_State *L);
static int l_obj_on(lua_State *L);
static int l_obj_on_click(lua_State *L);
static int l_obj_on_value_changed(lua_State *L);
static int l_obj_off(lua_State *L);
static int l_obj_set_flex_flow(lua_State *L);
static int l_obj_set_flex_align(lua_State *L);
static int l_obj_set_flex_grow(lua_State *L);
static int l_obj_set_grid_dsc(lua_State *L);
static int l_obj_set_grid_cell(lua_State *L);
static int l_obj_set_scroll_dir(lua_State *L);
static int l_obj_set_scrollbar_mode(lua_State *L);
static int l_obj_set_scroll_snap_x(lua_State *L);
static int l_obj_set_scroll_snap_y(lua_State *L);
static int l_obj_set_style_bg_color(lua_State *L);
static int l_obj_set_style_bg_opa(lua_State *L);
static int l_obj_set_style_border_color(lua_State *L);
static int l_obj_set_style_border_width(lua_State *L);
static int l_obj_set_style_radius(lua_State *L);
static int l_obj_set_style_text_color(lua_State *L);
static int l_obj_set_style_text_font(lua_State *L);
static int l_obj_set_style_text_opa(lua_State *L);
static int l_obj_set_style_opa(lua_State *L);
static int l_obj_set_style_pad_all(lua_State *L);
static int l_obj_set_style_pad_top(lua_State *L);
static int l_obj_set_style_pad_bottom(lua_State *L);
static int l_obj_set_style_pad_left(lua_State *L);
static int l_obj_set_style_pad_right(lua_State *L);
static int l_obj_set_style_pad_hor(lua_State *L);
static int l_obj_set_style_pad_ver(lua_State *L);
static int l_obj_set_style_shadow_width(lua_State *L);
static int l_obj_set_style_shadow_color(lua_State *L);
static int l_obj_set_style_shadow_opa(lua_State *L);
static int l_obj_set_style_border_opa(lua_State *L);
static int l_obj_set_style_line_color(lua_State *L);
static int l_obj_set_style_line_width(lua_State *L);
static int l_obj_set_style_line_opa(lua_State *L);
static int l_obj_set_style_arc_color(lua_State *L);
static int l_obj_set_style_arc_width(lua_State *L);
static int l_obj_set_style_arc_rounded(lua_State *L);
static int l_obj_set_style_arc_opa(lua_State *L);

static const luaL_Reg s_generic_methods[] = {
    { "set_size",              l_obj_set_size },
    { "set_width",              l_obj_set_width },
    { "set_height",              l_obj_set_height },
    { "set_pos",                l_obj_set_pos },
    { "set_x",                  l_obj_set_x },
    { "set_y",                  l_obj_set_y },
    { "set_align",               l_obj_set_align },
    { "align",                  l_obj_align },
    { "align_to",                l_obj_align_to },
    { "center",                 l_obj_center },
    { "add_flag",                l_obj_add_flag },
    { "clear_flag",              l_obj_clear_flag },
    { "has_flag",                l_obj_has_flag },
    { "add_state",               l_obj_add_state },
    { "clear_state",             l_obj_clear_state },
    { "has_state",               l_obj_has_state },
    { "delete",                 l_obj_delete },
    { "clean",                  l_obj_clean },
    { "set_parent",              l_obj_set_parent },
    { "move_foreground",         l_obj_move_foreground },
    { "move_background",         l_obj_move_background },
    { "get_size",                l_obj_get_size },
    { "get_pos",                 l_obj_get_pos },
    { "get_parent",              l_obj_get_parent },
    { "get_child_count",          l_obj_get_child_count },
    { "get_child",               l_obj_get_child },
    { "on",                     l_obj_on },
    { "on_click",                l_obj_on_click },
    { "on_value_changed",         l_obj_on_value_changed },
    { "off",                    l_obj_off },
    { "set_flex_flow",            l_obj_set_flex_flow },
    { "set_flex_align",           l_obj_set_flex_align },
    { "set_flex_grow",            l_obj_set_flex_grow },
    { "set_grid_dsc",             l_obj_set_grid_dsc },
    { "set_grid_cell",            l_obj_set_grid_cell },
    { "set_scroll_dir",           l_obj_set_scroll_dir },
    { "set_scrollbar_mode",       l_obj_set_scrollbar_mode },
    { "set_scroll_snap_x",        l_obj_set_scroll_snap_x },
    { "set_scroll_snap_y",        l_obj_set_scroll_snap_y },
    { "set_style_bg_color",        l_obj_set_style_bg_color },
    { "set_style_bg_opa",          l_obj_set_style_bg_opa },
    { "set_style_border_color",     l_obj_set_style_border_color },
    { "set_style_border_width",     l_obj_set_style_border_width },
    { "set_style_border_opa",       l_obj_set_style_border_opa },
    { "set_style_radius",          l_obj_set_style_radius },
    { "set_style_text_color",       l_obj_set_style_text_color },
    { "set_style_text_font",        l_obj_set_style_text_font },
    { "set_style_text_opa",         l_obj_set_style_text_opa },
    { "set_style_opa",             l_obj_set_style_opa },
    { "set_style_pad_all",         l_obj_set_style_pad_all },
    { "set_style_pad_top",         l_obj_set_style_pad_top },
    { "set_style_pad_bottom",       l_obj_set_style_pad_bottom },
    { "set_style_pad_left",        l_obj_set_style_pad_left },
    { "set_style_pad_right",       l_obj_set_style_pad_right },
    { "set_style_pad_hor",         l_obj_set_style_pad_hor },
    { "set_style_pad_ver",         l_obj_set_style_pad_ver },
    { "set_style_shadow_width",     l_obj_set_style_shadow_width },
    { "set_style_shadow_color",     l_obj_set_style_shadow_color },
    { "set_style_shadow_opa",       l_obj_set_style_shadow_opa },
    { "set_style_line_color",       l_obj_set_style_line_color },
    { "set_style_line_width",       l_obj_set_style_line_width },
    { "set_style_line_opa",        l_obj_set_style_line_opa },
    { "set_style_arc_color",        l_obj_set_style_arc_color },
    { "set_style_arc_width",        l_obj_set_style_arc_width },
    { "set_style_arc_rounded",      l_obj_set_style_arc_rounded },
    { "set_style_arc_opa",          l_obj_set_style_arc_opa },
    { NULL, NULL },
};

static void push_generic_methods(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, LVGL_GENERIC_KEY);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lvgl_install_locked(L, s_generic_methods);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, LVGL_GENERIC_KEY);
    }
}

void lvgl_register_class(lua_State *L, const char *name, const luaL_Reg *specific)
{
    char mtname[64];
    snprintf(mtname, sizeof(mtname), LVGL_OBJ_MT_PREFIX "%s", name);
    if (!luaL_newmetatable(L, mtname)) {
        lua_pop(L, 1);
        return;
    }

    lua_newtable(L);                      /* methods                          */
    if (specific) {
        lvgl_install_locked(L, specific);
    }
    lua_newtable(L);                      /* methods' own metatable           */
    push_generic_methods(L);
    lua_setfield(L, -2, "__index");       /* methods_mt.__index = generic     */
    lua_setmetatable(L, -2);              /* setmetatable(methods, methods_mt)*/
    lua_setfield(L, -2, "__index");       /* class_mt.__index = methods       */

    lua_pushcfunction(L, lvgl_obj_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushstring(L, mtname);
    lua_setfield(L, -2, "__name");
    lua_pop(L, 1);
}

lvgl_obj_ud_t *lvgl_push_obj(lua_State *L, lv_obj_t *obj, const char *widget_name)
{
    char mtname[64];
    snprintf(mtname, sizeof(mtname), LVGL_OBJ_MT_PREFIX "%s", widget_name);

    lvgl_obj_ud_t *ud = (lvgl_obj_ud_t *)lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    ud->obj   = obj;
    ud->epoch = s_lvgl.epoch;

    if (luaL_getmetatable(L, mtname) == LUA_TNIL) {
        lua_pop(L, 1);
        luaL_getmetatable(L, LVGL_OBJ_MT_PREFIX "obj");   /* generic fallback */
    }
    lua_setmetatable(L, -2);

    /* Stash the widget class name on the lv_obj_t itself (a static string
     * literal from lvgl_widget_spec_t, valid for program lifetime) so a later
     * event trampoline — which only gets the lv_obj_t*, never this userdata —
     * can re-wrap the same object with its real class instead of falling
     * back to the generic "obj" metatable (which lacks e.g. slider's
     * get_value()). See lvgl_event_trampoline(). */
    if (obj) {
        lv_obj_set_user_data(obj, (void *)widget_name);
    }
    return ud;
}

lvgl_obj_ud_t *lvgl_check_ud(lua_State *L, int idx)
{
    if (lua_type(L, idx) != LUA_TUSERDATA) {
        luaL_error(L, "arg %d: expected an lvgl object", idx);
    }
    return (lvgl_obj_ud_t *)lua_touserdata(L, idx);
}

lv_obj_t *lvgl_check_obj(lua_State *L, int idx)
{
    lvgl_obj_ud_t *ud = lvgl_check_ud(L, idx);
    if (!ud->obj || ud->epoch != s_lvgl.epoch) {
        luaL_error(L, "arg %d: lvgl object no longer valid (deleted or session ended)", idx);
    }
    return ud->obj;
}

/* ========================================================================= */
/* Event dispatch queue — producer: lvgl_timer_task's trampoline; consumer:  */
/* the script's own thread inside lv.run()/lv.process_events() (phase5 §7a.4)*/
/* ========================================================================= */

typedef struct {
    int             fn_ref;
    lv_obj_t       *obj;
    const char     *widget_name; /* class to re-wrap `obj` as; "obj" if unknown */
    lv_event_code_t code;
} evq_item_t;

static evq_item_t   s_evq[CLAW_LVGL_EVENT_QUEUE_MAX];
static int          s_evq_head, s_evq_count;
static rtos_mutex_t s_evq_lock;

static void evq_push(int fn_ref, lv_obj_t *obj, const char *widget_name, lv_event_code_t code)
{
    rtos_mutex_take(s_evq_lock, RTOS_MAX_DELAY);
    if (s_evq_count == CLAW_LVGL_EVENT_QUEUE_MAX) {
        s_evq_head = (s_evq_head + 1) % CLAW_LVGL_EVENT_QUEUE_MAX;
        s_evq_count--;
        RTK_LOGW(LVGL_LOG, "event queue full, dropped oldest\n");
    }
    int tail = (s_evq_head + s_evq_count) % CLAW_LVGL_EVENT_QUEUE_MAX;
    s_evq[tail].fn_ref      = fn_ref;
    s_evq[tail].obj         = obj;
    s_evq[tail].widget_name = widget_name;
    s_evq[tail].code        = code;
    s_evq_count++;
    rtos_mutex_give(s_evq_lock);
}

static int evq_pop(evq_item_t *out)
{
    int ok = 0;
    rtos_mutex_take(s_evq_lock, RTOS_MAX_DELAY);
    if (s_evq_count > 0) {
        *out = s_evq[s_evq_head];
        s_evq_head = (s_evq_head + 1) % CLAW_LVGL_EVENT_QUEUE_MAX;
        s_evq_count--;
        ok = 1;
    }
    rtos_mutex_give(s_evq_lock);
    return ok;
}

/* Drop any not-yet-drained entries that reference `obj` (called by :delete()
 * BEFORE lv_obj_delete(), so a later drain never hands the script a dangling
 * object — phase5 §11 risk item). */
static void evq_purge_obj(lv_obj_t *obj)
{
    rtos_mutex_take(s_evq_lock, RTOS_MAX_DELAY);
    evq_item_t tmp[CLAW_LVGL_EVENT_QUEUE_MAX];
    int kept = 0;
    for (int i = 0; i < s_evq_count; i++) {
        evq_item_t *it = &s_evq[(s_evq_head + i) % CLAW_LVGL_EVENT_QUEUE_MAX];
        if (it->obj != obj) {
            tmp[kept++] = *it;
        }
    }
    memcpy(s_evq, tmp, (size_t)kept * sizeof(tmp[0]));
    s_evq_head  = 0;
    s_evq_count = kept;
    rtos_mutex_give(s_evq_lock);
}

/* Whole-session reset — called from lvgl_timer_task's own teardown (never
 * the caller thread; see the file banner deviation #4 on why no per-widget
 * unref happens here). */
static void evq_clear_all(void)
{
    rtos_mutex_take(s_evq_lock, RTOS_MAX_DELAY);
    s_evq_head = s_evq_count = 0;
    rtos_mutex_give(s_evq_lock);
}

static void lvgl_event_trampoline(lv_event_t *e)
{
    lvgl_cb_handle_t *h = (lvgl_cb_handle_t *)lv_event_get_user_data(e);
    if (!h || lv_event_get_code(e) != h->code) {
        return;
    }
    lv_obj_t *target = lv_event_get_target_obj(e);
    /* lvgl_push_obj() stashed the widget's class name here at creation time;
     * fall back to the generic "obj" class if it's somehow unset (e.g. an
     * internal LVGL part object rather than one this module created). */
    const char *widget_name = (const char *)lv_obj_get_user_data(target);
    if (!widget_name) {
        widget_name = "obj";
    }
    evq_push(h->fn_ref, target, widget_name, h->code);
}

int lvgl_on(lua_State *L, lvgl_obj_ud_t *ud, lv_event_code_t code, int fn_idx)
{
    if (!ud->obj || ud->epoch != s_lvgl.epoch) {
        return -1;
    }
    int slot = -1;
    for (int i = 0; i < CLAW_LVGL_EVENT_CB_MAX; i++) {
        if (!ud->cbs[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }
    lvgl_cb_handle_t *h = (lvgl_cb_handle_t *)rtos_mem_malloc(sizeof(*h));
    if (!h) {
        return -1;
    }
    h->code = code;
    lua_pushvalue(L, fn_idx);
    h->fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ud->cbs[slot] = h;
    lv_obj_add_event_cb(ud->obj, lvgl_event_trampoline, LV_EVENT_ALL, h);
    return 0;
}

int lvgl_off(lua_State *L, lvgl_obj_ud_t *ud, lv_event_code_t code)
{
    if (ud->epoch != s_lvgl.epoch) {
        return 0;
    }
    for (int i = 0; i < CLAW_LVGL_EVENT_CB_MAX; i++) {
        lvgl_cb_handle_t *h = ud->cbs[i];
        if (h && h->code == code) {
            if (ud->obj) {
                lv_obj_remove_event_cb_with_user_data(ud->obj, lvgl_event_trampoline, h);
            }
            luaL_unref(L, LUA_REGISTRYINDEX, h->fn_ref);
            rtos_mem_free(h);
            ud->cbs[i] = NULL;
            return 1;
        }
    }
    return 0;
}

void lvgl_obj_release(lua_State *L, lvgl_obj_ud_t *ud)
{
    if (ud->epoch != s_lvgl.epoch) {
        /* Stale session: bulk teardown already happened; touching lv_* here
         * would dereference freed LVGL memory, and re-unref'ing a slot would
         * corrupt an unrelated live registry entry (see file banner #4). */
        memset(ud->cbs, 0, sizeof(ud->cbs));
        ud->obj = NULL;
        return;
    }
    for (int i = 0; i < CLAW_LVGL_EVENT_CB_MAX; i++) {
        lvgl_cb_handle_t *h = ud->cbs[i];
        if (!h) {
            continue;
        }
        if (ud->obj) {
            lv_obj_remove_event_cb_with_user_data(ud->obj, lvgl_event_trampoline, h);
        }
        luaL_unref(L, LUA_REGISTRYINDEX, h->fn_ref);
        rtos_mem_free(h);
        ud->cbs[i] = NULL;
    }
    ud->obj = NULL;
}

int lvgl_obj_gc(lua_State *L)
{
    lvgl_obj_ud_t *ud = (lvgl_obj_ud_t *)lua_touserdata(L, 1);
    if (ud) {
        lvgl_obj_release(L, ud);
    }
    return 0;
}

/* Drains up to `budget` queued events, running each Lua closure on THIS
 * (the caller's) thread. Must only be called from lv.run()/process_events(). */
int lvgl_drain_events(lua_State *L, uint32_t budget)
{
    evq_item_t it;
    uint32_t n = 0;
    while (n < budget && evq_pop(&it)) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, it.fn_ref);
        if (lua_isfunction(L, -1)) {
            lvgl_push_obj(L, it.obj, it.widget_name);
            lua_pushstring(L, lvgl_event_name_from_code(it.code));
            if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
                /* LOGE, not LOGW: a callback error here is otherwise fully
                 * silent to the script (this pcall is inside the C event
                 * pump, decoupled from the job's own top-level pcall/status —
                 * see docs/lvgl.md and CLAUDE.md notes on lua_job_get not
                 * reflecting event-callback failures). */
                RTK_LOGE(LVGL_LOG, "event callback error: %s\n", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
        n++;
    }
    return (int)n;
}

/* ========================================================================= */
/* Generic object methods                                                    */
/* ========================================================================= */

static int l_obj_set_size(lua_State *L)
{
    lv_obj_set_size(lvgl_check_obj(L, 1), lvgl_check_int(L, 2), lvgl_check_int(L, 3));
    return 0;
}
static int l_obj_set_width(lua_State *L)
{
    lv_obj_set_width(lvgl_check_obj(L, 1), lvgl_check_int(L, 2));
    return 0;
}
static int l_obj_set_height(lua_State *L)
{
    lv_obj_set_height(lvgl_check_obj(L, 1), lvgl_check_int(L, 2));
    return 0;
}
static int l_obj_set_pos(lua_State *L)
{
    lv_obj_set_pos(lvgl_check_obj(L, 1), lvgl_check_int(L, 2), lvgl_check_int(L, 3));
    return 0;
}
static int l_obj_set_x(lua_State *L)
{
    lv_obj_set_x(lvgl_check_obj(L, 1), lvgl_check_int(L, 2));
    return 0;
}
static int l_obj_set_y(lua_State *L)
{
    lv_obj_set_y(lvgl_check_obj(L, 1), lvgl_check_int(L, 2));
    return 0;
}
static int l_obj_set_align(lua_State *L)
{
    lv_obj_set_align(lvgl_check_obj(L, 1), lvgl_align_from_name(L, luaL_checkstring(L, 2)));
    return 0;
}
static int l_obj_align(lua_State *L)
{
    if (lua_type(L, 2) == LUA_TUSERDATA) {
        return luaL_error(L, "align(obj, ...): arg 2 must be an align string, not an object. "
                              "align() only aligns within the parent; to align relative to "
                              "ANOTHER widget use :align_to(other_obj, align[, xofs, yofs]) instead.");
    }
    lv_align_t a = lvgl_align_from_name(L, luaL_checkstring(L, 2));
    int xo = lua_isinteger(L, 3) ? lvgl_check_int(L, 3) : 0;
    int yo = lua_isinteger(L, 4) ? lvgl_check_int(L, 4) : 0;
    lv_obj_align(lvgl_check_obj(L, 1), a, xo, yo);
    return 0;
}
static int l_obj_align_to(lua_State *L)
{
    lv_obj_t *base = lvgl_check_obj(L, 2);
    lv_align_t a = lvgl_align_from_name(L, luaL_checkstring(L, 3));
    int xo = lua_isinteger(L, 4) ? lvgl_check_int(L, 4) : 0;
    int yo = lua_isinteger(L, 5) ? lvgl_check_int(L, 5) : 0;
    lv_obj_align_to(lvgl_check_obj(L, 1), base, a, xo, yo);
    return 0;
}
static int l_obj_center(lua_State *L)
{
    lv_obj_center(lvgl_check_obj(L, 1));
    return 0;
}
static int l_obj_add_flag(lua_State *L)
{
    lv_obj_add_flag(lvgl_check_obj(L, 1), lvgl_flag_from_name(L, luaL_checkstring(L, 2)));
    return 0;
}
static int l_obj_clear_flag(lua_State *L)
{
    lv_obj_remove_flag(lvgl_check_obj(L, 1), lvgl_flag_from_name(L, luaL_checkstring(L, 2)));
    return 0;
}
static int l_obj_has_flag(lua_State *L)
{
    lua_pushboolean(L, lv_obj_has_flag(lvgl_check_obj(L, 1),
                                       lvgl_flag_from_name(L, luaL_checkstring(L, 2))));
    return 1;
}
static int l_obj_add_state(lua_State *L)
{
    lv_obj_add_state(lvgl_check_obj(L, 1), lvgl_state_from_name(L, luaL_checkstring(L, 2)));
    return 0;
}
static int l_obj_clear_state(lua_State *L)
{
    lv_obj_remove_state(lvgl_check_obj(L, 1), lvgl_state_from_name(L, luaL_checkstring(L, 2)));
    return 0;
}
static int l_obj_has_state(lua_State *L)
{
    lua_pushboolean(L, lv_obj_has_state(lvgl_check_obj(L, 1),
                                        lvgl_state_from_name(L, luaL_checkstring(L, 2))));
    return 1;
}
static int l_obj_delete(lua_State *L)
{
    lvgl_obj_ud_t *ud = lvgl_check_ud(L, 1);
    if (ud->obj && ud->epoch == s_lvgl.epoch) {
        lv_obj_t *obj = ud->obj;
        lvgl_obj_release(L, ud);
        evq_purge_obj(obj);
        lv_obj_delete(obj);
    }
    return 0;
}
static int l_obj_clean(lua_State *L)
{
    lv_obj_clean(lvgl_check_obj(L, 1));
    return 0;
}
static int l_obj_set_parent(lua_State *L)
{
    lv_obj_set_parent(lvgl_check_obj(L, 1), lvgl_check_obj(L, 2));
    return 0;
}
static int l_obj_move_foreground(lua_State *L)
{
    lv_obj_t *obj = lvgl_check_obj(L, 1);
    lv_obj_t *parent = lv_obj_get_parent(obj);
    if (!parent) {
        /* obj is a screen (or otherwise parentless) — no siblings to
         * reorder among, so there's nothing to do. Without this check
         * lv_obj_get_child_count(NULL) hits LV_ASSERT_NULL -> while(1). */
        return 0;
    }
    lv_obj_move_to_index(obj, (int32_t)lv_obj_get_child_count(parent) - 1);
    return 0;
}
static int l_obj_move_background(lua_State *L)
{
    lv_obj_move_to_index(lvgl_check_obj(L, 1), 0);
    return 0;
}
static int l_obj_get_size(lua_State *L)
{
    lv_obj_t *obj = lvgl_check_obj(L, 1);
    lua_pushinteger(L, lv_obj_get_width(obj));
    lua_pushinteger(L, lv_obj_get_height(obj));
    return 2;
}
static int l_obj_get_pos(lua_State *L)
{
    lv_obj_t *obj = lvgl_check_obj(L, 1);
    lua_pushinteger(L, lv_obj_get_x(obj));
    lua_pushinteger(L, lv_obj_get_y(obj));
    return 2;
}
static int l_obj_get_parent(lua_State *L)
{
    lv_obj_t *p = lv_obj_get_parent(lvgl_check_obj(L, 1));
    if (!p) {
        lua_pushnil(L);
    } else {
        lvgl_push_obj(L, p, "obj");
    }
    return 1;
}
static int l_obj_get_child_count(lua_State *L)
{
    lua_pushinteger(L, lv_obj_get_child_count(lvgl_check_obj(L, 1)));
    return 1;
}
static int l_obj_get_child(lua_State *L)
{
    lv_obj_t *c = lv_obj_get_child(lvgl_check_obj(L, 1), (int32_t)lvgl_check_int(L, 2));
    if (!c) {
        lua_pushnil(L);
    } else {
        lvgl_push_obj(L, c, "obj");
    }
    return 1;
}
static int l_obj_on(lua_State *L)
{
    const char *name = luaL_checkstring(L, 2);
    lvgl_obj_ud_t *ud = lvgl_check_ud(L, 1);
    lvgl_check_obj(L, 1);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    lv_event_code_t code = lvgl_event_code_from_name(L, name);
    if (lvgl_on(L, ud, code, 3) != 0) {
        return luaL_error(L, "lvgl: too many callbacks on this widget (max %d)",
                          CLAW_LVGL_EVENT_CB_MAX);
    }
    return 0;
}
static int on_named(lua_State *L, const char *ev)
{
    lvgl_obj_ud_t *ud = lvgl_check_ud(L, 1);
    lvgl_check_obj(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lv_event_code_t code = lvgl_event_code_from_name(L, ev);
    if (lvgl_on(L, ud, code, 2) != 0) {
        return luaL_error(L, "lvgl: too many callbacks on this widget (max %d)",
                          CLAW_LVGL_EVENT_CB_MAX);
    }
    return 0;
}
static int l_obj_on_click(lua_State *L)
{
    return on_named(L, "clicked");
}
static int l_obj_on_value_changed(lua_State *L)
{
    return on_named(L, "value_changed");
}
static int l_obj_off(lua_State *L)
{
    lvgl_obj_ud_t *ud = lvgl_check_ud(L, 1);
    lv_event_code_t code = lvgl_event_code_from_name(L, luaL_checkstring(L, 2));
    lvgl_off(L, ud, code);
    return 0;
}

/* ---- layout (flex / grid) -------------------------------------------------- */

static const name_val_t s_flex_flow_tbl[] = {
    { "row",              LV_FLEX_FLOW_ROW },
    { "column",           LV_FLEX_FLOW_COLUMN },
    { "row_wrap",         LV_FLEX_FLOW_ROW_WRAP },
    { "column_wrap",      LV_FLEX_FLOW_COLUMN_WRAP },
    { "row_reverse",      LV_FLEX_FLOW_ROW_REVERSE },
    { "column_reverse",   LV_FLEX_FLOW_COLUMN_REVERSE },
};
static const name_val_t s_flex_align_tbl[] = {
    { "start",          LV_FLEX_ALIGN_START },
    { "end",            LV_FLEX_ALIGN_END },
    { "center",         LV_FLEX_ALIGN_CENTER },
    { "space_between",   LV_FLEX_ALIGN_SPACE_BETWEEN },
    { "space_around",    LV_FLEX_ALIGN_SPACE_AROUND },
    { "space_evenly",    LV_FLEX_ALIGN_SPACE_EVENLY },
};

static int l_obj_set_flex_flow(lua_State *L)
{
    uint32_t f = lookup_or_error(L, s_flex_flow_tbl,
        sizeof(s_flex_flow_tbl) / sizeof(s_flex_flow_tbl[0]), "flex flow",
        luaL_checkstring(L, 2));
    lv_obj_set_flex_flow(lvgl_check_obj(L, 1), (lv_flex_flow_t)f);
    return 0;
}
static int l_obj_set_flex_align(lua_State *L)
{
    uint32_t m = lookup_or_error(L, s_flex_align_tbl,
        sizeof(s_flex_align_tbl) / sizeof(s_flex_align_tbl[0]), "flex align",
        luaL_checkstring(L, 2));
    uint32_t c = lookup_or_error(L, s_flex_align_tbl,
        sizeof(s_flex_align_tbl) / sizeof(s_flex_align_tbl[0]), "flex align",
        luaL_checkstring(L, 3));
    uint32_t t = lookup_or_error(L, s_flex_align_tbl,
        sizeof(s_flex_align_tbl) / sizeof(s_flex_align_tbl[0]), "flex align",
        luaL_checkstring(L, 4));
    lv_obj_set_flex_align(lvgl_check_obj(L, 1), (lv_flex_align_t)m, (lv_flex_align_t)c, (lv_flex_align_t)t);
    return 0;
}
static int l_obj_set_flex_grow(lua_State *L)
{
    lv_obj_set_flex_grow(lvgl_check_obj(L, 1), (uint8_t)lvgl_check_int(L, 2));
    return 0;
}
/* One grid track: a plain number (pixels), the string "content" (size to
 * fit its content, LV_GRID_CONTENT), or "<N>fr"/"fr" (fraction of the
 * remaining space, LV_GRID_FR(N), N defaults to 1). Reads table[idx1] off
 * the stack at `table_idx` and leaves the stack balanced. */
static int32_t grid_track_value(lua_State *L, int table_idx, lua_Integer idx1)
{
    lua_rawgeti(L, table_idx, idx1);
    int32_t v;
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char *s = lua_tostring(L, -1);
        if (strcmp(s, "content") == 0) {
            v = LV_GRID_CONTENT;
        } else {
            char *end = NULL;
            long n = strtol(s, &end, 10);
            if (end == s) {
                n = 1;   /* bare "fr" == "1fr" */
            }
            if (!end || strcmp(end, "fr") != 0) {
                luaL_error(L, "lvgl: grid track '%s' must be a number, \"content\", or \"<N>fr\"", s);
            }
            v = LV_GRID_FR((int32_t)n);
        }
    } else {
        v = (int32_t)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);
    return v;
}
static int l_obj_set_grid_dsc(lua_State *L)
{
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TTABLE);
    lua_Integer ncols = luaL_len(L, 2);
    lua_Integer nrows = luaL_len(L, 3);
    int32_t *cols = (int32_t *)rtos_mem_malloc((u32)((ncols + 1) * sizeof(int32_t)));
    int32_t *rows = (int32_t *)rtos_mem_malloc((u32)((nrows + 1) * sizeof(int32_t)));
    if (!cols || !rows) {
        if (cols) rtos_mem_free(cols);
        if (rows) rtos_mem_free(rows);
        return luaL_error(L, "lvgl: out of memory building grid dsc");
    }
    for (lua_Integer i = 0; i < ncols; i++) {
        cols[i] = grid_track_value(L, 2, i + 1);
    }
    cols[ncols] = LV_GRID_TEMPLATE_LAST;
    for (lua_Integer i = 0; i < nrows; i++) {
        rows[i] = grid_track_value(L, 3, i + 1);
    }
    rows[nrows] = LV_GRID_TEMPLATE_LAST;
    /* lv_obj_set_grid_dsc_array keeps only the pointer (no copy), so these
     * must outlive the object — intentionally leaked (freed only when the
     * whole session tears down); grids are created rarely per skill. */
    lv_obj_set_grid_dsc_array(lvgl_check_obj(L, 1), cols, rows);
    return 0;
}
static int l_obj_set_grid_cell(lua_State *L)
{
    lv_obj_t *obj = lvgl_check_obj(L, 1);
    lv_grid_align_t ca = (lv_grid_align_t)lvgl_check_int(L, 2);
    int col_pos = lvgl_check_int(L, 3), col_span = lvgl_check_int(L, 4);
    lv_grid_align_t ra = (lv_grid_align_t)lvgl_check_int(L, 5);
    int row_pos = lvgl_check_int(L, 6), row_span = lvgl_check_int(L, 7);
    lv_obj_set_grid_cell(obj, ca, col_pos, col_span, ra, row_pos, row_span);
    return 0;
}

/* ---- scrolling -------------------------------------------------------------- */

static const name_val_t s_scrollbar_tbl[] = {
    { "off",    LV_SCROLLBAR_MODE_OFF },
    { "on",     LV_SCROLLBAR_MODE_ON },
    { "active", LV_SCROLLBAR_MODE_ACTIVE },
    { "auto",   LV_SCROLLBAR_MODE_AUTO },
};
static const name_val_t s_scroll_snap_tbl[] = {
    { "none",   LV_SCROLL_SNAP_NONE },
    { "start",  LV_SCROLL_SNAP_START },
    { "end",    LV_SCROLL_SNAP_END },
    { "center", LV_SCROLL_SNAP_CENTER },
};

static int l_obj_set_scroll_dir(lua_State *L)
{
    lv_obj_set_scroll_dir(lvgl_check_obj(L, 1), lvgl_dir_from_name(L, luaL_checkstring(L, 2)));
    return 0;
}
static int l_obj_set_scrollbar_mode(lua_State *L)
{
    uint32_t m = lookup_or_error(L, s_scrollbar_tbl,
        sizeof(s_scrollbar_tbl) / sizeof(s_scrollbar_tbl[0]), "scrollbar mode",
        luaL_checkstring(L, 2));
    lv_obj_set_scrollbar_mode(lvgl_check_obj(L, 1), (lv_scrollbar_mode_t)m);
    return 0;
}
static int l_obj_set_scroll_snap_x(lua_State *L)
{
    uint32_t a = lookup_or_error(L, s_scroll_snap_tbl,
        sizeof(s_scroll_snap_tbl) / sizeof(s_scroll_snap_tbl[0]), "scroll snap",
        luaL_checkstring(L, 2));
    lv_obj_set_scroll_snap_x(lvgl_check_obj(L, 1), (lv_scroll_snap_t)a);
    return 0;
}
static int l_obj_set_scroll_snap_y(lua_State *L)
{
    uint32_t a = lookup_or_error(L, s_scroll_snap_tbl,
        sizeof(s_scroll_snap_tbl) / sizeof(s_scroll_snap_tbl[0]), "scroll snap",
        luaL_checkstring(L, 2));
    lv_obj_set_scroll_snap_y(lvgl_check_obj(L, 1), (lv_scroll_snap_t)a);
    return 0;
}

/* ---- styles ---------------------------------------------------------------- */

static lv_style_selector_t sel_from_optargs(lua_State *L, int part_idx, int state_idx)
{
    const char *part = lua_isstring(L, part_idx) ? lua_tostring(L, part_idx) : "main";
    const char *state = lua_isstring(L, state_idx) ? lua_tostring(L, state_idx) : NULL;
    return lvgl_selector_from_names(L, part, state);
}

static int l_obj_set_style_bg_color(lua_State *L)
{
    lv_obj_set_style_bg_color(lvgl_check_obj(L, 1), lvgl_check_color(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_bg_opa(lua_State *L)
{
    lv_obj_set_style_bg_opa(lvgl_check_obj(L, 1), (lv_opa_t)lvgl_check_int(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_border_color(lua_State *L)
{
    lv_obj_set_style_border_color(lvgl_check_obj(L, 1), lvgl_check_color(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_border_width(lua_State *L)
{
    lv_obj_set_style_border_width(lvgl_check_obj(L, 1), lvgl_check_int(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_border_opa(lua_State *L)
{
    lv_obj_set_style_border_opa(lvgl_check_obj(L, 1), (lv_opa_t)lvgl_check_int(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_radius(lua_State *L)
{
    lv_obj_set_style_radius(lvgl_check_obj(L, 1), lvgl_check_int(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_text_color(lua_State *L)
{
    lv_obj_set_style_text_color(lvgl_check_obj(L, 1), lvgl_check_color(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static const lv_font_t *font_for_size(int px)
{
    if (px >= 26) return &lv_font_montserrat_26;
    if (px >= 24) return &lv_font_montserrat_24;
    if (px >= 20) return &lv_font_montserrat_20;
    if (px >= 14) return &lv_font_montserrat_14;
    return NULL;
}
static int l_obj_set_style_text_font(lua_State *L)
{
    int px = lvgl_check_int(L, 2);
    const lv_font_t *f = font_for_size(px);
    if (!f) {
        return luaL_error(L, "lvgl: unsupported font size %d (use 14/20/24/26)", px);
    }
    lv_obj_set_style_text_font(lvgl_check_obj(L, 1), f, sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_text_opa(lua_State *L)
{
    lv_obj_set_style_text_opa(lvgl_check_obj(L, 1), (lv_opa_t)lvgl_check_int(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_opa(lua_State *L)
{
    lv_obj_set_style_opa(lvgl_check_obj(L, 1), (lv_opa_t)lvgl_check_int(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_pad_all(lua_State *L)
{
    lv_obj_set_style_pad_all(lvgl_check_obj(L, 1), lvgl_check_int(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_pad_top(lua_State *L)
{
    lv_obj_set_style_pad_top(lvgl_check_obj(L, 1), lvgl_check_int(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_pad_bottom(lua_State *L)
{
    lv_obj_set_style_pad_bottom(lvgl_check_obj(L, 1), lvgl_check_int(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_pad_left(lua_State *L)
{
    lv_obj_set_style_pad_left(lvgl_check_obj(L, 1), lvgl_check_int(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_pad_right(lua_State *L)
{
    lv_obj_set_style_pad_right(lvgl_check_obj(L, 1), lvgl_check_int(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_pad_hor(lua_State *L)
{
    lv_obj_set_style_pad_hor(lvgl_check_obj(L, 1), lvgl_check_int(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_pad_ver(lua_State *L)
{
    lv_obj_set_style_pad_ver(lvgl_check_obj(L, 1), lvgl_check_int(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_shadow_width(lua_State *L)
{
    lv_obj_set_style_shadow_width(lvgl_check_obj(L, 1), lvgl_check_int(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_shadow_color(lua_State *L)
{
    lv_obj_set_style_shadow_color(lvgl_check_obj(L, 1), lvgl_check_color(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_shadow_opa(lua_State *L)
{
    lv_obj_set_style_shadow_opa(lvgl_check_obj(L, 1), (lv_opa_t)lvgl_check_int(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_line_color(lua_State *L)
{
    lv_obj_set_style_line_color(lvgl_check_obj(L, 1), lvgl_check_color(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_line_width(lua_State *L)
{
    lv_obj_set_style_line_width(lvgl_check_obj(L, 1), lvgl_check_int(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_line_opa(lua_State *L)
{
    lv_obj_set_style_line_opa(lvgl_check_obj(L, 1), (lv_opa_t)lvgl_check_int(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_arc_color(lua_State *L)
{
    lv_obj_set_style_arc_color(lvgl_check_obj(L, 1), lvgl_check_color(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_arc_width(lua_State *L)
{
    lv_obj_set_style_arc_width(lvgl_check_obj(L, 1), lvgl_check_int(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_arc_rounded(lua_State *L)
{
    lv_obj_set_style_arc_rounded(lvgl_check_obj(L, 1), lua_toboolean(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}
static int l_obj_set_style_arc_opa(lua_State *L)
{
    lv_obj_set_style_arc_opa(lvgl_check_obj(L, 1), (lv_opa_t)lvgl_check_int(L, 2), sel_from_optargs(L, 3, 4));
    return 0;
}

/* ========================================================================= */
/* Touch indev + LCDC flush reconfiguration                                  */
/* ========================================================================= */

static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    int x = 0, y = 0, pressed = 0;
    touch_engine_snapshot(&x, &y, &pressed);
    data->point.x = x;
    data->point.y = y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* be->init() wires the LCDC backend's lv_display_t as a "display"-mode
 * formality only (dummy few-line buffer, no flush_cb — see
 * CLAW_DISPLAY_LCDC_DUMMY_LINES): `display` mode's canvas bypasses LVGL's own
 * render pipeline entirely. `lvgl` mode needs that pipeline for real, so this
 * reprograms the SAME lv_display_t with the backend's real page-flip buffers
 * + a flush_cb that hands the just-rendered buffer to the backend's own
 * present_full() (file banner deviation #3). */
static void lvgl_lcdc_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    s_lvgl.surf.render_buf = px_map;
    s_lvgl.be->present_full(&s_lvgl.surf);
    lv_display_flush_ready(disp);
}

/* ========================================================================= */
/* lv_timer_handler driver task                                              */
/* ========================================================================= */

static void lvgl_timer_task(void *arg)
{
    (void)arg;
    while (s_lvgl.running) {
        lvgl_lock();
        uint32_t next_ms = lv_timer_handler();
        lvgl_unlock();
        if (next_ms == LV_NO_TIMER_READY) {
            next_ms = CLAW_LVGL_TIMER_DEF_PERIOD_MS;
        }
        rtos_time_delay_ms(next_ms);
    }

    /* Teardown happens HERE, on this task's own thread — never on the caller
     * thread that asked for it (phase5 §5 / §11: all lv_* calls must stay on
     * this one thread). Locked too: by the time we get here the script
     * thread should be blocked in lvgl_teardown()'s bounded wait (or already
     * past lua_close()), but the lock is cheap insurance against any lv_*
     * call still in flight on that thread. */
    lvgl_lock();
    if (s_lvgl.indev) {
        lv_indev_delete(s_lvgl.indev);
        s_lvgl.indev = NULL;
    }
    if (s_lvgl.screen) {
        lv_obj_clean(s_lvgl.screen);
        s_lvgl.screen = NULL;
    }
    if (s_lvgl.be) {
        s_lvgl.be->deinit(&s_lvgl.surf);
        s_lvgl.be = NULL;
    }
    lvgl_unlock();
    if (s_lvgl.touch_active) {
        touch_engine_deinit();
        s_lvgl.touch_active = 0;
    }
    evq_clear_all();

    rtos_sema_give(s_lvgl.stop_done);
    rtos_task_delete(NULL);
}

/* Registered with display_ownership.h; called with s_disp.lock HELD, from
 * whichever thread called disp_own_release() (lv.stop(), a wall-clock
 * timeout/cancel, or the __gc sentinel on lua_close()). Only signals + waits
 * — never touches lv_* itself (phase5 §5). */
static void lvgl_teardown(void)
{
    if (!s_lvgl.running) {
        return;
    }
    s_lvgl.running = 0;
    rtos_sema_take(s_lvgl.stop_done, CLAW_LVGL_TEARDOWN_JOIN_TIMEOUT_MS);
    /* On timeout the task is still alive and will finish + self-delete on its
     * own; nothing more to do from this thread either way. */
}

/* ========================================================================= */
/* Lifecycle: lv.start/stop/run/process_events/screen_active                 */
/* ========================================================================= */

static int sentinel_gc(lua_State *L)
{
    uint32_t *tok = (uint32_t *)luaL_checkudata(L, 1, LVGL_SENTINEL_MT);
    if (tok && *tok) {
        disp_own_release(*tok);
    }
    return 0;
}

static void sentinel_create(lua_State *L, uint32_t token)
{
    uint32_t *tok = (uint32_t *)lua_newuserdata(L, sizeof(uint32_t));
    *tok = token;
    luaL_getmetatable(L, LVGL_SENTINEL_MT);
    lua_setmetatable(L, -2);
    luaL_ref(L, LUA_REGISTRYINDEX);
}

static const char *owner_mode_name(disp_owner_mode_t m)
{
    switch (m) {
        case DISP_OWNER_DISPLAY: return "display";
        case DISP_OWNER_LVGL:    return "lvgl";
        default:                 return "none";
    }
}

static int l_start(lua_State *L)
{
    const char *display_id = luaL_checkstring(L, 1);
    const char *touch_id = lua_isstring(L, 2) ? lua_tostring(L, 2) : NULL;

    uint32_t token = disp_own_acquire(DISP_OWNER_LVGL);
    if (!token) {
        lua_pushnil(L);
        lua_pushfstring(L, "display busy: owned by another mode (%s)",
                        owner_mode_name(disp_own_current_mode()));
        return 2;
    }

    disp_lv_ensure_init();

    static const display_backend_t *const backends[] = {
        &display_backend_spi,
        &display_backend_lcdc,
    };
    const display_backend_t *be = NULL;
    for (size_t i = 0; i < sizeof(backends) / sizeof(backends[0]); i++) {
        if (backends[i]->probe && backends[i]->probe(display_id)) {
            be = backends[i];
            break;
        }
    }
    if (!be) {
        disp_own_release(token);
        lua_pushnil(L);
        lua_pushfstring(L, "no display backend for device '%s'", display_id);
        return 2;
    }

    char errbuf[96];
    display_surface_t surf;
    if (be->init(display_id, &surf, errbuf, sizeof(errbuf)) != 0) {
        disp_own_release(token);
        lua_pushnil(L);
        lua_pushstring(L, errbuf);
        return 2;
    }

    /* `lvgl` builds its tree directly on the screen — drop the `display`
     * module's canvas child (phase5 §2 deviation #2). */
    if (surf.canvas) {
        lv_obj_delete(surf.canvas);
        surf.canvas = NULL;
    }

    s_lvgl.be   = be;
    s_lvgl.surf = surf;

    if (be == &display_backend_lcdc) {
        size_t full_sz = (size_t)surf.w * surf.h * surf.bpp;
        lv_display_set_buffers(surf.disp, surf.buffers[0], surf.buffers[1],
                               (uint32_t)full_sz, LV_DISPLAY_RENDER_MODE_FULL);
        lv_display_set_flush_cb(surf.disp, lvgl_lcdc_flush_cb);
        /* Only buffers[0]/[1] are handed to LVGL above — LVGL's own FULL-mode
         * ping-pong rotates just those two, NOT the backend's full nbuf-way
         * round robin. present_full() picks its wait strategy off buf_count,
         * so it must see 2 here or it takes the fire-and-forget triple-buffer
         * path (assumes a 3rd, always-free buffer that lvgl mode never uses)
         * and races LVGL's next redraw against the still-in-flight page flip
         * — the cause of the flicker/tearing seen on tab-switch/slider-drag. */
        s_lvgl.surf.buf_count = 2;
    }

    s_lvgl.epoch++;
    s_lvgl.screen = lv_screen_active();

    s_lvgl.touch_active = 0;
    s_lvgl.indev = NULL;
    if (touch_id) {
        char terr[96];
        if (touch_engine_init(touch_id, terr, sizeof(terr), 0) == 0) {
            s_lvgl.touch_active = 1;
            s_lvgl.indev = lv_indev_create();
            lv_indev_set_type(s_lvgl.indev, LV_INDEV_TYPE_POINTER);
            lv_indev_set_read_cb(s_lvgl.indev, lvgl_touch_read_cb);
        } else {
            RTK_LOGW(LVGL_LOG, "touch_engine_init('%s') failed: %s (continuing without touch)\n",
                     touch_id, terr);
        }
    }

    if (!s_lvgl.stop_done) {
        rtos_sema_create(&s_lvgl.stop_done, 0, 1);
    }
    if (!s_evq_lock) {
        rtos_mutex_create(&s_evq_lock);
    }
    s_lvgl.running = 1;
    if (rtos_task_create(NULL, "lvgl_timer", lvgl_timer_task, NULL,
                         CLAW_LVGL_TIMER_TASK_STACK, CLAW_LVGL_TIMER_TASK_PRIO) != RTK_SUCCESS) {
        s_lvgl.running = 0;
        if (s_lvgl.indev) {
            lv_indev_delete(s_lvgl.indev);
            s_lvgl.indev = NULL;
        }
        if (s_lvgl.touch_active) {
            touch_engine_deinit();
            s_lvgl.touch_active = 0;
        }
        s_lvgl.be->deinit(&s_lvgl.surf);
        s_lvgl.be = NULL;
        disp_own_release(token);
        lua_pushnil(L);
        lua_pushstring(L, "lvgl: timer task create failed");
        return 2;
    }

    s_lvgl.token = token;
    sentinel_create(L, token);

    lua_pushboolean(L, 1);
    lua_pushinteger(L, s_lvgl.surf.w);
    lua_pushinteger(L, s_lvgl.surf.h);
    return 3;
}

static int l_stop(lua_State *L)
{
    (void)L;
    if (s_lvgl.token) {
        disp_own_release(s_lvgl.token);
        s_lvgl.token = 0;
    }
    return 0;
}

static int l_run(lua_State *L)
{
    while (s_lvgl.running) {
        lvgl_drain_events(L, CLAW_LVGL_EVENT_QUEUE_MAX);
        rtos_time_delay_ms(CLAW_LVGL_TIMER_DEF_PERIOD_MS);
    }
    return 0;
}

static int l_process_events(lua_State *L)
{
    uint32_t timeout = lua_isinteger(L, 1) ? (uint32_t)lua_tointeger(L, 1) : 0;
    uint32_t start = (uint32_t)rtos_time_get_current_system_time_ms();
    for (;;) {
        int n = lvgl_drain_events(L, CLAW_LVGL_EVENT_QUEUE_MAX);
        if (n == 0 || !s_lvgl.running) {
            break;
        }
        if ((uint32_t)rtos_time_get_current_system_time_ms() - start >= timeout) {
            break;
        }
    }
    return 0;
}

static int l_screen_active(lua_State *L)
{
    lvgl_lock();
    lv_obj_t *scr = lv_screen_active();
    lvgl_unlock();
    lvgl_push_obj(L, scr, "obj");
    return 1;
}

/* ========================================================================= */
/* Registration                                                              */
/* ========================================================================= */

static const luaL_Reg s_module_funcs[] = {
    { "start",           l_start },
    { "stop",            l_stop },
    { "run",             l_run },
    { "process_events",   l_process_events },
    { "screen_active",    l_screen_active },
    { NULL, NULL },
};

int luaopen_lvgl(lua_State *L)
{
    if (!s_lv_lock) {
        rtos_mutex_create(&s_lv_lock);
    }
    if (luaL_newmetatable(L, LVGL_SENTINEL_MT)) {
        lua_pushcfunction(L, sentinel_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    int lv_tbl = lua_gettop(L);
    luaL_setfuncs(L, s_module_funcs, 0);

    lvgl_widgets_install(L, lv_tbl);

    return 1;
}

void lua_module_lvgl_init(void)
{
    disp_set_lvgl_teardown_fn(lvgl_teardown);
}

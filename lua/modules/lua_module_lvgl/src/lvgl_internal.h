/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * lvgl_internal.h — private seam between lvgl_lua.c (lifecycle, timer task,
 * generic obj/style/layout methods, event dispatch) and lvgl_widgets.c
 * (widget factories + per-widget-type methods).  Not installed anywhere
 * public; both .c files in this module #include it directly.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "lua.h"
#include "lauxlib.h"

#include "ameba_claw_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One Lua-registered callback attached via lv_obj_add_event_cb().  Owned by
 * exactly one slot in an lvgl_obj_ud_t.cbs[]; freed by lvgl_obj_release(). */
typedef struct {
    lv_event_code_t code;
    int             fn_ref;   /* luaL_ref into LUA_REGISTRYINDEX */
} lvgl_cb_handle_t;

/* The userdata payload for EVERY widget/plain-obj wrapper, regardless of
 * which metatable (widget "class") it was created with — only the method
 * table differs; layout and __gc are shared. */
typedef struct {
    lv_obj_t         *obj;    /* NULL after :delete()                       */
    uint32_t          epoch;  /* session generation this wrapper was born in */
    lvgl_cb_handle_t *cbs[CLAW_LVGL_EVENT_CB_MAX];
} lvgl_obj_ud_t;

/* lv_chart_series_t wrapper — not an lv_obj_t, needs the owning chart to call
 * lv_chart_set_*(chart, ser, ...). */
typedef struct {
    lv_obj_t           *chart;
    lv_chart_series_t  *ser;
} lvgl_series_ud_t;

#define LVGL_OBJ_MT_PREFIX "lvgl.obj."      /* + widget name, e.g. "lvgl.obj.button" */
#define LVGL_SERIES_MT     "lvgl.chart_series"

/* ---- object wrapping / validation (lvgl_lua.c) --------------------------- */

/* Wrap `obj` as a new userdata using the metatable for widget class `name`
 * (e.g. "button", "bar", "obj" for the generic fallback).  Leaves the new
 * userdata on top of the Lua stack; returns it for convenience. */
lvgl_obj_ud_t *lvgl_push_obj(lua_State *L, lv_obj_t *obj, const char *widget_name);

/* Checked accessors: raise a Lua error on type mismatch or a deleted object. */
lvgl_obj_ud_t *lvgl_check_ud(lua_State *L, int idx);
lv_obj_t      *lvgl_check_obj(lua_State *L, int idx);

/* Registers one widget "class": builds (or reuses) the "lvgl.obj.<name>"
 * metatable with __index = `specific` methods, falling back to the shared
 * generic method table, and __gc = the common release function.  Called once
 * per widget from lvgl_widgets.c's registration table AND once for "obj"
 * (the plain/generic fallback type) from lvgl_lua.c. */
void lvgl_register_class(lua_State *L, const char *name, const luaL_Reg *specific);

/* Detach every registered Lua callback from `ud` (unref + lv_obj_remove_event_cb
 * + drop any already-queued-but-undrained events for this obj) and null the
 * `obj` pointer.  Does NOT call lv_obj_delete() — :delete() calls this first
 * (while `obj` is still valid, so lv_obj_remove_event_cb has something to
 * operate on) and then deletes the widget itself; __gc calls this alone
 * (the wrapper going out of scope in Lua must not delete a widget that is
 * still live in the visible tree). A no-op if `ud->epoch` no longer matches
 * the current session — see the epoch comment on lvgl_obj_ud_t above: a bulk
 * session teardown (lv_obj_clean) invalidates every wrapper from the old
 * session at once by bumping the epoch, without walking them individually. */
void lvgl_obj_release(lua_State *L, lvgl_obj_ud_t *ud);

/* Current session generation — bumped once per successful lv.start().  A
 * wrapper whose ->epoch differs is from an already-torn-down session: every
 * checked accessor and __gc treat it as already-released (no lv_* calls). */
uint32_t lvgl_current_epoch(void);

/* ---- event dispatch (lvgl_lua.c) — see phase5_lvgl_full.md §7a.4 --------- */

/* Register `fn` (Lua closure at stack index fn_idx) for `code` on `ud`.
 * Returns 0 on success, -1 if `ud` already holds CLAW_LVGL_EVENT_CB_MAX
 * callbacks (caller should raise the Lua error). */
int lvgl_on(lua_State *L, lvgl_obj_ud_t *ud, lv_event_code_t code, int fn_idx);

/* Undo lvgl_on() for the first slot matching `code` (if any). Returns 1 if a
 * slot was removed, 0 if none matched. */
int lvgl_off(lua_State *L, lvgl_obj_ud_t *ud, lv_event_code_t code);

/* Drain queued (fn_ref, obj, code) triples and lua_pcall each — run ONLY on
 * the script's own thread (lv.run()/lv.process_events()), never from
 * lvgl_timer_task.  Returns the number drained. */
int lvgl_drain_events(lua_State *L, uint32_t budget);

/* event_name <-> lv_event_code_t (see phase5 §7a.4's fixed name list). */
lv_event_code_t lvgl_event_code_from_name(lua_State *L, const char *name);
const char      *lvgl_event_name_from_code(lv_event_code_t code);

/* ---- string <-> enum lookup tables shared across obj/style/layout/widgets */
lv_align_t       lvgl_align_from_name(lua_State *L, const char *name);
lv_obj_flag_t    lvgl_flag_from_name(lua_State *L, const char *name);
lv_state_t       lvgl_state_from_name(lua_State *L, const char *name);
lv_style_selector_t lvgl_selector_from_names(lua_State *L, const char *part, const char *state);
lv_dir_t         lvgl_dir_from_name(lua_State *L, const char *name);

/* ---- global LVGL call lock (lvgl_lua.c) ----------------------------------
 * LVGL's C state (object tree, style cache, its internal allocator) is NOT
 * thread-safe. lvgl_timer_task calls lv_timer_handler() continuously on its
 * own thread from the moment lv.start() returns; every Lua-facing function
 * below that touches an lv_* API runs on the SCRIPT's thread instead. Both
 * MUST be serialized by one lock, or the two threads mutate the same object
 * tree/allocator concurrently (seen in practice as a hang inside a corrupted
 * linked-list walk, or a blank/garbage frame from a torn draw buffer).
 * lvgl_lock()/lvgl_unlock() bracket lv_timer_handler(); lvgl_install_locked()
 * wraps a luaL_Reg table's functions so every widget/style/layout method call
 * brackets itself automatically (see lvgl_locked_call in lvgl_lua.c for how
 * it survives a luaL_error()/longjmp from inside the wrapped function without
 * leaking the lock held forever). */
void lvgl_lock(void);
void lvgl_unlock(void);
void lvgl_install_locked(lua_State *L, const luaL_Reg *funcs);

/* ---- misc shared helpers -------------------------------------------------- */

/* Same floor-on-negative int coercion as display_lua.c::check_int, so pixel
 * math written for `display` behaves identically when ported to `lvgl`. */
int lvgl_check_int(lua_State *L, int idx);

/* 0xRRGGBB integer -> lv_color_t (errors on non-integer args), matching
 * display_lua.c's colour convention (phase5 §7.4). */
lv_color_t lvgl_check_color(lua_State *L, int idx);

/* Widget-factory registration (lvgl_widgets.c -> consumed by luaopen_lvgl). */
typedef struct {
    const char       *name;               /* "button", "bar", ...            */
    lv_obj_t         *(*create1)(lv_obj_t *parent);   /* lv_<w>_create        */
    const luaL_Reg   *methods;            /* widget-specific methods, or NULL */
} lvgl_widget_spec_t;

const lvgl_widget_spec_t *lvgl_widget_specs(size_t *count);

/* Registers lvgl_widget_specs()'s classes and builds the `lv.<widget>` factory
 * sub-tables; called once from luaopen_lvgl(). `lv_module_tbl` is the stack
 * index of the module table being built. */
void lvgl_widgets_install(lua_State *L, int lv_module_tbl);

#ifdef __cplusplus
}
#endif

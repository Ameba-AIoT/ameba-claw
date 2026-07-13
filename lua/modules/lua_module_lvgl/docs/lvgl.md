# lvgl — `require("lvgl")`

Declarative widget-tree UI (buttons/sliders/bars/charts/lists/menus/tables/
text input/keyboard/popups) for the on-board LCD. Built for **dashboards and
control panels** — build the widget tree once, then let it run; you do not
redraw every frame like `display`.

- **Mutually exclusive with `display`.** Only one of the two "owns" the
  screen at a time. `lvgl.start()` fails (`nil, err`) if `display` (or another
  `lvgl` session) already owns it, and vice versa.
- **Colours are integers `0xRRGGBB`**, same convention as `display`.
- **Declarative, not a per-frame redraw loop.** Create widgets, set their
  properties, register event callbacks — the screen keeps itself up to date.
  Do **not** write a `while true do ... display-style redraw ... end` loop.
- **You must still call `lv.run()` or `lv.process_events()`** for registered
  callbacks (`:on_click`, etc.) to actually fire — see "Most common mistake"
  below. The UI renders and responds to touch either way; only your
  *callbacks* need this.

**This document is the complete, authoritative API surface — not a sample.**
Every widget type (§"Creating widgets"), every shared method (§"Methods every
widget shares"), and every per-widget method (§"Widget-specific methods") that
exists is listed below, in full, with its exact Lua call form. **If a method,
widget, or enum string isn't written down here, it is not bound and calling
it raises a Lua error — there is no larger, partially-documented API to guess
at.** In particular:
- Don't assume a method exists because *some other* LVGL Lua binding you
  might know of has it (e.g. `lv.chart_create(parent)`, `lv.scr_act()`,
  `lv.color_hex()`, numeric `lv.XXX_YYY` enum constants) — this binding's
  naming convention is `lv.<widget>.create(parent)` / `obj:<method>(...)`,
  colours are plain `0xRRGGBB` integers, and enums are lowercase strings
  (`"line"`, not `LV_CHART_TYPE_LINE`). None of the procedural/numeric-enum
  forms exist here.
- Don't assume a `set_style_*`/generic method exists on one widget just
  because it's documented under another — the tables below are per-section
  authoritative; a method not listed in "Methods every widget shares" AND not
  listed in that widget's row in "Widget-specific methods" does not exist for
  that widget.
- The 21 widget types listed under "Creating widgets" are the complete set.
  `buttonmatrix`, `calendar`, `canvas` (as a widget — use `display` for a
  command-style canvas), `imagebutton`, `led`, `spangroup`, `spinbox`,
  `tileview`, `window` are **not implemented** — don't try them, they will
  fail with "unknown method"/nil errors.
- **`table` is temporarily disabled** — its C binding exists but cell text
  does not render on screen (border/background draw fine, data read/write
  via `set_cell_value`/`get_cell_value` works, but nothing is visible); this
  is an open, unresolved bug, not yet root-caused. **Do not use `lv.table`.**
  If you need a table-like grid, simulate it with plain `obj` row containers
  (one `obj` per row, `set_flex_flow("row")`, a `label` per column at fixed
  `set_pos`/`set_width`) — every other widget's text renders normally, only
  `table`'s cell text is affected.

---

## Lifecycle

**`require("lvgl")` does NOT initialize anything by itself** — it only loads
the module table. You must call `lv.start(...)` yourself before creating
widgets; nothing happens automatically on `require()`.

```lua
local lv = require("lvgl")

local ok, w, h = lv.start("display_lcd_rgb_st7701p", "touch_gt911")
-- 2nd arg (touch device id) is optional — omit it for a touch-less panel.
if not ok then
    print("lvgl busy or failed:", w)   -- err message is the 2nd return on failure
    return
end
-- `w`, `h` are the panel's resolution in pixels — read these, don't guess
-- from "common" LCD sizes; boards vary (this one is 480x480, not 480x320).

local btn = lv.button.create()          -- parent defaults to lv.screen_active()
local lbl = lv.label.create(btn)
lbl:set_text("Hello")
btn:set_size(120, 50)
btn:center()
btn:on_click(function()
    lbl:set_text("Clicked!")
end)

lv.run()          -- blocks: keeps the UI alive + dispatches callbacks
                  -- until lv.stop() is called (e.g. from a callback)
```

| API | Signature | Notes |
| --- | --- | --- |
| `lv.start(display_id[, touch_id])` | `start(id[, touch_id]) -> ok, w, h` | Both ids are board.json device ids (same ones `display.init`/`touch.init` take). `touch_id` is optional — omit for a display with no touch panel. On success returns `true, w, h` (panel resolution in pixels — use these instead of guessing). On failure returns `nil, err` (e.g. `"display busy: owned by another mode (display)"`). |
| `lv.stop()` | `stop()` | Tears down the widget tree, touch, and panel. Idempotent. A fallback releases everything even if your script never calls it — but call it explicitly. |
| `lv.run()` | `run()` | **Blocks** the calling script: keeps dispatching registered event callbacks until `lv.stop()` is called (typically from inside one of your own callbacks, or from a companion trigger). The UI itself (rendering, touch) keeps working even without this — only callbacks need it. |
| `lv.process_events(timeout_ms)` | `process_events(ms)` | Non-blocking-ish variant: dispatches whatever callbacks are already pending, then returns once nothing is pending or `timeout_ms` has elapsed. Call this repeatedly from your own loop if you need to do other work between UI events. |
| `lv.screen_active()` | `screen_active() -> obj` | The current screen object (same as the default `create()` parent). |

---

## Creating widgets

Every widget type has a `lv.<type>.create(parent)` factory. `parent` is
optional and defaults to `lv.screen_active()`.

```lua
local scr = lv.screen_active()
local bar = lv.bar.create(scr)
bar:set_range(0, 100)
bar:set_value(42, true)     -- true = animate
```

Available types: `obj` (plain container), `button`, `label`, `bar`, `slider`,
`switch`, `checkbox`, `arc`, `spinner`, `image`, `line`, `chart`, `dropdown`,
`roller`, `list`, `menu`, `msgbox`, `tabview`, `textarea`,
`keyboard`, `animimg`. This is the full set — no others are available.
**`table` is not usable right now** (see note above) — don't create one.

---

## Methods every widget shares

| Method | Notes |
| --- | --- |
| `:set_size(w,h)` / `:set_width(w)` / `:set_height(h)` | Pixels. |
| `:set_pos(x,y)` / `:set_x(x)` / `:set_y(y)` | Pixels, relative to parent. |
| `:set_align(align)` | See align strings below. |
| `:align(align[,xofs,yofs])` | Align within the **parent**. 2nd arg is always an align **string** — never pass a widget here. |
| `:align_to(other, align[,xofs,yofs])` | Align relative to **another widget** `other` (1st arg after self is that widget, 2nd is the align string). |
| `:center()` | Shortcut for `align("center")`. |
| `:add_flag(name)` / `:clear_flag(name)` / `:has_flag(name)` | `name` ∈ `"hidden"`, `"clickable"`, `"scrollable"`, `"click_focusable"`, `"checkable"`, `"ignore_layout"`. |
| `:add_state(name)` / `:clear_state(name)` / `:has_state(name)` | `name` ∈ `"checked"`, `"disabled"`, `"focused"`, `"pressed"`, `"edited"`. |
| `:delete()` | Removes the widget (and its children) from the screen. Any callbacks registered on it are dropped. |
| `:clean()` | Deletes all of this widget's children (keeps the widget itself). |
| `:set_parent(parent)` | Reparent. |
| `:move_foreground()` / `:move_background()` | Z-order among siblings. |
| `:get_size() -> w,h` / `:get_pos() -> x,y` | |
| `:get_parent() -> obj` | |
| `:get_child_count() -> n` / `:get_child(idx) -> obj` | `idx` is 0-based. |
| `:set_scroll_dir(dir)` | `dir` ∈ the **dir strings** below. Which directions this widget can be dragged/scrolled. |
| `:set_scrollbar_mode(mode)` | `mode` ∈ `"off"`, `"on"`, `"active"` (visible only while scrolling), `"auto"` (visible only when content overflows). |
| `:set_scroll_snap_x(align)` / `:set_scroll_snap_y(align)` | `align` ∈ `"none"`, `"start"`, `"end"`, `"center"`. Snaps scrolling to child boundaries on that axis. |

**Align strings:** `"top_left"`, `"top_mid"`, `"top_right"`, `"left_mid"`,
`"center"`, `"right_mid"`, `"bottom_left"`, `"bottom_mid"`, `"bottom_right"`.

**Dir strings** (used by `:set_scroll_dir()` and `dropdown:set_dir()`):
`"none"`, `"left"`, `"right"`, `"top"`, `"bottom"`, `"hor"` (= left+right),
`"ver"` (= top+bottom), `"all"`.

### Events

| Method | Notes |
| --- | --- |
| `:on(event_name, fn)` | `event_name` ∈ `"clicked"`, `"value_changed"`, `"pressed"`, `"released"`, `"long_pressed"`, `"focused"`, `"defocused"`, `"ready"`, `"cancel"`. `fn(obj, event_name)` is called from `lv.run()`/`lv.process_events()` — **never** synchronously inside `:on(...)` itself. |
| `:on_click(fn)` | Shortcut for `:on("clicked", fn)`. |
| `:on_value_changed(fn)` | Shortcut for `:on("value_changed", fn)`. Fires for bar/slider/switch/checkbox/dropdown/roller. |
| `:off(event_name)` | Unregister. |

At most **4** callbacks per widget (registering a 5th raises a Lua error —
call `:off()` first, or spread callbacks across separate widgets).

### Layout (flex / grid)

| Method | Notes |
| --- | --- |
| `:set_flex_flow(dir)` | `dir` ∈ `"row"`, `"column"`, `"row_wrap"`, `"column_wrap"`, `"row_reverse"`, `"column_reverse"`. |
| `:set_flex_align(main, cross, track)` | Each ∈ `"start"`, `"end"`, `"center"`, `"space_between"`, `"space_around"`, `"space_evenly"`. |
| `:set_flex_grow(n)` | Set on a **child** of a flex container. |
| `:set_grid_dsc(cols, rows)` | `cols`/`rows` are arrays; each entry is a pixel number, the string `"content"` (size to fit its content), or `"<N>fr"`/`"fr"` (fraction of remaining space, `"fr"` alone = `"1fr"`). E.g. `{100, "1fr", "content"}`. |
| `:set_grid_cell(col_align, col_pos, col_span, row_align, row_pos, row_span)` | Set on a **child** of a grid container. `col_align`/`row_align` are the raw `LV_GRID_ALIGN_*` integers (start=0, center=1, end=2, stretch=3). |

**Flex + content-sized children pitfall.** `table`, `list`, `chart` and a few
others default their width/height to "size to content" (`LV_SIZE_CONTENT`).
Inside a `flex_flow("column")` (or `"row"`) container, a content-sized child
with no `:set_flex_grow(n)` and no explicit `:set_size(...)` can get **shrunk
down to near-zero** by the flex algorithm when the container is shorter than
the sum of its children's natural sizes — the widget silently ends up with
~0 height and looks empty even though its cell/row data was set correctly.
Fix: either call `:set_flex_grow(1)` on that child, or give it an explicit
`:set_size(w, h)`, or don't put it inside a flex container at all (absolute
`:set_pos(...)` + explicit size works too). If a widget "has data but shows
nothing" inside a flex layout, check this first before suspecting the data.

### Styles

All take an optional trailing `(part, state)` pair — `part` ∈ `"main"`,
`"indicator"`, `"knob"`, `"items"` (default `"main"`); `state` ∈ the state
names above (default: the widget's default state).

| Method |
| --- |
| `:set_style_bg_color(0xRRGGBB[, part, state])` / `:set_style_bg_opa(0-255[, part, state])` |
| `:set_style_border_color(0xRRGGBB[, part, state])` / `:set_style_border_width(px[, part, state])` / `:set_style_border_opa(0-255[, part, state])` |
| `:set_style_radius(px[, part, state])` |
| `:set_style_text_color(0xRRGGBB[, part, state])` / `:set_style_text_opa(0-255[, part, state])` |
| `:set_style_text_font(px[, part, state])` — `px` ∈ `14, 20, 24, 26` only (same four fonts as `display`); any other value raises an error. |
| `:set_style_opa(0-255[, part, state])` — overall widget opacity (distinct from the per-part `_opa` setters above). |
| `:set_style_pad_all(px[, part, state])` — all 4 sides at once. `:set_style_pad_top/bottom/left/right(px[, part, state])` — one side. `:set_style_pad_hor/ver(px[, part, state])` — left+right, or top+bottom, together. |
| `:set_style_shadow_width(px[, part, state])` / `:set_style_shadow_color(0xRRGGBB[, part, state])` / `:set_style_shadow_opa(0-255[, part, state])` |
| `:set_style_line_color(0xRRGGBB[, part, state])` / `:set_style_line_width(px[, part, state])` / `:set_style_line_opa(0-255[, part, state])` |
| `:set_style_arc_color(0xRRGGBB[, part, state])` / `:set_style_arc_width(px[, part, state])` / `:set_style_arc_rounded(bool[, part, state])` / `:set_style_arc_opa(0-255[, part, state])` |

**No other `set_style_*` methods exist** — in particular there is no `outline_*`,
`transform_*`/`translate_*`, `margin_*`, `bg_grad_*`/`bg_image_*`,
`text_letter_space`/`text_line_space`/`text_decor`/`text_align`, `recolor`,
or `blend_mode`. These are real LVGL style properties but were deliberately
not bound (see `phase5_lvgl_full.md` §1 scope decisions) — don't guess at
them; every group above is the complete set for that property.

These mirror LVGL's real `lv_obj_set_style_*` C API 1:1 and are on the shared
method table like every other style setter — they work on any widget, not
just `arc` (LVGL just won't render an arc-only property on a widget that
doesn't draw an arc). If you know the C property name, `:set_style_<name>`
is almost always the right guess; a handful (`radius`, `opa`, `pad_all`) drop
the leading `lv_obj_` for brevity but otherwise follow the C name exactly.

**Commonly-used `part` per widget** (both parts always exist and accept any
style — these are just the ones actually visible):

| Widget | Parts |
| --- | --- |
| `bar` | `main` = track, `indicator` = fill |
| `slider` | `main` = track, `indicator` = filled range, `knob` = the draggable knob |
| `arc` | `main` = background arc, `indicator` = the value arc, `knob` = the draggable knob (if enabled) |
| `switch`/`checkbox` | `main` = the box/track, `indicator` = the fill/checkmark, `knob` = the switch's dot |
| `chart` | `main` = plot area/background; per-series colour is set on the series object, not a `part` |
| anything else | `main` only |

---

## Widget-specific methods

| Widget | Methods |
| --- | --- |
| `button` | None beyond the shared set — put a `label` (or other widget) inside it. |
| `label` | `:set_text(str)`, `:set_long_mode(mode)` (`mode` ∈ `"wrap"`, `"dot"`, `"scroll"`, `"scroll_circular"`, `"clip"`), `:set_recolor(bool)`. |
| `bar` | `:set_value(v, anim)`, `:set_start_value(v, anim)`, `:set_range(min, max)`, `:set_mode(mode)` (`"normal"`/`"symmetrical"`/`"range"`), `:get_value()`. `anim` is a boolean. |
| `slider` | `:set_value(v, anim)`, `:set_range(min, max)`, `:set_mode(mode)`, `:get_value()`. |
| `switch` | None beyond shared — use `:has_state("checked")` to read it. |
| `checkbox` | `:set_text(str)`. No `:set_recolor()` — that's a `label`-only LVGL feature, checkbox's internal label isn't exposed for inline `#RRGGBB text#` colouring. |
| `arc` | `:set_value(v)`, `:set_range(min, max)`, `:set_angles(start, end)`, `:set_bg_angles(start, end)`, `:set_rotation(deg)`, `:set_mode(mode)` (`"normal"`/`"reverse"`/`"symmetrical"`), `:get_value()`. |
| `spinner` | None — spins on its own once created. |
| `image` | `:set_src(str)` (built-in `LV_SYMBOL_*` glyph or short text only — no bitmap loading), `:set_rotation(deg)`, `:set_scale(zoom)` (256 = 100%), `:set_offset(x, y)`. |
| `line` | `:set_points({x1,y1, x2,y2, ...})`, `:set_y_invert(bool)`. |
| `chart` | `:set_type(type)` (`"line"`/`"bar"`/`"scatter"`), `:set_point_count(n)`, `:set_range(axis, min, max)` (`axis` ∈ `"primary_y"`,`"secondary_y"`,`"primary_x"`,`"secondary_x"`), `:add_series(0xRRGGBB[, axis]) -> series`, `:refresh()`. `series` is its own small object: `series:set_next_value(v)`, `series:set_all_values(v)`, `series:set_values({v1,v2,...})` (batch-writes the whole series in one call — the table length must equal the chart's `:set_point_count(n)`), `series:set_color(0xRRGGBB)`. |
| `dropdown` | `:set_options(str)` (`\n`-separated), `:add_option(str, pos)`, `:set_selected(idx)`, `:get_selected() -> idx`, `:get_options() -> str`, `:set_dir(dir)` (dir strings above — which side the list drops down towards), `:set_symbol(str_or_nil)` (the dropdown arrow/icon glyph, e.g. a `LV_SYMBOL_*` string; call with no arg / `nil` to remove it). |
| `roller` | `:set_options(str[, mode])` (`mode` ∈ `"normal"`/`"infinite"`), `:set_selected(idx, anim)`, `:get_selected() -> idx`, `:get_selected_str() -> str`, `:set_visible_row_count(n)`. |
| `list` | `:add_text(str) -> label_obj`, `:add_button(str[, icon]) -> button_obj` (`str` first — `icon` is the optional trailing arg, a `LV_SYMBOL_*` string; omit it for plain text). |
| `menu` | `:page_create([title]) -> page_obj`, `:set_page(page_obj)`, `:set_sidebar_page(page_obj)`. Minimal subset — enough for a simple settings-style menu. |
| `msgbox` | `:add_title(str)`, `:add_text(str)`, `:add_close_button()`, `:add_footer_button(str) -> button_obj`, `:close()`. |
| `tabview` | `:add_tab(name) -> content_obj`, `:set_active(idx, anim)`, `:get_tab_active() -> idx`. The tab-bar (buttons) and content area are internal LVGL implementation detail with no stable child index — don't rely on `:get_child(0)`/`:get_child_count()` on a `tabview` itself to reach them; style the `content_obj` each `:add_tab()` returns instead. |
| `textarea` | `:set_text(str)`, `:add_text(str)`, `:get_text() -> str`, `:set_placeholder_text(str)`, `:set_password_mode(bool)`, `:set_one_line(bool)`, `:set_max_length(n)`. |
| `keyboard` | `:set_textarea(ta_obj)` (binds so key presses type into that textarea), `:set_mode(mode)` (`"text_lower"`/`"text_upper"`/`"special"`/`"number"`). |
| `animimg` | `:set_src({str1, str2, ...}[, duration_ms, repeat_count])`, `:start()`. |

**Not available** (by design — see design_spec if you think you need one):
`buttonmatrix`, `calendar`, `canvas` (use `display` for a command-style
canvas), `imagebutton`, `led`, `spangroup`, `spinbox`, `tileview`, `window`.
**`table` also not usable right now** — bound but its cell text doesn't
render (open bug, see the note near the top of this doc). Build tables out
of manual row containers instead:

```lua
-- table-like grid, simulated with row containers (workaround for the
-- lv.table text-render bug — use this pattern instead of lv.table)
local function mk_row(parent, cols, widths, color)
    local row = lv.obj.create(parent)
    row:set_size(widths.total, 28)
    row:set_flex_flow("row")
    row:set_style_bg_opa(0, "main")
    row:set_style_border_width(0, "main")
    row:set_style_pad_all(0, "main")
    for i, txt in ipairs(cols) do
        local l = lv.label.create(row)
        l:set_text(txt)
        l:set_width(widths[i])
        l:set_style_text_color(color, "main")
    end
    return row
end

local widths = {total = 280, 60, 120, 100}
mk_row(parent, {"ID", "Name", "Status"}, widths, 0xE040FB)   -- header
mk_row(parent, {"01", "Sensor-A", "Active"}, widths, 0xFFFFFF)
mk_row(parent, {"02", "Sensor-B", "Idle"}, widths, 0xFFFFFF)
```

### Minimal examples (chart / tabview)

```lua
-- chart: one line series, updated in place
local chart = lv.chart.create()
chart:set_type("line")
chart:set_point_count(10)
chart:set_range("primary_y", 0, 100)
local s1 = chart:add_series(0xFF0000)          -- axis defaults to "primary_y"
for i = 1, 10 do s1:set_next_value(50) end     -- seed initial data
chart:refresh()
-- later: s1:set_next_value(new_val); chart:refresh()

-- tabview: two tabs, put widgets inside the returned content object
local tv = lv.tabview.create()
local tab1 = tv:add_tab("Home")
local tab2 = tv:add_tab("Settings")
lv.label.create(tab1):set_text("Hello from tab 1")
lv.label.create(tab2):set_text("Hello from tab 2")
```

---

## Most common mistake: forgetting `lv.run()` / `lv.process_events()`

Widgets render and respond to touch **without** calling either — but your
`:on_click`/`:on(...)` callbacks are only *queued*, never run, until the
script calls `lv.run()` (blocking) or `lv.process_events(ms)` (repeatedly, in
your own loop). Symptom: buttons visibly press but "nothing happens". Fix: at
minimum, end your script with `lv.run()`, or call
`lv.process_events(50)` inside whatever loop you already have.

## Periodic updates (e.g. refreshing a chart every second)

There is no `lv.timer`. For UI that needs to update itself on a schedule
(chart data, a clock label, polling a sensor into a bar), drive it from your
own script thread with `sys.millis()` + `lv.process_events()` — this is the
correct pattern, not a workaround, and it is the ONE exception to "don't
write `while true`" (that guidance is about `display`'s per-frame redraw
loop, not about this):

```lua
local lv  = require("lvgl")
local sys = require("sys")
-- ... build widgets, call lv.start() ...

local last = sys.millis()
while true do
    lv.process_events(100)              -- dispatch pending callbacks, wait up to 100ms
    local now = sys.millis()
    if now - last >= 1000 then
        last = now
        series:set_next_value(read_sensor())
        chart:refresh()
    end
end
```

Do **not** use `require("timer")` for this: its callback runs as a CODE
STRING in a *separate* fresh Lua state (see `timer.md`) — it cannot see your
widget locals (`chart`, `series`, ...), so it cannot update them.
`timer`/`basictimer` are for standalone periodic tasks unrelated to a live
widget tree (GPIO polling, scheduled jobs); for anything that touches
widgets you already created, use the loop above instead.

## `display` vs `lvgl`

Use `display` for games/animations/pixel-level drawing (you redraw every
frame). Use `lvgl` for dashboards/control panels (you declare widgets once).
They cannot run at the same time — starting one while the other owns the
screen fails with `nil, "display busy: owned by another mode (...)"`.

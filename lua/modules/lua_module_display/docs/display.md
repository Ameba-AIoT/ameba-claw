# display — `require("display")`

Command-style 2D drawing canvas for the on-board SPI LCD (ST7789, 240×240).
Built for **games, animations and simple dashboards**. You draw shapes/text
into a frame and call `present()` to push it to the screen.

- **No handle, no pins, no SPI setup.** All hardware config comes from
  `board.json`; you only pass a device id to `init`.
- **Colours are integers `0xRRGGBB`** (24-bit), screen/depth independent.
  A colour is a *number*, never a string and never `{r,g,b}`.
- **Single owner.** Only one script can hold the display at a time. `init`
  fails (returns `nil,err`) if another script already owns it.

---

## Lifecycle (call once, outside your loop)

**`require("display")` does NOT initialize anything by itself** — it only
loads the module table. You must call `d.init(id)` yourself before drawing.

```lua
local d = require("display")

local ok, err = d.init("display_lcd_spi_st7789")  -- returns ok, err
if not ok then
    print("display busy or failed:", err)
    return                       -- ALWAYS check — do not draw if init failed
end

print(d.width, d.height)         -- 240, 240 (read-only, from board.json)

d.backlight(true)                -- backlight on/off (true/false)

-- ... your frame loop ...

d.deinit()                       -- release the panel when done
```

| API | Signature | Notes |
| --- | --- | --- |
| `d.init(id)` | `init(board_dev_id) -> ok, err` | Bind the panel. `id` is a board.json display device id, e.g. `"display_lcd_spi_st7789"`. Returns `true` on success, or `nil, err` if the display is already owned or on error. **Check the return value.** |
| `d.deinit()` | `deinit()` | Release the panel. Idempotent. If your script exits without calling it, a fallback still releases the panel — but **call it explicitly** so other scripts can use the screen. |
| `d.width` / `d.height` | read-only fields | Screen size in pixels. Use these to lay out — don't hard-code 240. |
| `d.backlight(on)` | `backlight(bool)` | Turn the backlight on/off. |

---

## Frames

```lua
d.begin_frame({clear = true, color = 0x000000})  -- start a frame
-- ... draw calls ...
d.present()                                        -- render + push to screen
```

| API | Signature | Notes |
| --- | --- | --- |
| `d.begin_frame(opts)` | `begin_frame({clear=true, color=0x000000})` | Start a frame. `clear=true` (default) wipes to `color` first — the safe path, redraw everything each frame. `clear=false` keeps the previous frame's pixels (draw only what changed). `opts` is optional. |
| `d.present()` | `present()` | Render the frame and push it to the panel. **Synchronous — when it returns, the pixels are on the screen.** |
| `d.present_full()` | `present_full()` | Same as `present()` (full-screen push). |
| `d.present_rect(x,y,w,h)` | `present_rect(x, y, w, h)` | Push **only** the given rectangle to the panel — much faster than a full push when little changed (transfer time scales with the rect area). Use with `clear=false`: redraw only the region that changed, then `present_rect` its bounding box. Coordinates are clamped to the screen; an off-screen/empty rect is a no-op. |
| `d.end_frame()` | `end_frame()` | Optional no-op hook, reserved. |
| `d.fps()` | `fps() -> number` | Frames per second measured from the **last** `present()`. `0` before the first present. Handy for an on-screen counter: `d.draw_text(4, 4, "FPS " .. d.fps(), 0x00FF66)`. |
| `d.frame_ms()` | `frame_ms() -> number` | Milliseconds the DMA push step of the last `present()` took (the SPI/LCDC transfer time). |
| `d.render_ms()` | `render_ms() -> number` | Milliseconds the AA-rasterisation step of the last `present()` took (`flush_layer` CPU cost). If `render_ms` is large the frame is CPU-bound; if `frame_ms` is large the frame is DMA/push-bound. Only updated by `present()`, not `present_rect()`. |

You may also call draw functions without `begin_frame` — a frame is opened
lazily. But for clarity always `begin_frame` → draw → `present`.

---

## Drawing primitives

All coordinates are integers in pixels, origin top-left. All colours are
`0xRRGGBB` integers. Calling a draw function before `init` raises an error.

### Fill / clear
```lua
d.clear(color)                          -- fill whole screen
d.fill_rect(x, y, w, h, color)
```

### Rectangles
```lua
d.draw_rect(x, y, w, h, color[, thickness])       -- outline (thickness default 1)
d.fill_round_rect(x, y, w, h, radius, color)
d.draw_round_rect(x, y, w, h, radius, color[, thickness])
```

### Points / lines
```lua
d.draw_pixel(x, y, color)
d.draw_line(x0, y0, x1, y1, color[, width])       -- width default 1
d.draw_points(tbl [, color])                       -- batch pixel plot (direct, no AA)
```
- With a `color` argument: `tbl` is `{x1,y1, x2,y2, …}` — uniform colour for all points.
- Without `color`: `tbl` is `{x1,y1,c1, x2,y2,c2, …}` — each point carries its own `0xRRGGBB`.
- One Lua↔C crossing + one layer flush for the whole batch. Use for particle/star trails instead of repeated `draw_pixel` calls.

### Circles / ellipses / arcs (angles in degrees, 0° = 3 o'clock, clockwise)
```lua
d.fill_circles(tbl)                                          -- solid circle(s), anti-aliased
d.fill_ellipses(tbl)                                         -- solid ellipse(s), anti-aliased
d.draw_circle(cx, cy, r, color[, width])                     -- circle ring outline
d.draw_ellipse(cx, cy, rx, ry, color[, width])               -- ellipse outline, anti-aliased
d.draw_arc(cx, cy, r, start_deg, end_deg, color[, width])
d.fill_arc(cx, cy, r_out, r_in, start_deg, end_deg, color)   -- filled ring sector
```

**`fill_circles` is the one and only filled-circle primitive — for one circle or a thousand.**
It takes a **flat array** `{cx1, cy1, r1, color1, cx2, cy2, r2, color2, …}` — **4 values
per circle**, colour is `0xRRGGBB` (24-bit integer). To draw a single circle just pass a
one-element batch:

```lua
d.fill_circles({120, 120, 30, 0xFFD700})              -- ONE gold ball, cx,cy,r,color
d.fill_circles({x1,y1,3,0xFFFFFF,  x2,y2,3,0xFFFFFF}) -- two 3px white dots, one call
```

- **Anti-aliased.** Edges are smooth (coverage-blended rim); the same quality whether you
  pass 1 circle or 100. There is **no** separate "smooth" vs "fast" circle call to choose
  between — this is it.
- **Batch everything into ONE call.** All circles you draw this frame should go in a single
  `fill_circles{...}` — one Lua↔C crossing and one layer flush for the whole set. Don't loop
  calling it once per circle; build the flat array and pass it once.
- **Cost scales with pixels, not call count.** The interior is a bare span-fill; only the
  thin rim is blended. Hundreds of small particles per frame are cheap.

> There is **no** `fill_circle` (singular) — use `fill_circles` with a 4-value array.

**Ellipses (`fill_ellipses` / `draw_ellipse`).** Axis-aligned ovals — use these when a shape
is wider than it is tall (or vice-versa): planet rings, eyes, shadows under a sprite, health
bars with rounded caps, squash-and-stretch animation. `rx` is the horizontal radius, `ry` the
vertical; `rx == ry` gives exactly the same result as the matching circle.

```lua
d.fill_ellipses({cx, cy, rx, ry, color})                    -- ONE solid ellipse
d.fill_ellipses({80,60,30,18,0xFF8800,  160,60,12,28,0x33AAFF})  -- two ovals, one call
d.draw_ellipse(120, 120, 60, 30, 0x00FF66)                  -- outline, 1px
d.draw_ellipse(120, 120, 60, 30, 0x00FF66, 3)               -- outline, 3px stroke
```

- **`fill_ellipses` is the batch solid-ellipse primitive**, exactly like `fill_circles` but
  with a **flat array of 5 values per ellipse**: `{cx, cy, rx, ry, color, …}`. One circle or a
  thousand ovals go in a single call (one Lua↔C crossing + one flush). Anti-aliased rim, opaque
  span-filled interior — same cost model as `fill_circles`.
- **`draw_ellipse(cx, cy, rx, ry, color[, width])`** draws the outline only, anti-aliased,
  optional stroke `width` (default 1). It's the ellipse analogue of `draw_circle`; it is a
  **single-shape** call (no batch form), so if you stroke many ovals call it per shape.
- Both auto-floor float coords/radii. Degenerate `rx==0` or `ry==0` collapses to a 1px line.

### Triangles
```lua
d.fill_triangle(x0, y0, x1, y1, x2, y2, color)
d.draw_triangle(x0, y0, x1, y1, x2, y2, color)    -- outline (3 lines)
```

### Text
```lua
d.draw_text(x, y, str, fg [, bg] [, font_px])
d.draw_text_aligned(x, y, w, h, str, fg [, align])  -- align: "left"|"center"|"right"
local w, h = d.measure_text(str [, font_px])
```
- **Fonts are fixed sizes: 14, 20, 24, 26 px** (`font_px` snaps to the nearest
  available size; default 14). There is **no free scaling** to arbitrary sizes.
- **ASCII only.** Chinese / CJK characters are **not** available (they render as
  empty boxes). Phase 1 ships no CJK font.
- `bg` (optional) draws a solid background box behind the text.

### Clipping
```lua
d.set_clip_rect(x, y, w, h)   -- restrict subsequent draws to this rectangle
d.clear_clip_rect()           -- remove the clip
```

### Raw pixels
```lua
d.draw_pixels(x, y, w, h, buf, format)   -- blit a raw RGB565 block
```
- `buf` is a binary string of `w*h` 16-bit pixels.
- `format`: `"rgb565"` (big-endian, the panel's native order) or `"rgb565le"`
  (little-endian, byte-swapped on the way in).
- Unlike the vector primitives, `draw_pixels` writes immediately into the
  canvas; it composites at the next `present()`.

### Images (JPEG)
```lua
local w, h = d.draw_image(x, y, path [, target_w, target_h])
```
- Hardware-decodes a **JPEG** file (`path`, e.g. `"vfs:/photo.jpg"`) and draws it
  at `(x, y)`. Returns the on-screen size actually drawn (`w, h`), or `nil, err`.
- **`target_w` / `target_h` scale the picture DURING decode** (the SoC's JPEG
  post-processor). Sizing rules:
  - **omit both** (or `0`) → keep the source size.
  - **give both** → exactly that size. May distort: e.g. a 1280×720 photo forced
    to `480,480` gets squished horizontally. Use only for a deliberate stretch or
    when the source already matches the target aspect.
  - **give only one** → the other axis is derived from the source aspect ratio.
    So `d.draw_image(0, 0, "vfs:/bg.jpg", d.width)` fits the photo to the screen
    width without distortion (a 1280×720 photo → 480×270, letterboxed).
  Scaling during decode means a large photo is **never held in RAM at full
  resolution**. Aspect is only preserved automatically in the one-axis case;
  scaling one axis up while the other goes down is rejected either way.
- **JPEG only** (baseline). PNG/BMP/GIF are not supported. Non-JPEG or a missing
  file returns `nil, err` (check it).
- **Draw the image FIRST, then rects/text on top.** Like `draw_pixels`,
  `draw_image` writes straight into the frame immediately; vector primitives
  (rect/text/…) composite on top at `present()`. So the pattern is:
  `begin_frame` → `draw_image(background)` → `draw_rect`/`draw_text`(labels) →
  `present`.
- Best supported on the **LCDC/RGB** panel (native RGB565). It also works on the
  SPI/ST7789 panel (bytes are swapped for you), just without hardware
  acceleration.

For raw (already-decoded) RGB565 blocks, use `draw_pixels` instead.

### Annotating a photo (boxes + labels)

To draw boxes over a photo (e.g. from `vision_describe`), work in **normalized
coordinates**, never raw pixels — the photo resolution, the screen size, and the
on-screen image rectangle are all different, so pixel guesses land in the wrong
place. A vision model returns each object as a normalized box `[x0,y0,x1,y1]`
(each 0..1, origin = top-left of the *image*, x right / y down).

`vision_describe` returns `{"description":"<text>"}`, and when you asked it to
locate objects the box array `[{"label":..,"box":[x0,y0,x1,y1]}]` is **embedded
inside that `description` text** — read the `description` field and parse the
JSON array out of it (it is not a separate return field).

Map a normalized box onto the rectangle where you **actually drew** the image.
`draw_image` returns the real drawn size `(iw, ih)`, and you chose the draw
origin `(ix, iy)`:

```lua
local ix, iy = 0, 0
local iw, ih = d.draw_image(ix, iy, "vfs:/photo.jpg", d.width)  -- fit to width, aspect kept
-- box = {x0,y0,x1,y1} normalized 0..1 from the vision model:
local rx = ix + x0 * iw
local ry = iy + y0 * ih
local rw = (x1 - x0) * iw
local rh = (y1 - y0) * ih
d.draw_rect(rx, ry, rw, rh, 0xFF0000, 2)   -- box in the SAME rect as the image
d.draw_text(rx, ry - 16, label, 0xFF0000)  -- label above the box
```

Because both the image and the boxes are placed against `(ix, iy, iw, ih)`, the
annotation stays aligned regardless of camera resolution or screen size. Draw
the image first, then the rects/text on top, then `present()`.

---

## Typical game loop

```lua
local d   = require("display")
local sys = require("sys")

local ok, err = d.init("display_lcd_spi_st7789")
if not ok then print("no display:", err); return end

local x = 0
while true do
    d.begin_frame({clear = true, color = 0x000000})
    d.fill_circles({x, 120, 12, 0xFFD700})       -- yellow ball (cx,cy,r,color)
    d.draw_text(4, 4, "X=" .. x, 0x00FF66)       -- HUD text
    d.present()                                   -- returns when on-screen

    x = (x + 4) % d.width
    -- NO sleep here — present() already blocked ~20 ms and yielded the CPU.
end
-- d.deinit() on exit (or the __gc fallback releases it)
```

> ⛔ **Do NOT put `sys.sleep_ms(...)` in a loop that calls `present()` every
> iteration** (games, animations, interactive UI). `present()` is *synchronous*
> and its ~20 ms vsync/DMA wait is **interrupt-driven — it already yields the CPU
> to WiFi / the agent** while the frame is pushed. This SoC is slow and the LCDC
> refresh itself takes real time, so any extra `sleep_ms` is pure dead time
> **subtracted straight from your frame budget** — a `sleep_ms(33)` roughly halves
> your fps for zero benefit. The push is your frame pacing; let it be.
>
> The **only** time you need a small `sleep_ms(5)` is a loop that may spin
> **without** calling `present()` — e.g. polling a button while the screen is
> static, or a `clear=false` frame where nothing changed and you skip the push.
> Then a tiny sleep yields the CPU that `present()` would otherwise have yielded
> for you. **If you present every frame, do not sleep.**

## Performance notes (measure, don't guess)

A full-screen `present()` costs roughly **~20 ms** and this push **dominates the
frame** — it is a fixed cost set by the SPI transfer, not by how much you drew.

- **Drawing is cheap.** Several shapes (circles, text, a trail) render in
  **< 2 ms** combined. Don't contort your drawing code for speed — the push, not
  the primitives, is the ceiling (~40–50 fps full-screen).
- **`clear=false` is not a speedup by itself.** `clear=true` vs `clear=false`
  differ by only ~1–2 ms, because `present()` still pushes the whole screen
  either way. Use `clear=false` only when you genuinely want to *keep* previous
  pixels and repaint just the parts that changed.
- **`sleep_ms` in a present-every-frame loop only hurts.** The ~20 ms push
  already yields the CPU (interrupt-driven vsync wait), so the sleep buys nothing
  and its whole duration is subtracted from your fps. Don't add it (see rule 3).
- **Profile with `d.frame_ms()`** to find where time goes before optimizing;
  intuition about "clear is slow" or "drawing is slow" is usually wrong here.

**Incremental drawing (`clear=false`).** To animate without a full wipe, per
frame: **erase all old pixels first, then draw all new ones** — if you interleave
them, freshly drawn pixels get erased. Also restore any background/effects that
a moved object was covering.

**The real speedup: `present_rect`.** `present()` always pushes the whole screen
(~20 ms) no matter how little changed. To actually go faster, redraw only the
changed region (`clear=false`) and push just its bounding box with
`d.present_rect(x, y, w, h)` — transfer time scales with the rect area, so a
small moving sprite can run far above the full-screen ceiling. Track the union
of "where it was" + "where it is" as the rect to push.

## Hard rules (read these)

1. **Check `init`'s return.** `local ok,err = d.init(id)` — if `nil`, stop; the
   display is owned by someone else or out of memory. Drawing anyway = blank
   screen or crash.
2. **`init` / `deinit` go OUTSIDE the loop** (once each). The loop body is only
   `begin_frame → draw → present → sleep`.
3. **Don't `sleep_ms` in a loop that presents every frame.** `present()` is
   synchronous and its ~20 ms push is interrupt-driven — it **already yields the
   CPU** to WiFi / the agent, so it *is* your frame pacing. An extra `sleep_ms`
   is dead time cut straight out of your fps (a `sleep_ms(33)` roughly halves it).
   Add a small `sleep_ms(5)` **only** in a loop that can iterate *without*
   presenting (polling input on a static screen, or skipping the push when
   nothing changed) — there the sleep supplies the yield `present()` otherwise
   would. See the boxed rule under "Typical game loop".
4. **Colours are integers `0xRRGGBB`.** `0xFF0000` = red. Not `"red"`, not
   `"#ff0000"`, not `{255,0,0}`.
5. **Don't `deinit` (or let the script end) right after `present`.** The image
   only stays while the display session is alive — end immediately and it just
   flashes once. Keep the loop / sleep running to hold a static screen.
6. **Coordinates may be floats** — they are auto-floored to whole pixels, so you
   don't need `math.floor()` on physics results. (Colours must still be
   integers, `0xRRGGBB`.)
7. **Avoid per-frame table allocation** in tight loops to prevent GC stutter:
   reuse fixed-size arrays with a ring index (not `table.insert`/`remove`),
   precompute per-object colours once, and keep vector math in locals.

## Concurrency & resources

- One owner at a time. A second `init` while the display is held returns
  `nil, "display already in use by another session"`.
- The full-screen canvas costs ~112 KB of heap plus a smaller draw buffer;
  `init` returns an out-of-memory error rather than crashing if the heap is
  too fragmented.
- Input (buttons / touch) is **not** part of `display`; use the `gpio` /
  `button` drivers for controls.

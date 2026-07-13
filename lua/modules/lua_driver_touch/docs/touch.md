# touch  —  require("touch")

Capacitive **touch-panel** input for the GT911 controller on the st7701p
480×480 LCD (I²C + INT). `require("touch")` returns a flat function table (no
handle/object) — there is only one panel. Coordinates are already rotated to
match what you draw with `require("display")`, so `(0,0)` is the top-left of the
screen and `(width-1, height-1)` the bottom-right.

> **Not the same as `captouch`.** `require("captouch")` drives the RTL8721F's
> on-chip self-capacitance touch **keys** (`captouch.new(pin)`). This module,
> `touch`, is the GT911 touch **screen**. They are unrelated.

The panel and its pins/address are described in `board.json` (device id
`"touch_gt911"`); you pass only that id — never wiring. Pair it with the
`display` module (`display.init("display_lcd_rgb_st7701p")`) to build touch UIs
and games.

## Model

A background reader (woken on every touch interrupt) turns the raw contact into
a **queued stream** of discrete events; `get_event()` pops them:

| `ev.type` | When | `dx,dy` |
|-----------|------|---------|
| `"down"`  | finger first touches the panel | `0, 0` |
| `"move"`  | finger slides ≥ a few px from the last report | delta since last event |
| `"up"`    | finger lifts | `0, 0` |

`get_event()` is **always non-blocking** and returns **one** event table, or
`nil` when the queue is empty. Because events are queued, **drain them in a loop
each frame** (call until `nil`) so a burst of movement is fully processed:

```lua
while true do
  local ev = t.get_event()
  if not ev then break end
  handle(ev)
end
```

Capture happens in the reader, not in `get_event()`, so a fast tap that begins
and ends between two frames is **not lost** — you'll still see its `down`+`up`
on the next drain, even at low frame rates. Tiny sub-pixel jitter while a finger
is held still is filtered (no `move` spam). Only the **first** touch point is
reported (no multi-touch); most games and UIs only need one finger.

## API

```lua
local t = require("touch")

-- Bring up the panel: reads pins/address/resolution from board.json device `id`,
-- runs the GT911 reset sequence, configures I²C + the INT interrupt.
-- Returns true, or (nil, errmsg) — ALWAYS check it.
local ok, err = t.init("touch_gt911")
if not ok then return err end

-- Pull ONE state-change event (non-blocking). Returns a table or nil.
local ev = t.get_event()

-- Configured panel resolution (valid after init).
local w = t.width()      -- e.g. 480
local h = t.height()     -- e.g. 480

-- Release the INT interrupt + I²C and hold the panel in reset.
t.deinit()
```

### Event table

`get_event()` returns one table (or `nil`):

```lua
{ type = "down"|"move"|"up", x = <int>, y = <int>, dx = <int>, dy = <int> }
```

- `x, y` — current contact position in **screen pixels** (`0..width-1`,
  `0..height-1`), same coordinate space as `display` drawing calls.
- `dx, dy` — movement since the previous event; `0` for `down`/`up`.

The shape is fixed — every field is always present, no need to test for `nil`
members.

## Patterns

**Finger-follow drawing (pair with `display`)**
```lua
local t = require("touch")
local d = require("display")
assert(t.init("touch_gt911"))
assert(d.init("display_lcd_rgb_st7701p"))
while true do
  local dirty = false
  for ev in function() return t.get_event() end do   -- drain this frame's events
    if ev.type ~= "up" then                          -- down/move → paint under finger
      d.fill_circles({ev.x, ev.y, 8, 0xFF4400}); dirty = true
    end
  end
  if dirty then d.present() end
  sys.sleep_ms(16)
end
```

**Game loop — tap to act**
```lua
local t = require("touch")
assert(t.init("touch_gt911"))
while running do
  local ev = t.get_event()
  while ev do                                         -- drain all queued input
    if ev.type == "down" then player_jump(ev.x, ev.y) end
    ev = t.get_event()
  end
  update_physics(); draw_frame(); sys.sleep_ms(16)
end
```

**Detect a tap vs a swipe yourself**
```lua
-- Accumulate dx/dy between down and up; small total = tap, large = swipe.
local sx, sy, moved
local ev = t.get_event()
if ev then
  if ev.type == "down" then sx, sy, moved = ev.x, ev.y, 0
  elseif ev.type == "move" then moved = (moved or 0) + math.abs(ev.dx) + math.abs(ev.dy)
  elseif ev.type == "up" then
    if (moved or 0) < 20 then on_tap(ev.x, ev.y) else on_swipe(sx, sy, ev.x, ev.y) end
  end
end
```

## Gestures — `rolfs:/lib/gesture.lua`

For the common tap / long-press / swipe vocabulary you don't have to write the
state machine above by hand — load the bundled recogniser and feed it the raw
events. It ships as a blessed library in `rolfs:/lib/`, so load it the same way
as any other module: `require("gesture")` (do **not** `dofile` it — the blessed
`require` uses a VFS-safe loader; a raw `dofile` of a `rolfs:` path fails):

```lua
local gesture = require("gesture")
local t       = require("touch")
assert(t.init("touch_gt911"))

local g = gesture.new()          -- opts: {long_press_ms=1000, swipe_min=30, now=fn}

local function handle(gz)
  if gz.kind == "tap"        then on_tap(gz.x, gz.y) end
  if gz.kind == "long_press" then on_hold(gz.x, gz.y) end
  if gz.kind == "swipe"      then on_swipe(gz.dir) end   -- "up"/"down"/"left"/"right"
end

while true do
  local saw = false
  for ev in function() return t.get_event() end do   -- drain ALL of this frame's events
    saw = true
    local gz = g:feed(ev)                            -- feed EACH event to the recogniser
    if gz then handle(gz) end
  end
  if not saw then                                    -- queue was empty this frame...
    local gz = g:feed(nil)                           -- ...still tick it so long_press can fire
    if gz then handle(gz) end
  end
  sys.sleep_ms(16)
end
```

`g:feed(ev)` takes one event (or `nil`) and returns **at most one** gesture table
per call, or `nil`:

| `gz.kind`      | Fields | Meaning |
|----------------|--------|---------|
| `"tap"`        | `x, y` | short contact that barely moved |
| `"long_press"` | `x, y` | held ≥ `long_press_ms` (1000) without moving — fires **while still held**, not on release |
| `"swipe"`      | `x, y, dx, dy, dir` | released after travelling ≥ `swipe_min` (30 px); `dir` is the dominant direction |

- 🔴 **`feed()` and the drain loop go together — feed EVERY event, and tick
  once with `nil` on empty frames.** Because events are queued (see *Model*),
  each frame you must drain the queue and pass **each** popped event to `feed()`;
  do **not** drain the events and then feed a single `nil` — that throws away the
  `down`/`up` the recogniser needs (a common mistake). Only when the queue is
  empty this frame do you call `feed(nil)` once, so the wall-clock long-press
  deadline can still fire while the finger is held still (no new events arrive).
- There is **no double-tap** — at ~10-40 fps a reliable double-tap window is hard
  to hit, so it's deliberately omitted; use two taps or a long-press instead.
- Call `g:reset()` after `deinit()`/`init()` to discard a stroke in progress.
- Only one gesture per `feed()`. A long-press that is then dragged past
  `swipe_min` still yields a `swipe` on release (the long_press already fired).

## Notes & limits

- 🔴 **Always check `init`'s return** (`local ok, err = t.init(id)`). It fails if
  the panel is busy, the board.json device is missing, or the GT911 does not ACK
  on I²C — drawing after a failed init reads uninitialised state.
- 🔴 **Call `get_event()` only after a successful `init`** — it errors otherwise.
- **Single panel, single consumer.** There is one screen; run touch from one
  loop. Call `deinit()` when you're done so another script can take over.
- **Do not use `touch` and the full LVGL widget UI at the same time** — both
  drive the same I²C bus + INT pin and are mutually exclusive (one owns the
  display session at a time).
- **Drain each frame.** Events are captured in the background and queued, so no
  input is lost between frames — but `get_event()` returns only one at a time.
  Call it in a loop until `nil` each frame, then sleep (~16 ms is a good cadence).
- **Release is robust.** If the panel ever misses the "finger lifted" interrupt,
  the driver synthesises an `up` after a short idle timeout so you never get
  stuck in a pressed state.
- Thresholds (move jitter filter, release timeout) are fixed in firmware
  (`ameba_claw_defs.h`).

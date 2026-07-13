# button  —  require("button")

Interrupt-driven push-button events for the RTL8721F, layered over `gpio`.
Debounce is done in hardware (the GPIO debounce filter, ~8.2 ms); the
click/double/long-press/hold discrimination happens in C — your Lua code never
manages timers or bounce. `require("button")` returns a flat function
table (no handle/object). Pin names: `PA_0..PA_31`, `PB_0..PB_31`, `PC_0..PC_8`
(e.g. `"PA_22"`).

The first `button.on(pin, ...)` call **implicitly takes over** that pin and
configures the hardware (BOTHEDGE interrupt, internal pull-up, active-low). A pin
owned by `button` no longer produces raw `gpio.on` events — the two are mutually
exclusive per pin.

## Event types

| Tier | `ev.type` | When |
|------|-----------|------|
| raw (opt-in) | `"down"` | press confirmed |
| | `"up"` | release confirmed |
| semantic | `"click"` | **emitted the instant the button is released — zero added latency** |
| | `"double"` | appended after a 2nd click lands within the double-gap window |
| | `"long_press"` | held past the long threshold (fires once) |
| | `"hold"` | repeats every ~200 ms while still held, after `long_press` |

Thresholds are fixed in firmware (`ameba_claw_defs.h`): hardware debounce
~8.2 ms, long 1500 ms, double-gap 300 ms, hold-repeat 200 ms, active-low.

### Click latency — the eager (mouse) model

`click` does **not** wait for the double-gap window. It fires immediately on
release (after the ~8.2 ms hardware debounce only), exactly like a PC mouse: Windows delivers a
single click on `WM_LBUTTONUP` and *then* adds `WM_LBUTTONDBLCLK` if a second
click follows. We do the same:

```
single click :  down → up → click                       (click is instant)
double click :  down → up → click → down → up → double   (click THEN double)
long press   :  down → long_press → hold → … → up        (no click)
```

Consequences you must design for:

- **A double-click always delivers a `click` first**, then `double`. If you
  subscribe both, make the actions composable — e.g. single = step, double =
  reset — or treat `double` as an *upgrade/undo* of the click you just handled.
  If single and double must be mutually exclusive, debounce it yourself in Lua
  (start a 300 ms timer on `click`, cancel it if `double` arrives).
- **If a pin does NOT subscribe `double`**, the window is skipped entirely:
  `click` fires on release and the FSM returns to idle at once. So a plain
  `btn.on(pin, "click")` is the lowest-latency path — use it when you don't
  need double-click discrimination.
- `long_press` still suppresses `click` (a held button never released quickly
  is not a click). `down`/`up` are opt-in raw brackets at different instants.

Subscribe only the types you want — unsubscribed types never queue.

## API

```lua
local btn = require("button")

-- Subscribe (and optionally register a callback)
btn.on(pin, type)            -- subscribe only; pull events with get_event/events
btn.on(pin, type, fn)        -- subscribe + register fn (call btn.dispatch to fire it)
                             -- → true, or (nil, errmsg) if >8 pins are in use
-- Multiple btn.on calls on the SAME pin are ADDITIVE — each call OR-adds one event
-- type to the pin's subscription mask without removing previously subscribed types:
--   btn.on("PA_22", "click")
--   btn.on("PA_22", "double")     ← click is still subscribed
--   btn.on("PA_22", "long_press") ← click + double still subscribed
-- The pin slot is allocated once; subsequent calls reuse it.

btn.off(pin)                 -- release one pin
btn.off()                    -- release all pins

-- Active polling (games / UI main loops)
local ev = btn.get_event()           -- non-blocking: one event table, or nil
local ev = btn.get_event(timeout_ms) -- blocking: wait up to timeout_ms; nil on timeout
for ev in btn.events() do ... end     -- drain all currently-queued events (non-blocking)

local pressed = btn.get_level(pin)   -- instantaneous logical state: 1 = pressed
                                     -- (polarity-aware; not queued)

-- Callback dispatch (declarative automation)
btn.dispatch()  -> count             -- drain queue, invoke callbacks for this script's pins

-- Context switch
btn.flush()                          -- drop queued events + reset every FSM to idle
```

### Event table

`get_event` / the `events()` iterator / callbacks all yield one table:

```lua
{ pin = "PA_22", type = "click", hold_ms = <integer> }
```

`ev.pin` is the canonical pin name string (e.g. `"PA_22"`) — the same format
accepted by `btn.on()` and stored in `board.json`. Compare directly:
`if ev.pin == "PA_22" then ...`

`hold_ms` = how long the press has lasted so far; meaningful for `up` /
`long_press` / `hold`, otherwise `0`.

## Patterns

**Game loop — fixed frame rate, non-blocking + level query**
```lua
local btn = require("button")
btn.on(KEY1, "click")                 -- subscribe only (no callback)
while true do
  for ev in btn.events() do           -- drain this frame's input
    if ev.type == "click" then turn(ev.pin) end
  end
  if btn.get_level(KEY2) == 1 then speed_up() end   -- continuous: held = faster
  step_game(); render(); sys.sleep_ms(TICK_MS)
end
```

**Idle UI / clock — blocking, near-zero CPU**
```lua
local btn = require("button")
btn.on(KEY1, "click"); btn.on(KEY1, "long_press")
while true do
  local ev = btn.get_event(1000)      -- wakes early on a key; long_press also wakes
  if ev then handle(ev) else tick_clock() end
end
```

**Declarative callbacks + press-and-repeat**
```lua
local btn = require("button")
btn.on(KEY1, "double", reset)
btn.on(KEY1, "hold", volume_up)       -- repeats every ~200 ms while held
while true do btn.dispatch(); sys.sleep_ms(20) end
```

## Notes & limits

- **Presses shorter than the ~8.2 ms hardware debounce window are invisible** — fine for
  human input (fastest taps are ≥30 ms), but don't use `button` for very short
  electrical pulses; use raw `gpio.on` / `gpio.get_irq_count` for those.
- **One polarity per firmware** (active-low by default). Active-high boards need
  a firmware rebuild.
- **`dispatch` timing precision = your poll interval.** For accurate time events
  (`long_press`/`double`/`hold`), prefer the blocking `get_event(timeout)`, which
  wakes exactly at the next deadline.
- **Single consumer.** `button` is designed for one UI/game loop. `dispatch`
  fires callbacks only for pins the calling script owns; `get_event` returns
  whatever is queued (multiple scripts sharing one button is the caller's
  responsibility). Concurrent configuration from different scripts is safe.
- Up to **8** button pins (`button.on` of a 9th returns `nil, errmsg`).
- Raw level is still `gpio.get_level(pin)`; `btn.get_level(pin)` is the
  polarity-aware *logical* state.

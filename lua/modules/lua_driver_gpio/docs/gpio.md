# gpio  —  require("gpio")

Digital IO + pin interrupts for the RTL8721F. `require("gpio")` returns a flat
function table — there is **no handle/object** to create or close. Pin names:
`PA_0..PA_31`, `PB_0..PB_31`, `PC_0..PC_8` (e.g. `"PA_30"`); raw integer PinName
values are also accepted. All functions raise via `error()` on bad arguments;
wrap in `pcall` if recovery is needed.

## API

```lua
-- Basic I/O
gpio.set_direction(pin, mode)   -- mode: "input" | "output"
gpio.set_level(pin, value)      -- value: 0 or 1
local v = gpio.get_level(pin)   -- → 0 | 1  (hardware readback)
gpio.set_pull(pin, pull)        -- pull: "none" | "up" | "down"

-- Callback-based interrupt  ★ preferred for event-driven scripts
gpio.on(pin, edge, fn)          -- edge: "rising"|"falling"|"both"
                                -- registers fn as callback; auto-configures IRQ
                                -- with pull-up for "both" (active-low buttons)
                                -- call gpio.irq_enable(pin) afterwards
gpio.off(pin)                   -- unregister callback + disable IRQ for pin
gpio.dispatch()  -> count       -- non-blocking: call all pending callbacks, return count

-- Low-level polled counter (legacy / high-frequency counting)
gpio.set_irq(pin, trigger [, debounce_en])
    -- trigger:     "rising" | "falling" | "both" | "level_high" | "level_low"
    -- debounce_en: 1 (default) or 0
    -- Configures IRQ hardware; does NOT enable. Required before irq_enable
    -- when NOT using gpio.on (gpio.on auto-configures).
gpio.irq_enable(pin)
gpio.irq_disable(pin)
local n = gpio.get_irq_count(pin)
gpio.clear_irq_count(pin)
```

### Parameter summary

| Function          | arg1  | arg2                                | arg3                  |
|-------------------|-------|-------------------------------------|-----------------------|
| `set_direction`   | pin   | `"input"`\|`"output"`               | —                     |
| `set_level`       | pin   | `0`\|`1`                            | —                     |
| `get_level`       | pin   | —                                   | —                     |
| `set_pull`        | pin   | `"none"`\|`"up"`\|`"down"`          | —                     |
| `on`              | pin   | `"rising"`\|`"falling"`\|`"both"`   | callback fn           |
| `off`             | pin   | —                                   | —                     |
| `dispatch`        | —     | —                                   | —                     |
| `set_irq`         | pin   | trigger string                      | debounce_en (def `1`) |
| `irq_enable`      | pin   | —                                   | —                     |
| `irq_disable`     | pin   | —                                   | —                     |
| `get_irq_count`   | pin   | —                                   | —                     |
| `clear_irq_count` | pin   | —                                   | —                     |

### Callback event table

`fn` receives one table: `{type="gpio", pin="PA_22", edge="rise"|"fall"}`
(`pin` is the canonical string name, same format as `gpio.on()` input)

### Level interrupt note

For `"level_high"`/`"level_low"`, the hardware fires continuously while the
level is held. The ISR auto-flips polarity and re-arms after each fire (with a
64 µs settle delay) so it self-rate-limits instead of storming; each transition
still increments the counter.

## Examples

Event-driven button loop (preferred pattern):
```lua
function run(args)
    gpio.on("PA_22", "both", function(ev)
        print("btn", ev.edge, sys.millis())
    end)
    gpio.irq_enable("PA_22")
    while true do
        event.wait(30000)
    end
end
```

Multiple buttons with `event.wait`:
```lua
function run(args)
    local function cb(ev) print(ev.pin, ev.edge) end
    gpio.on("PA_22", "both", cb)
    gpio.on("PA_23", "both", cb)
    gpio.irq_enable("PA_22"); gpio.irq_enable("PA_23")
    for _ = 1, 10 do event.wait(5000) end
end
```

Drive and verify an output:
```lua
function run(args)
    gpio.set_direction(args.pin, "output")
    gpio.set_level(args.pin, 1)
    return '{"level":' .. gpio.get_level(args.pin) .. '}'
end
```

## gpio.on / event.wait pattern

`gpio.on` is the **preferred** interrupt API for event-driven scripts.
It uses the unified hardware event queue: ISR writes a pending flag and gives a
counting semaphore; `gpio.dispatch()` (non-blocking) or `event.wait(timeout_ms)`
(blocking, cancel-safe) drains the queue and calls registered callbacks in the
script's own `lua_State`.

`event.wait(timeout_ms)`:
- Returns `true` when an event was dispatched; `nil` on timeout.
- Checks the cooperative cancel hook every 50 ms → safe for long waits.
- Available in **both skill scripts and the REPL**.

**Rule of thumb:** use `gpio.on` + `event.wait` for buttons / edge sensors.
Use the polled counter (`get_irq_count`) only for high-frequency pulse counting
where every edge matters and callbacks would be too slow.

### Mechanical buttons → use the `button` module, not raw `gpio.on`

`gpio.on` delivers **raw hardware edges**. A single physical button press fires
2–5 edges in a few milliseconds (mechanical contact bounce), so raw `gpio.on` is
the wrong tool for buttons — you would have to debounce and discriminate
click/double/long-press yourself in Lua.

**For any push-button, use `require("button")`** (see `rolfs:/docs/button.md`).
It debounces in hardware (~8.2 ms GPIO filter), emits clean `click`/`double`/`long_press`/`hold` events, offers
a blocking `get_event(timeout)` and an instantaneous `get_level(pin)`, and is
safe across concurrent scripts. Reserve raw `gpio.on` for non-button edge sources
(encoders, external triggers) where you genuinely want every edge.

**Single-script ownership:** at most one running script can hold callbacks at a
time. The second script calling `gpio.on` takes ownership; the first script's
callbacks are silently dropped (their `gpio.dispatch()` returns 0). This is
sufficient for typical single-job embedded use.

## Concurrency & resources

One process-wide mutex protects all hardware operations. The ISR is lock-free
(single-byte writes + semaphore give). No per-pin handle; GPIO clock stays on for
the lifetime of the boot. Plain I/O needs no release; interrupt pins release via
`gpio.off(pin)` or `gpio.irq_disable(pin)`.

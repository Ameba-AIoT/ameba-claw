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
local v = gpio.get_level(pin)   -- → 0 | 1  (hardware readback; verify outputs)
gpio.set_pull(pin, pull)        -- pull: "none" | "up" | "down"

-- Interrupt (polled counter — there is NO Lua callback)
gpio.set_irq(pin, trigger [, debounce_en])
    -- trigger:     "rising" | "falling" | "both" | "level_high" | "level_low"
    -- debounce_en: 1 (default) or 0  — 0 disables the ~64 µs hardware debounce
    -- Configures interrupt mode and resets this pin's counter; does NOT enable.
gpio.irq_enable(pin)            -- enable the interrupt (call after set_irq)
gpio.irq_disable(pin)           -- disable the interrupt
local n = gpio.get_irq_count(pin)  -- → fires counted by the ISR since last reset
gpio.clear_irq_count(pin)          -- reset that pin's counter to 0
```

### Parameter order / type (verified against `src/lua_driver_gpio.c`)

| Function          | arg1  | arg2                              | arg3                 |
|-------------------|-------|-----------------------------------|----------------------|
| `set_direction`   | pin   | `"input"`\|`"output"`             | —                    |
| `set_level`       | pin   | `0`\|`1`                          | —                    |
| `get_level`       | pin   | —                                 | —                    |
| `set_pull`        | pin   | `"none"`\|`"up"`\|`"down"`        | —                    |
| `set_irq`         | pin   | trigger string                    | debounce_en (def `1`)|
| `irq_enable`      | pin   | —                                 | —                    |
| `irq_disable`     | pin   | —                                 | —                    |
| `get_irq_count`   | pin   | —                                 | —                    |
| `clear_irq_count` | pin   | —                                 | —                    |

### Level interrupt note

For `"level_high"`/`"level_low"`, the hardware fires continuously while the
level is held. The ISR auto-flips polarity and re-arms after each fire (with a
64 µs settle delay) so it self-rate-limits instead of storming; each transition
still increments the counter.

## Examples

Drive and verify an output:
```lua
local gpio = require("gpio"); local resp = require("lib/resp")
function run(args)
    gpio.set_direction(args.pin, "output")
    gpio.set_level(args.pin, 1)
    local v = gpio.get_level(args.pin)   -- always read back before reporting success
    return resp.ok({pin=args.pin, level=v, verified=(v==1)})
end
```

Count rising edges on a pin:
```lua
gpio.set_irq("PA_31", "rising", 1)   -- arg order: pin, trigger, debounce_en
gpio.clear_irq_count("PA_31")
gpio.irq_enable("PA_31")
-- ... stimulus happens ...
gpio.irq_disable("PA_31")
print("edges:", gpio.get_irq_count("PA_31"))
```

## Concurrency & resources

GPIO is a shared peripheral and `gpio` is loaded into several Lua states (REPL,
timer sandbox, skill sandbox), so concurrent `lua_run` jobs and timer callbacks
may reach it at once. The driver holds **one process-wide mutex** for every
hardware operation, so two jobs can never interleave register writes (the shared
GPIO clock enable and per-port IRQ registration are also done under the lock and
are idempotent). The ISR runs lock-free: it only bumps a per-pin counter and, for
level triggers, re-arms its own pin.

**Resource model (init → operation → deinit):** there is no per-pin handle and
no hardware deinit — the GPIO clock stays on for the lifetime of the boot, and a
pin's first use lazily configures it (idempotent thereafter). "Release" applies
only to interrupts:

- `gpio.irq_disable(pin)` stops the pin firing (the acquire is `set_irq` +
  `irq_enable`).
- `gpio.clear_irq_count(pin)` resets the counter; `set_irq` also clears it.

The acquire/release cycle (`set_irq` → `irq_enable` → poll `get_irq_count` →
`irq_disable` → `clear_irq_count`) is fully repeatable. Plain I/O has nothing to
release; just re-`set_direction`/`set_pull` as needed.

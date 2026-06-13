# lua_driver_gpio

Lua GPIO driver for Ameba RTOS (RTL8721F).

Uses fwlib raw API (`GPIO_*`, `PAD_PullCtrl`, interrupt registration) directly —
no HAL layer dependency. `require("gpio")` returns a flat function table (no
handle object).

## API

```lua
local gpio = require("gpio")

-- Basic I/O
gpio.set_direction(pin, mode)   -- mode: "input" | "output"
gpio.set_level(pin, value)      -- value: 0 or 1
local v = gpio.get_level(pin)   -- returns 0 or 1 (hardware readback)

-- Pull resistor (Ameba extension)
gpio.set_pull(pin, pull)        -- pull: "none" | "up" | "down"

-- Interrupt (polled counter — no Lua callback is invoked)
gpio.set_irq(pin, trigger [, debounce_en])
    -- trigger:     "rising" | "falling" | "both" | "level_high" | "level_low"
    -- debounce_en: 1 (default) or 0  (~64 µs hardware debounce; pass 0 to disable)
    -- Configures interrupt mode AND resets this pin's counter; does NOT enable yet.
gpio.irq_enable(pin)               -- enable interrupt (call after set_irq)
gpio.irq_disable(pin)              -- disable interrupt
local n = gpio.get_irq_count(pin)  -- interrupt count since last reset
gpio.clear_irq_count(pin)          -- reset count to 0
```

`pin` accepts string names (`"PA_30"`, `"PB_5"`) or raw integer PinName values.

### Parameter order

Every function takes `pin` as the **first** argument. `set_irq` is
`(pin, trigger [, debounce_en])` — `debounce_en` defaults to **1** (enabled).
All orders/defaults above are verified line-by-line against
`src/lua_driver_gpio.c`.

### Level interrupt note

For `"level_high"` / `"level_low"` the hardware fires continuously while the
level is held. The ISR auto-flips polarity and re-arms after each fire (with a
64 µs settle delay) so it self-rate-limits instead of storming; each transition
still bumps the counter. With a single high→low pulse you may therefore see more
than one count.

## Test

The suite lives in `test/test_gpio.lua` and is also embedded as a C string in
`test/lua_gpio_test_provision.c` (the two MUST stay in sync). On boot the script
is written to VFS (`vfs:test_gpio.lua`) but is **not** auto-run. Trigger it via
AT command on the serial console:

```
AT+CLAW=gpio
```

### Wiring

**Single board, loopback required: `PA_30` ↔ `PA_31`** (PA30 = output/driver,
PA31 = input/interrupt). No external components needed.

### Cases (covers all 9 APIs)

| #  | Check               | What it exercises                                                       |
|----|---------------------|-------------------------------------------------------------------------|
| 0a | get_level on output | `set_direction("output")`, `set_level`, `get_level` read back 0/1       |
| 0b | input loopback      | `set_direction("input")`, PA31 reads what PA30 drives                    |
| 0c | pull up/down/none   | `set_pull` — both ends high-Z, the pull owns the wire (up→1, down→0)     |
| 1  | rising edge ×10     | `set_irq("rising")` + `irq_enable`, `get_irq_count() >= 5`               |
| 2  | falling edge ×10    | `set_irq("falling")`, `get_irq_count() >= 5`                            |
| 3  | both edges ×10      | `set_irq("both")`, `get_irq_count() >= 10` (≈20 transitions)            |
| 4  | level_high ×10      | `set_irq("level_high")`, auto re-arm, `get_irq_count() >= 1`             |
| 5  | level_low ×10       | `set_irq("level_low")`, `get_irq_count() >= 1`                          |
| 6  | resource recycle    | re-`set_irq`/`irq_enable` → re-fire ≥3 → `irq_disable` → `clear` → ==0   |

Each check prints `[gpio] <name>: ok` or `FAIL ...`. The final line is
`success` only when all checks pass, otherwise `FAIL: N test(s) failed`.

All 9 APIs are covered: `set_direction`, `set_level`, `get_level`, `set_pull`,
`set_irq`, `irq_enable`, `irq_disable`, `get_irq_count`, `clear_irq_count`.

## Concurrency & resources

GPIO is a shared peripheral and `gpio` is loaded into several Lua states (REPL,
timer sandbox, skill sandbox), so concurrent `lua_run` jobs and timer callbacks
may reach it at once. The driver holds **one process-wide mutex**
(`s_gpio_lock`, created in `lua_driver_gpio_init()` during the single-threaded
boot phase) for every hardware operation, so two jobs can never interleave
register writes; the shared GPIO clock enable and per-port IRQ registration are
also done under the lock and are idempotent. Argument/range validation runs
**before** the lock is taken (those paths `longjmp` and would otherwise leak the
mutex). The ISR (`gpio_irq_cb`) runs lock-free — it only bumps a per-pin counter
(a single-word write) and, for level triggers, re-arms its own pin.

**Resource model (init → operation → deinit):** there is no per-pin handle and
no hardware deinit — the GPIO clock stays on for the lifetime of the boot, and a
pin's first use lazily configures it (idempotent thereafter). "Release" applies
only to interrupts:

- `irq_disable(pin)` stops the pin firing (acquire = `set_irq` + `irq_enable`).
- `clear_irq_count(pin)` resets the counter; `set_irq` also clears it.

The acquire → operate → release cycle is fully repeatable, as **check 6**
demonstrates (re-arm, re-fire, disable, then `clear_irq_count` brings the counter
back to 0). Plain I/O has nothing to release.

## Notes

- `set_pull` is an Ameba extension for explicit pull resistor control.
- The interrupt model is **polled counter**, not a Lua callback: enable, let
  stimulus happen, then read `get_irq_count`. This keeps ISR work minimal and
  avoids re-entering the Lua VM from interrupt context.

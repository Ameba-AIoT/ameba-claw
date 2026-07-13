# pwm — require("pwm")

PWM output driver for Ameba RTOS (RTL8721F).
`require("pwm")` returns a flat table with one constructor: `pwm.new(config)`.
Each call to `new()` returns an independent **handle** (userdata) that owns one
timer channel. Multiple handles on the same timer are allowed when they use the
same `frequency_hz`.

---

## Constructor

```
pwm.new(config) -> handle
```

`config` fields:

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `pin` | string | yes | — | GPIO pin name, e.g. `"PA_26"` |
| `timer_idx` | integer | yes | — | PWM timer: `4`, `5`, `6`, or `7` |
| `channel` | integer | yes | — | Timer channel: `0`, `1`, `2`, or `3` |
| `frequency_hz` | integer | no | `1000` | PWM frequency in Hz (1–40 000 000) |
| `duty_percent` | number | no | `50` | Initial duty cycle, 0.0–100.0 |

Raises an error if `pin`, `timer_idx`, or `channel` is missing/invalid, if
`duty_percent` is out of range, or if the same timer is already open at a
different frequency.

---

## Handle methods

```
handle:set_enabled(bool)           -- true = run, false = stop timer output
handle:set_duty(percent)           -- set duty cycle 0.0–100.0
handle:set_duty(channel, percent)  -- channel arg accepted but ignored (always 1 ch per handle)
handle:set_frequency(hz)           -- change PWM frequency; re-applies duty ratio
handle:get_channel_count() -> 1    -- always 1 (one channel per handle)
handle:close()                     -- release hardware resources
```

---

## Examples

### Basic 1 kHz output

```lua
local pwm = require("pwm")
local sys  = require("sys")

local h = pwm.new({ pin="PA_26", timer_idx=4, channel=3, frequency_hz=1000 })
h:set_enabled(true)
sys.sleep_ms(500)
h:set_duty(25)
sys.sleep_ms(500)
h:set_frequency(2000)
sys.sleep_ms(500)
h:set_enabled(false)
h:close()
```

### SG90 servo at 50 Hz

When using `lua_run`, wrap the body in a global `run(args)` function:

```lua
local pwm = require("pwm")
local sys  = require("sys")

local function angle_to_duty(a)  -- SG90: 500-2500 us over 20 ms period
    return (500 + 2000 * a / 180) / 200
end

function run(args)  -- REQUIRED by lua_run: global, not local, not self-executing
    local h = pwm.new({ pin="PA_26", timer_idx=4, channel=3, frequency_hz=50,
                        duty_percent=angle_to_duty(0) })
    h:set_enabled(true)
    for a = 0, 180, 10 do
        h:set_duty(angle_to_duty(a))
        sys.sleep_ms(80)
    end
    h:set_enabled(false)
    h:close()
end
```

---

## RTL8721F pin reference (TIM4)

| Channel | Pin |
|---------|-----|
| 0 | PA_15 |
| 1 | PA_16 |
| 2 | PA_25 |
| 3 | PA_26 |

---

## Concurrency & resources

### Resource lifecycle

Each `pwm.new()` call acquires a timer channel. The first handle on a given
timer initialises the timer hardware and enables the peripheral clock. The last
`close()` on that timer runs `RTIM_DeInit` and disables the clock. Between
those two events, other handles on the same timer are unaffected by any single
handle's `close()`.

Resources held by a handle: one timer channel (CCRx register), one pinmux
assignment, one reference count slot in the timer. All are released by
`close()` or by garbage collection (`__gc`). **Always call `close()` explicitly**
— relying on GC is non-deterministic.

### Concurrency contract (for Lua callers)

1. **Single-API calls are safe to call concurrently from multiple Lua tasks.**
   `set_duty` writes one per-channel CCRx register (no shared timer state, no
   lock needed). All other methods (`set_enabled`, `set_frequency`, `close`)
   hold a per-timer mutex for the duration of the call; concurrent callers on
   the *same* timer will queue safely with a 100 ms timeout (`close` uses
   1000 ms). Two tasks operating on *different* timers never contend.

2. **Multi-step sequences are not atomic.** A sequence such as
   `set_frequency(50)` followed by `set_duty(7.5)` is two separate calls.
   Another task can interleave between them. If atomic reconfiguration matters,
   serialize calls from one task.

3. **Do not share a handle between tasks.** A handle must be created and used
   from a single Lua task. Passing a handle to another task and calling methods
   concurrently is unsupported and may produce incorrect duty values or corrupt
   the `ud->duty`/`ud->period` cache.

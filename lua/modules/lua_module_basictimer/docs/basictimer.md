# basictimer  —  require("basictimer")

Hardware timer driver for RTL8721F basic timers **TIM0–TIM3** (LTIM0–LTIM3).
`basictimer.new()` returns a **handle** (userdata); every operation goes through
the handle. TIM4–TIM8 (PWM timers) are not covered by this module.

## API

```lua
-- Acquire
local bt = basictimer.new(tim_idx, period_us [, clk_src [, xtal_div]])
    -- tim_idx  : 0–3
    -- period_us: reload period in µs (≥ 1000 for sdm32k, ≥ 5 for xtal)
    -- clk_src  : "sdm32k" (default, 32 768 Hz) | "xtal" (40 MHz / xtal_div)
    -- xtal_div : 2–64, default 40 → 1 MHz / 1 µs per tick  (xtal only)
    -- Raises on bad args or if the timer index is already in use.

-- Lifecycle
bt:start()                       -- start the counter
bt:stop()                        -- pause the counter (does not release the slot)
bt:close()                       -- stop, deinit, release hardware slot  ← always call this

-- Counter & period
local v = bt:get_count()         -- → integer: current raw ARR counter register value
bt:set_period(period_us)         -- change reload period while running or stopped

-- Interrupt counter (C-level ISR only; no Lua callback)
local n = bt:get_irq_count()     -- → integer: overflow IRQ count since new() or last clear
bt:clear_irq_count()             -- reset overflow counter to 0
```

### Parameter summary

| Function          | arg1             | arg2                            | arg3                          | arg4                    |
|-------------------|------------------|---------------------------------|-------------------------------|-------------------------|
| `new`             | `tim_idx` 0–3   | `period_us` int                 | `clk_src` str (def `"sdm32k"`) | `xtal_div` 2–64 (def 40)|
| `set_period`      | `period_us` int  | —                               | —                             | —                       |
| `get_count`       | —                | —                               | —                             | —                       |
| `get_irq_count`   | —                | —                               | —                             | —                       |
| `clear_irq_count` | —                | —                               | —                             | —                       |

## Concurrency & resources

One process-wide mutex protects all Lua-level hardware operations. The ISR
(`basictimer_irq_handler`) runs lock-free — it only increments a `volatile u32`
counter (Cortex-M 32-bit aligned write is atomic).

**Resource model — always call `close()`:**
`new()` acquires a hardware slot: marks `s_in_use[idx]`, enables the peripheral
clock, and registers the ISR. `close()` is the mandatory release: it calls
`RTIM_DeInit`, disables the peripheral clock, restores the clock source to
SDM32K, and clears `s_in_use[idx]`. The userdata carries a `__gc` metamethod
that performs the same cleanup, but embedded Lua may never trigger GC when
memory pressure is low — do **not** rely on GC for cleanup. After `close()`,
`tim_idx` is immediately available for a new `new()` call.

## Examples

SDM32K, 100 ms period:
```lua
local bt  = require("basictimer")
local sys = require("sys")

local h = bt.new(0, 100000)          -- TIM0, 100 ms, sdm32k
h:start()
sys.sleep_ms(1000)
print("irq count:", h:get_irq_count())  -- ~10
h:stop()
h:close()
```

XTAL at 1 MHz (div=40), 50 ms period:
```lua
local h = bt.new(1, 50000, "xtal", 40)
h:start()
sys.sleep_ms(500)
print("irq count:", h:get_irq_count())  -- ~10
h:stop()
h:close()
```

Change period at runtime:
```lua
h:set_period(200000)   -- switch to 200 ms while running or stopped
```

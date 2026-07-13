# lua_module_basictimer

Lua timer driver for RTL8721F basic hardware timers TIM0–TIM3 (LTIM0–LTIM3).

Uses fwlib raw API (`RTIM_*`, `RCC_PeriphClockCmd`, `RCC_PeriphClockSourceSet`,
`RCC_PeriphClockDividerSet`) directly — no HAL layer dependency. `basictimer.new()`
returns a **handle** (userdata); there is no flat function table.

## Clock sources

| Source | Frequency | Resolution | Min period |
|---|---|---|---|
| `"sdm32k"` (default) | 32 768 Hz | ~30.5 µs/tick | 1 000 µs |
| `"xtal"` | 40 MHz / xtal_div | div=40 → 1 µs/tick | 5 µs |

ARR (auto-reload register) is computed directly from `period_us` rather than
relying on `RTIM_ChangePeriodImmediate_us`, which assumes a fixed 1 MHz XTAL
clock. This makes arbitrary `xtal_div` values work correctly.

Not applicable to PWM timers (TIM4–TIM8).

## API

### `basictimer.new`

```lua
bt = basictimer.new(tim_idx, period_us [, clk_src [, xtal_div]])
```

| Parameter | Type | Range | Default | Description |
|---|---|---|---|---|
| `tim_idx` | integer | 0–3 | — | Hardware timer index (TIM0–TIM3). |
| `period_us` | integer | ≥ 1000 (sdm32k) / ≥ 5 (xtal) | — | Reload period in microseconds. |
| `clk_src` | string | `"sdm32k"` / `"xtal"` | `"sdm32k"` | Clock source. |
| `xtal_div` | integer | 2–64 | 40 | XTAL divider (xtal only). 40 → 1 MHz → 1 µs/tick. |

Returns a handle on success. Raises if `tim_idx` is out of range, `clk_src` is
unknown, `xtal_div` is out of range, `period_us` is below the minimum for the
chosen clock, or the timer index is already in use.

### Lifecycle

```lua
bt:start()    -- start the counter
bt:stop()     -- pause without releasing the hardware slot
bt:close()    -- stop, deinit, release hardware slot  ← always call this explicitly
```

### Counter & period

```lua
local v = bt:get_count()        -- current raw ARR counter register value
bt:set_period(period_us)        -- change reload period while running or stopped
```

### Interrupt counter

The hardware ISR (`basictimer_irq_handler`) fires on every counter overflow and
increments a `volatile u32` counter in C. There is no Lua-level callback.

```lua
local n = bt:get_irq_count()   -- overflow count since new() or last clear_irq_count()
bt:clear_irq_count()           -- reset counter to 0
```

## Period accuracy

**SDM32K:** Actual period = `floor(period_us / 1_000_000 × 32768) / 32768 × 1_000_000` µs.
At 100 ms requested: ARR = 3275, actual ≈ 99.98 ms.

**XTAL:** ARR = `period_us × 40 / xtal_div − 1`.
At div=40 (1 MHz): ARR = period_us − 1, exact µs resolution.

## Test

```
AT+CLAW=basic,timer
```

Expected output ends with `success`. The test uses no external wiring and takes
approximately 5 seconds.

### Test cases (TC1–TC12)

| TC | API(s) exercised | What is checked |
|---|---|---|
| TC1–TC4 | `new`, `start`, `get_irq_count`, `stop`, `close` | TIM0–TIM3 × SDM32K, 100 ms period, ≥ 3 IRQs in 500 ms |
| TC5–TC8 | `new`, `start`, `get_irq_count`, `stop`, `close` | TIM0–TIM3 × XTAL div=40, 50 ms period, ≥ 6 IRQs in 500 ms |
| TC9 | `get_count` | returns non-negative integer while timer running |
| TC10 | `close` + `new` | hardware slot released and immediately reusable |
| TC11 | `clear_irq_count` | counter ≤ 1 immediately after clear (≥ 3 before) |
| TC12 | `set_period` | longer period produces fewer IRQs over fixed window |

All 8 APIs are covered: `new`, `start`, `stop`, `close`, `get_count`,
`get_irq_count`, `clear_irq_count`, `set_period`.

## Concurrency & resources

One process-wide mutex (`s_lock`) protects all Lua-level hardware operations.
Argument validation runs **before** the lock is acquired so that a `luaL_error`
/ `longjmp` path cannot leak the mutex. The ISR (`basictimer_irq_handler`) runs
lock-free — it only increments a `volatile u32` counter (Cortex-M 32-bit
aligned write is atomic).

**Resource model (new → start/stop → close):**

`new()` is the acquire step: it marks `s_in_use[idx]`, sets the clock source
and divider, enables the peripheral clock via `RCC_PeriphClockCmd`, initialises
the hardware with `RTIM_TimeBaseInit`, enables the update interrupt, and
allocates a Lua userdata. No other thread can `new()` the same `tim_idx` until
`close()` is called.

`close()` is the mandatory release step: `RTIM_DeInit` stops the counter and
clears the IRQ, `RCC_PeriphClockCmd(..., DISABLE)` gates the peripheral clock,
the clock source is restored to SDM32K so the next `new()` starts from a known
state, and `s_in_use[idx]` is cleared. After `close()`, `tim_idx` is
immediately available for a new `new()` call (TC10 demonstrates this).

The userdata carries a `__gc` metamethod that performs the same cleanup when the
userdata is garbage-collected. However, embedded Lua may never trigger GC when
memory pressure is low — **always call `bt:close()` explicitly**; do not rely
on the finaliser. Calling `close()` (or `__gc`) on an already-closed handle is
a safe no-op.

The acquire/release cycle is fully repeatable: `new()` → `start()` → operate →
`stop()` → `close()` → `new()` works any number of times.

## Notes

- `start()` and `stop()` do not release the hardware slot — only `close()` does.
- `set_period()` uses `RTIM_ChangePeriodImmediate` (raw ARR) rather than
  `RTIM_ChangePeriodImmediate_us`, so it works correctly for any `xtal_div`.
- `get_count()` reads the live CNT register; the value decrements toward 0 and
  wraps at ARR (down-counting timer).
- `clear_irq_count()` is a plain C store (not under the lock) — the counter is
  a `volatile u32` and a 32-bit write on Cortex-M is atomic.

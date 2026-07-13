# rtc  —  require("rtc")

Lua C module for the RTL8721F on-chip RTC. `require("rtc")` returns a flat
function table — there is **no handle/object** to create or close. All functions
raise via `error()` on bad arguments; wrap in `pcall` if recovery is needed.

Single board, **no external hardware/wiring** required (uses the internal
131 kHz oscillator).

## Constraints

- Call `rtc.init()` once before any other call. A second `init()` is a safe
  no-op (idempotent) and does **not** clear the time.
- `set_time` / `set_alarm` / `set_wakeup` argument **order and ranges** must be
  exact (see below) — out-of-range values raise an error.
- Flags (`alarm_fired` / `wakeup_fired`) are polled, not interrupt callbacks.

## API

```lua
rtc.init()                                        -- enable clock, 24h format (call once)

rtc.set_time(year, mon, mday, hour, min, sec)     -- set calendar+wall clock
t = rtc.get_time()                                -- → {year,mon,mday,hour,min,sec,yday}  (UTC)
t = rtc.get_local_time(offset_hours)              -- → same table, UTC offset applied (e.g. 8 for UTC+8)

rtc.set_alarm(hour, min, sec)                      -- configure + enable daily H:M:S alarm
rtc.disable_alarm()                                -- disable alarm AND clear its flag
fired = rtc.alarm_fired()                          -- → bool (poll the alarm flag)
rtc.clear_alarm()                                  -- clear alarm flag, keep alarm enabled

rtc.set_wakeup(seconds)                            -- configure + enable periodic wakeup
rtc.disable_wakeup()                               -- disable wakeup AND clear its flag
fired = rtc.wakeup_fired()                         -- → bool (poll the wakeup flag)
rtc.clear_wakeup()                                 -- clear wakeup flag, keep wakeup enabled
```

### Parameter ranges

| Function    | Arg       | Range / Type           | Notes                                  |
|-------------|-----------|------------------------|----------------------------------------|
| `get_local_time` | `offset_hours` | -12–14 (int)  | UTC offset in whole hours; handles day/month/year rollover |
| `set_time`  | `year`    | 1900–2155 (int)        | full calendar year                     |
|             | `mon`     | 1–12 (int)             | 1 = January                            |
|             | `mday`    | 1–31 (int)             | day of month                           |
|             | `hour`    | 0–23 (int)             | 24-hour format                         |
|             | `min`     | 0–59 (int)             |                                        |
|             | `sec`     | 0–59 (int)             |                                        |
| `set_alarm` | `hour`    | 0–23 (int)             | alarm matches H:M:S **every day**      |
|             | `min`     | 0–59 (int)             |                                        |
|             | `sec`     | 0–59 (int)             |                                        |
| `set_wakeup`| `seconds` | 1–131072 (int)         | period of the 1 Hz periodic wakeup     |

`get_time()` returns a table with keys `year, mon, mday, hour, min, sec` plus
`yday` (0-based day-of-year, 0 = Jan 1).

## Examples

Set the clock and read it back:
```lua
rtc.init()
rtc.set_time(2024, 6, 11, 8, 30, 0)   -- year, mon, mday, hour, min, sec  (this order!)
local t = rtc.get_time()
print(string.format("%04d-%02d-%02d %02d:%02d:%02d",
      t.year, t.mon, t.mday, t.hour, t.min, t.sec))
```

Get local time (UTC+8) — preferred over `get_time()` for display:
```lua
rtc.init()
local t = rtc.get_local_time(8)   -- UTC+8; handles midnight/month-end rollover
print(string.format("%04d-%02d-%02d %02d:%02d:%02d",
      t.year, t.mon, t.mday, t.hour, t.min, t.sec))
```

Daily alarm, polled (alarm args are H, M, S — there is no date field):
```lua
rtc.init()
rtc.set_time(2024, 1, 15, 10, 0, 0)
rtc.set_alarm(10, 0, 5)               -- fires at 10:00:05 every day
-- ... later, poll:
if rtc.alarm_fired() then
    rtc.clear_alarm()                  -- or rtc.disable_alarm() to stop it
end
```

Periodic wakeup timer:
```lua
rtc.init()
rtc.set_wakeup(3)                      -- flag is raised every 3 s
if rtc.wakeup_fired() then
    rtc.clear_wakeup()
end
rtc.disable_wakeup()                   -- stop it when done
```

## Concurrency & resources

The RTC is a **single shared peripheral**. `rtc` is loaded into several Lua
states (REPL, timer sandbox, skill sandbox), so multiple `lua_run` jobs and
timer callbacks may call it concurrently. The driver holds **one process-wide
mutex** (100 ms finite timeout) for the duration of every hardware operation,
so two jobs can never interleave register writes. If the mutex cannot be
acquired within 100 ms, the call raises `"rtc: busy"` — wrap in `pcall` if
you need to handle contention gracefully. `init()` runs `RTC_Init()` only once
(later calls are idempotent), so a late `init()` cannot reset prescalers
underneath an in-flight `set_time` in another job.

**Resource model (init → operation → deinit):** there is no per-call hardware
handle and no hardware deinit — the RTC clock stays on for the lifetime of the
boot. "Release" means stopping the optional timers you started:

- `disable_alarm()` disables the alarm and clears its flag.
- `disable_wakeup()` disables the wakeup and clears its flag.

A clean acquire/release cycle is `set_alarm()` → `alarm_fired()`/`clear_alarm()`
→ `disable_alarm()` (likewise for wakeup); it is fully repeatable. The time
registers are **not** cleared by any deinit — call `set_time()` to overwrite
them.

## Notes

- The RTC retains time across warm (non-POR) resets. `init()` re-configures
  prescalers but does not clear the time unless `set_time()` is also called.
- After `set_time()`, shadow-register sync takes up to ~61 µs; the test scripts
  use a conservative 200 ms settle delay.
- `alarm_fired()` / `wakeup_fired()` are level flags — after one reads `true`,
  call `clear_*` (or `disable_*`) before the next match to avoid re-triggering.
- **`rtc.get_time()` returns UTC:** The `sync_time` cap (SNTP) writes UTC to the RTC hardware. `rtc.get_time()` therefore returns raw UTC time. **Use `rtc.get_local_time(8)` instead** to get UTC+8 local time directly — it handles all day/month/year rollover correctly. Alternatively call `cap.call("get_current_time", {})` which returns a pre-formatted local datetime string.

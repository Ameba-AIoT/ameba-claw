# lua_driver_rtc

Lua RTC driver for Ameba RTOS (RTL8721F).

Uses fwlib raw API (`RTC_*`) directly — no HAL layer dependency.

## Hardware

The RTL8721F contains an on-chip RTC with the following features:

- 24-hour or AM/PM hour format (driver uses 24-hour)
- Calendar: year (1900–2155), day-of-year (0–511), hour, minute, second
- Alarm with H:M:S match (day field can be masked)
- Periodic wakeup timer (1 Hz base clock, 1–131072 second range)
- Write-protection on RTC registers (unlocked via key sequence 0xCA/0x53)

No external hardware required.

## API

### Initialization

```lua
rtc.init()
```

Enables the RTC clock, powers on the RTC peripheral, and configures 24-hour
format with default prescalers (PREDIV_A=127, PREDIV_S=255 → 1 Hz).

### Time

```lua
rtc.set_time(year, mon, mday, hour, min, sec)
```

| Parameter | Range       | Description                  |
|-----------|-------------|------------------------------|
| `year`    | 1900–2155   | Full calendar year           |
| `mon`     | 1–12        | Month (1 = January)          |
| `mday`    | 1–31        | Day of month                 |
| `hour`    | 0–23        | Hour (24-hour format)        |
| `min`     | 0–59        | Minute                       |
| `sec`     | 0–59        | Second                       |

```lua
local t = rtc.get_time()
-- t.year, t.mon, t.mday, t.hour, t.min, t.sec, t.yday
```

Returns a table. `yday` is 0-based day-of-year (0 = Jan 1).

### Alarm

```lua
rtc.set_alarm(hour, min, sec)   -- configure and enable alarm (H:M:S match)
rtc.disable_alarm()             -- disable alarm and clear flag
rtc.alarm_fired()               -- returns true if alarm flag is set (polling)
rtc.clear_alarm()               -- clear alarm flag without disabling
```

The alarm fires when the RTC H:M:S matches the configured values.
The day field is masked (don't-care), so the alarm fires every day at the
given time. After `alarm_fired()` returns `true`, call `clear_alarm()` or
`disable_alarm()` before the next second to avoid immediate re-trigger.

### Wakeup Timer

```lua
rtc.set_wakeup(seconds)         -- configure and enable periodic wakeup
rtc.disable_wakeup()            -- disable wakeup and clear flag
rtc.wakeup_fired()              -- returns true if wakeup flag is set (polling)
rtc.clear_wakeup()              -- clear wakeup flag without disabling
```

`seconds` must be 1–131072. The wakeup timer fires periodically every
`seconds` seconds using the 1 Hz synchronous clock.

## Test script

The suite lives in `test/rtc_test.lua` and is also embedded as a C string in
`test/lua_rtc_test_provision.c`. On boot it is written to VFS
(`vfs:rtc_test.lua`) but is **not** auto-run. Trigger it via AT command on the
serial console:

```
AT+CLAW=rtc,test
```

> **Do not** run it with `dofile("vfs:rtc_test.lua")` from the REPL — `dofile()`
> on a VFS path hits a NULL-lock assertion and crashes. The AT command runs the
> embedded copy in its own Lua state/task, which is the supported path.

The script runs on a single board with no external wiring and takes ~80 s
(it includes real-time waits). It performs 7 checks:

| # | Check            | What it exercises                                                            |
|---|------------------|------------------------------------------------------------------------------|
| 1 | Timing +10 s     | `init`, `set_time`, `get_time`; second counts up ≈10 (±2)                    |
| 2 | Timing +60 s     | minute carry `10:00 → 10:01`                                                  |
| 3 | Alarm            | `set_alarm` 5 s ahead → `alarm_fired()` → `clear_alarm` / `disable_alarm`     |
| 4 | Minute rollover  | `10:00:55 → 10:01:0x`                                                         |
| 5 | Hour rollover    | `10:59:55 → 11:00:0x`                                                         |
| 6 | Periodic wakeup  | `set_wakeup(3)` → `wakeup_fired()` → `clear_wakeup` / `disable_wakeup`        |
| 7 | Resource recycle | re-`init` (idempotent), date read-back, re-acquire then release alarm+wakeup  |

Each check prints `[RTC] <name> pass` or `[FAIL] ...`. The final line is
`[RTC] all pass` only when all 7 pass, otherwise `[RTC] fail`.

All 11 APIs are covered: `init`, `set_time`, `get_time`, `set_alarm`,
`disable_alarm`, `alarm_fired`, `clear_alarm`, `set_wakeup`, `disable_wakeup`,
`wakeup_fired`, `clear_wakeup`.

## Concurrency & resources

The RTC is a single shared peripheral and `rtc` is loaded into several Lua
states (REPL, timer sandbox, skill sandbox), so concurrent `lua_run` jobs and
timer callbacks may reach it at once. The driver holds **one process-wide mutex**
for every hardware operation, and `init()` runs `RTC_Init()` only once
(idempotent thereafter), so a late `init()` cannot reset registers under an
in-flight `set_time` in another job.

There is no per-call handle and no hardware deinit: the RTC clock stays on for
the lifetime of the boot, and `set_time` overwrites the calendar in place.
"Release" means stopping the optional timers — `disable_alarm()` and
`disable_wakeup()` each disable their timer and clear its flag. The
acquire/release cycle (`set_alarm`/`set_wakeup` → poll/clear → `disable_*`) is
fully repeatable, as check 7 demonstrates.

## Notes

- The RTC retains time across warm resets (non-POR). Calling `rtc.init()`
  re-configures prescalers but does not clear the time registers unless
  `rtc.set_time()` is also called.
- Shadow register synchronization after `set_time()` takes up to 2 RTCCLK
  cycles (~61 µs); the 200 ms settle delay in the test is conservative but reliable.
- No external hardware (XTAL, battery) is required for basic operation; the
  internal 131 kHz oscillator is used by default.

# sys  —  require("sys")

```lua
sys.sleep_ms(ms)          -- yield to the RTOS scheduler (10 ms min granularity)
sys.millis()  -> integer  -- monotonic millisecond counter since boot (wraps ~49 days)
sys.uptime()  -> number   -- seconds since boot as a float
```

Use `sys.millis()` for timeouts, debounce, and periodic-work patterns instead of
`while true do sleep_ms end`.  `sleep_ms` is safe in the script body up to ~25 s
total but must NEVER be called inside a timer callback (stalls all timers).
Scripts time out after 30 s.

There is no `os` module and no `sys.time()`/`sys.clock()` — this device has no
wall-clock RTC dependency baked into `sys`. `sys.millis()` (or `sys.uptime()`
for the same value in seconds) is the only clock; use it everywhere you would
reach for `os.time()`/`os.clock()` in standard Lua.

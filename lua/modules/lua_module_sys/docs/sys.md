# sys  —  require("sys")

```lua
sys.sleep_ms(ms)          -- yield to the RTOS scheduler (10 ms min granularity)
sys.millis()  -> integer  -- monotonic millisecond counter since boot (wraps ~49 days)
sys.uptime()  -> number   -- seconds since boot as a float
sys.time()    -> integer  -- current UTC Unix timestamp (like os.time()); 0 if clock unset
```

Use `sys.millis()` for timeouts, debounce, and periodic-work patterns instead of
`while true do sleep_ms end`.  `sleep_ms` is safe in the script body up to ~25 s
total but must NEVER be called inside a timer callback (stalls all timers).
Scripts time out after 30 s.

There is no `os` module. Use `sys.time()` where you would reach for `os.time()`
(it returns the UTC Unix timestamp), and `sys.millis()` / `sys.uptime()` for
monotonic since-boot timing (timeouts, debounce, periodic work) where you would
use `os.clock()`. `sys.time()` is UTC and returns a raw number — for a
human-readable LOCAL time string (with the device's configured timezone) call
the `get_local_time` cap instead of formatting it yourself.

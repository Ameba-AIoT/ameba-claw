# sys  —  require("sys")

- sleep_ms(ms)  -- yield to the RTOS scheduler

Safe in the script body up to ~25s total. NEVER call inside a timer callback
(it stalls all timers). Scripts time out after 30s; no unbounded loops.

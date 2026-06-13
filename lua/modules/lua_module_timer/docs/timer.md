# timer  —  require("timer")

- start(interval_ms, lua_code_string, repeat_bool) -> id
- stop(id)
- list()

Use this for anything that repeats / runs forever (never `while true`).
The callback is a CODE STRING run in a fresh state where modules
(gpio/i2c/rtc/cap/cjson/sys) are PRE-LOADED GLOBALS — do NOT require() inside it,
and never sleep inside it. The script returns immediately after start().

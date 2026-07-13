# timer  —  require("timer")

- start(interval_ms, lua_code_string, repeat_bool) -> id
- stop(id)
- list()

Use this for anything that repeats / runs forever (never `while true`).
The callback is a CODE STRING run in a fresh state where modules
(gpio/i2c/rtc/cap/cjson/sys) are PRE-LOADED GLOBALS — do NOT require() inside it,
and never sleep inside it. The script returns immediately after start().

The callback's Lua state is NOT the caller's state: it cannot see any local
(or global) variable, table, or object (widgets, sockets, ...) created by the
script that called `start()`. It can only use the pre-loaded modules above and
literals baked into the code string itself. If you need to repeatedly touch
objects you already created — e.g. an `lvgl` widget tree — this module is the
wrong tool; see `lvgl.md`'s "Periodic updates" section for the pattern that
runs in your own state instead.

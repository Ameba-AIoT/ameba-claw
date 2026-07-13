# coroutine — Lua 5.4 standard coroutines

Standard Lua 5.4 coroutine library is fully available. No extra require needed.

```lua
local co = coroutine.create(function(a)
    print("got", a)
    local b = coroutine.yield(a + 1)
    return b * 2
end)
coroutine.resume(co, 10)   -- prints "got 10", returns true, 11
coroutine.resume(co, 5)    -- returns true, 10
```

API: `coroutine.create`, `resume`, `yield`, `wrap`, `status`, `isyieldable`, `running`.

## ⚠ Critical limitation: cap.call is synchronous

`cap.call(...)` (web_search, web_fetch, etc.) makes a **blocking network call**.
Coroutines cannot yield across a blocking C call — `cap.call` will block the
entire Lua state until it returns, regardless of any surrounding coroutine.

**Consequence:** you cannot use coroutines to make `cap.call` non-blocking.
If you need a non-blocking network fetch from a real-time loop (e.g. a clock
that must tick every second), offload the fetch to a separate `lua_run_async`
job and communicate results via a VFS file.

```lua
-- WRONG: this still blocks the main loop for the duration of cap.call
local co = coroutine.create(function()
    return cap.call("web_search", ...) -- blocks here, no way to yield
end)

-- RIGHT: run the fetch in a background job, share via file
-- See weather_worker pattern: vfs:/scripts/weather_worker.lua
```

## When coroutines ARE useful

- Splitting long computations into resumable steps driven by `event.wait`
- Simple state machines where each `yield` marks a wait point
- Generator patterns over in-memory data (no I/O)

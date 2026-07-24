# captouch — require("captouch")

Self-capacitance touch-key driver for the RTL8721F (AmebaGreen2). `require("captouch")`
returns a flat table with one constructor, `captouch.new(...)`, which returns a **handle
object** — the usual pattern is `local dev = captouch.new(...)`.

Uses the Realtek fwlib raw API (`CapTouch_Init`, `CapTouch_Cmd`, `CapTouch_GetChAveData`,
`CapTouch_GetChBaseline`, `CapTouch_GetChDiffThres`, etc.) in polling mode; no interrupts.

## Supported pins (RTL8721F)

| Pin   | CapTouch Channel |
|-------|-----------------|
| PA_20 | CH0             |
| PA_19 | CH1             |
| PA_18 | CH2             |
| PA_17 | CH3             |
| PA_16 | CH4             |
| PA_24 | CH5             |
| PA_23 | CH6             |
| PA_22 | CH7             |
| PA_21 | CH8             |

> **Note:** PA_18 and PA_19 also share the SWD debug port — avoid using them when a
> debugger is connected.

## API

```lua
-- Constructor
local dev = captouch.new(pin1 [, pin2] [, opts])
-- pin1, pin2 : string pin name ("PA_16") or raw PinName integer
-- opts (table, all fields optional):
--   threshold   : integer  diff threshold, 1..4095  (default 1600)
--   mbias       : integer  common mbias current 0..65535 (default 0x800)
--   n_noise_thr : integer  negative noise threshold 0..4095 (default 800)
--   p_noise_thr : integer  positive noise threshold 0..4095 (default 800)
--   interval_ms : integer  scan interval in ms, 1..4000 (default 60)
--   ch_threshold: table    per-key diff threshold {val1, val2} (0 → use common)
--   ch_mbias    : table    per-key mbias {val1, val2} (0 → use common)
--   name        : string   device name (default "touch_keys")

-- Handle methods
local state = dev:read()
-- Returns a table:
--   state.count         → number of keys
--   state.any_pressed   → boolean
--   state.pressed_count → integer
--   state.keys[i]       → per-key table (see below)
--     .index     (1-based)
--     .channel   (0..8)
--     .pin       (raw PinName integer)
--     .pressed   (boolean)
--     .smooth    (latest averaged ADC count)
--     .benchmark (baseline ADC count)
--     .delta     (benchmark - smooth; positive when touched)
--     .threshold (current diff threshold)

local ok = dev:is_pressed(index)   -- index: 1-based key index → boolean

dev:set_threshold(index, val)       -- index 1-based; val 1..65535
dev:set_mbias(index, val)           -- index 1-based; val 0..65535
dev:set_scan_interval(val)          -- raw reg value 0..4095 (not ms)
local st = dev:get_ch_status(index) -- → integer (1=channel enabled, 0=disabled)
local name = dev:name()             -- → string device name

dev:close()  -- release channels; disable peripheral when last handle is closed
```

## Examples

```lua
local captouch = require("captouch")
local sys = require("sys")

-- Open two keys on PA_16 (CH4) and PA_17 (CH3)
local dev = captouch.new("PA_16", "PA_17", {
    interval_ms  = 10,
    ch_mbias     = {0x590, 0x560},
    ch_threshold = {80, 50},
    n_noise_thr  = 40,
    p_noise_thr  = 40,
})

sys.sleep_ms(1000)  -- wait for baseline calibration

for i = 1, 20 do
    local s = dev:read()
    for _, k in ipairs(s.keys) do
        if k.pressed then
            print("key " .. k.index .. " pressed  delta=" .. k.delta)
        end
    end
    sys.sleep_ms(100)
end

dev:close()
```

## Concurrency & resources

**Resource lifecycle:** `captouch.new(...)` enables the ADC peripheral clock and
initializes the CapTouch controller (first handle only). Subsequent `captouch.new`
calls with different pins add channels without re-initializing. `dev:close()` disables
the assigned channels and decrements the reference count; when the last handle is
closed the controller is disabled and the ADC clock is gated off.

No GDMA or timer resources are allocated — the driver uses polling only.

**Concurrency contract (for callers):**

- **Write operations are mutex-protected:** `set_threshold()`, `set_mbias()`, and
  `set_scan_interval()` each take `s_ctc_lock` around the register write. `new()` and
  `close()` also hold the lock (guards init/refcount). `read()`, `is_pressed()`, and
  `get_ch_status()` do **not** take the mutex — they issue direct read-only register
  accesses. Concurrent reads from two tasks are safe in practice (no write side-effects),
  but a read racing with `set_threshold()` may observe the old value.
- **Multi-step sequences are NOT atomic.** A read-modify-write sequence across multiple
  API calls (e.g. `read()` followed by `set_threshold()`) can be interleaved by another
  task between the two calls. If you need atomicity, serialize access at the Lua level
  (e.g. run the sequence in a single script rather than from two concurrent tasks).
- **Handles cannot be shared across tasks.** Each handle should be created and used
  within a single script/task. Do not pass a handle returned from `captouch.new()` to
  another `lua_run` task — the `closed` flag and underlying channel list are not safe
  for cross-task transfer.
- **`close()` is safe to call from any task** that owns the handle.

**Safe concurrent pattern (two tasks each with their own handle):**
```lua
-- Task A
local devA = captouch.new("PA_16", {ch_mbias={0x590}, ch_threshold={80}})
sys.sleep_ms(1000)
local s = devA:read()    -- safe: read-only register access, no lock needed
devA:close()

-- Task B (concurrent with A)
local devB = captouch.new("PA_17", {ch_mbias={0x560}, ch_threshold={50}})
sys.sleep_ms(1000)
local s = devB:read()    -- safe: read-only register access
devB:close()
```

**Unsafe pattern (shared handle, cross-task):**
```lua
-- DO NOT do this: dev created in one task, passed to another
-- The closed flag and channel list are per-handle, not shared safely.
```

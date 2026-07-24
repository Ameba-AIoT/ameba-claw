# adc — require("adc")

Reads analog voltages from ADC-capable GPIO pins on the RTL8721F.
`require("adc")` returns a table with one constructor; the constructor returns a
**handle** (userdata object) that owns 1–8 channels. All conversion methods are
called on the handle.

The ADC is a **single hardware instance**: only one handle may be open at a time.
A second `adc.new()` while a handle is open raises an error. Close the existing
handle before opening another.

## Supported pins (RTL8721F, external channels)

| Pin    | ADC Channel |
|--------|-------------|
| PA_20  | CH0         |
| PA_19  | CH1         |
| PA_18  | CH2         |
| PA_17  | CH3         |
| PA_15  | CH4         |
| PA_14  | CH5         |
| PA_13  | CH6         |
| PA_12  | CH7         |

> PA_18 and PA_19 are shared with SWD. Prefer PA_13 / PA_14 / PA_20 for testing.

## API

```lua
local adc = require("adc")

-- Constructor
local ch = adc.new(pin1 [, pin2, ...] [, mode])
-- pin:  string name, e.g. "PA_13"  (1..8 pins accepted)
-- mode: "sw" (default) or "auto"
-- Returns a handle. Raises if a handle is already open or a pin is not ADC-capable.

-- SW-trigger mode (default)
local t  = ch:read()            -- → {[ch_id] = mV, ...}  SW trigger all channels, blocking
ch:trigger([enable])            -- start (true/nil) or stop (false) non-blocking SW conversion
local ok = ch:readable()        -- → boolean; true when ≥1 sample is in the FIFO
local id, raw = ch:read_raw()   -- → ch_id, raw16  |  nil when FIFO is empty

-- AUTO mode  (requires adc.new(..., "auto"))
local arr = ch:read_auto(count) -- → array of {ch=id, mv=mV}; count: 1..256

-- Lifecycle
ch:close()                      -- disable ADC, gate RCC clock, restore pins to GPIO
                                 -- also called automatically by the GC
```

### Return value details

| Method      | Returns                                                    |
|-------------|------------------------------------------------------------|
| `read()`    | Table keyed by hardware channel ID, value = millivolts     |
| `read_raw()`| `ch_id` (integer), `raw` (0–65535) or `nil` when empty    |
| `read_auto` | Array of `{ch = hw_channel_id, mv = millivolts}`           |

## Examples

```lua
local adc = require("adc")
local sys = require("sys")

-- SW-trigger: single channel, 5 readings
local ch = adc.new("PA_13")
for i = 1, 5 do
    local r = ch:read()
    print(string.format("sample %d: %d mV", i, r[6]))  -- CH6 = PA_13
    sys.sleep_ms(200)
end
ch:close()

-- SW-trigger: two channels at once
local ch = adc.new("PA_13", "PA_14")
local r = ch:read()
print(string.format("CH6=%d mV  CH5=%d mV", r[6], r[5]))
ch:close()

-- AUTO mode: 8 samples round-robin over two channels
local ch = adc.new("PA_13", "PA_14", "auto")
local results = ch:read_auto(8)
for _, r in ipairs(results) do
    print(string.format("ch=%d  %d mV", r.ch, r.mv))
end
ch:close()

-- Non-blocking SW trigger / drain flow
local ch = adc.new("PA_13", "PA_14")
ch:trigger()                          -- start conversion (non-blocking)
local polls = 0
repeat polls = polls + 1 until ch:readable() or polls > 10000
ch:trigger(false)                     -- stop; FIFO retains samples already captured
repeat
    local id, raw = ch:read_raw()
    if id == nil then break end
    print(string.format("ch=%d raw=%d", id, raw))
until false
ch:close()
```

## Concurrency & resources

**Lifecycle:** `adc.new(...)` → use methods → `ch:close()`.
`close()` disables the ADC peripheral, gates the RCC clock, and restores pin mux
to GPIO. The GC calls `close()` automatically on collection, but explicit `close()`
is preferred to release hardware promptly.

**Single hardware instance — one handle at a time:**
Only one handle may be open. A second `adc.new()` while a handle is open raises
`"adc: another channel is open; close it first"`. Always `close()` before opening
a new handle, even with different pins or modes.

**Per-call thread safety:**
All methods except `readable()` acquire a global mutex (`s_adc_lock`) for the
duration of the hardware operation. `readable()` reads the FIFO status register
directly without locking. Individual locked API calls are thread-safe: concurrent
`lua_run` tasks may call them on the same open handle, and calls are serialized
by the mutex.

**Multi-step atomicity:**
A `trigger() → readable() → read_raw()` sequence is **not atomic**. Another task
may interleave between steps. If two scripts share one handle and both run the
trigger/drain flow concurrently, they will corrupt each other's FIFO reads.
Coordinate externally (e.g. one dedicated ADC task, or `thread.channel` hand-off)
when concurrent multi-step flows are required.

**Handle sharing:**
A handle can be passed to another task (plain Lua userdata), but see the atomicity
note above.

```lua
-- Safe: one script, sequential calls
local ch = adc.new("PA_13")
local r = ch:read()   -- internally mutex-protected, atomic
ch:close()

-- Safe: two concurrent tasks calling read() on the same handle
-- (each read() is a single mutex-protected call)
-- Task A: local ra = ch:read()
-- Task B: local rb = ch:read()   -- serialized, both succeed

-- Unsafe: split trigger/drain across two concurrent tasks
-- Task A: ch:trigger(); while not ch:readable() do end; ch:trigger(false)
-- Task B: ch:trigger(false)  -- races with Task A's poll loop
```

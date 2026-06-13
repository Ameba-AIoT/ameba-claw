# Lua ADC

This module reads voltage from ADC-capable GPIO pins using both software-trigger
mode and automatic (continuous) mode. It wraps the Realtek fwlib ADC driver and
exposes calibrated voltage readings via `ADC_GetVoltage()`.

The ADC is a single hardware instance: only **one** handle may be open at a time.
Opening a channel configures the op-mode and channel list once; a second
`adc.new()` while a handle is still open is rejected. Close the current handle
before opening another.

## Supported pins (RTL8721F)

| Pin    | ADC Channel |
|--------|-------------|
| PA\_20 | CH0         |
| PA\_19 | CH1         |
| PA\_18 | CH2         |
| PA\_17 | CH3         |
| PA\_15 | CH4         |
| PA\_14 | CH5         |
| PA\_13 | CH6         |
| PA\_12 | CH7         |

> **Note:** PA\_18 and PA\_19 are shared with the SWD debug port. Prefer PA\_13 /
> PA\_14 / PA\_20 for testing.

## API

### `adc.new(pin1 [, pin2, ...] [, mode])` → channel handle

Open 1..12 ADC channels in a single call and return one handle that owns them all.
Each `pin` is a pin name string (e.g. `"PA_13"`). The optional trailing `mode`
string selects the trigger mode:

| `mode`  | Behaviour                                         |
|---------|---------------------------------------------------|
| `"sw"`  | Software-trigger mode (default). Use `ch:read()` / `ch:trigger()`. |
| `"auto"`| Automatic mode. Use `ch:read_auto(count)`.        |

```lua
local ch = adc.new("PA_13")                  -- single channel, SW-trigger
local ch = adc.new("PA_13", "PA_14")         -- two channels, SW-trigger
local ch = adc.new("PA_13", "PA_14", "auto") -- two channels, AUTO mode
```

Errors if a handle is already open, if no pins are given, or if a pin is not
ADC-capable.

### `ch:read()` → table `{[ch_id] = mv, ...}`

(SW-trigger mode) Fire one SW conversion across all channels in the handle and
return a table keyed by hardware channel ID, each value in millivolts. Blocking.

```lua
local r = ch:read()
print(r[6])   -- mV on CH6 (PA_13)
```

### `ch:trigger([enable])`

(SW-trigger mode) Start or stop SW conversion for all channels, non-blocking.
`ch:trigger()` (or `ch:trigger(true)`) starts it; the hardware keeps converting
(filling the FIFO) while it is on, so stop it with `ch:trigger(false)` before
draining. Typical flow:

```lua
ch:trigger()                       -- start
while not ch:readable() do end     -- (or poll with a timeout)
ch:trigger(false)                  -- stop so the FIFO stops growing
repeat
    local id, raw = ch:read_raw()  -- drain
    if id == nil then break end
    print(string.format("ch=%d raw=%d", id, raw))
until false
```

### `ch:readable()` → boolean

Return `true` when at least one sample is available in the FIFO.

### `ch:read_raw()` → `ch_id, raw` | nil

Pop one sample from the FIFO, returning its hardware channel ID and the raw
16-bit ADC count (no mV conversion). Returns `nil` when the FIFO is empty. Use
`ADC_GetVoltage()`-equivalent scaling yourself, or prefer `ch:read()` for
calibrated millivolts.

### `ch:read_auto(count)` → array of `{ch = hw_channel_id, mv = millivolts}`

(AUTO mode only) Read `count` samples (1..256) in automatic mode across the
channels the handle was opened with. The ADC samples channels in round-robin
order. Requires the handle to have been opened with `adc.new(..., "auto")`.

```lua
local ch = adc.new("PA_13", "PA_14", "auto")
local results = ch:read_auto(8)
for _, r in ipairs(results) do
    print(string.format("ch=%d  %d mV", r.ch, r.mv))
end
ch:close()
```

### `ch:close()`

Release the channels and restore the pins to GPIO mode. Also called
automatically by the GC.

## Quick examples

```lua
local adc = require("adc")
local sys = require("sys")

-- Software-trigger single read on one pin
local ch = adc.new("PA_13")
for i = 1, 5 do
    local r = ch:read()
    print(string.format("sample %d: %d mV", i, r[6]))
    sys.sleep_ms(200)
end
ch:close()

-- Auto mode: read both PA_13 and PA_14
local ch = adc.new("PA_13", "PA_14", "auto")
local results = ch:read_auto(8)
for _, r in ipairs(results) do
    print(string.format("ch=%d  %d mV", r.ch, r.mv))
end
ch:close()
```

## Test

```
AT+CLAW=adc        -- loopback: PA_13<->PA_25, PA_14<->PA_26 must be wired
AT+CLAW=adc,ext    -- external supply on PA_13 and PA_14
```

## Hardware requirements

| Test mode    | Wiring needed                              |
|--------------|--------------------------------------------|
| `loopback`   | PA\_13 ↔ PA\_25, PA\_14 ↔ PA\_26 (jumpers) |
| `ext_supply` | External 0–3.3 V source on PA\_13, PA\_14  |

## Notes

- Only one `adc.new()` handle may be open at a time; close it before opening another.
- A single `adc.new()` call opens 1..12 channels together; do not call `new` per channel.
- `ch:read_auto()` is only valid on a handle opened with `"auto"` mode.
- Hardware voltage range: 0–3300 mV (0–3.3 V); calibration may read slightly higher.

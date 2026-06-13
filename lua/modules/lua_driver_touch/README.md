# Lua Touch (CapTouch, polling)

This module reads capacitive touch key states from Lua using the Realtek fwlib
CapTouch driver in polling mode. It wraps `CapTouch_Init`, `CapTouch_GetChAveData`,
`CapTouch_GetChBaseline`, and `CapTouch_GetChDiffThres` to expose per-key press
state, smooth value, baseline, delta, and threshold — mirroring the data model of
the reference touch driver.

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

> **Note:** PA_18 and PA_19 share the SWD debug port. Use PA_20 (CH0) for
> testing to avoid conflicts.

## How to call

```lua
local touch = require("touch")
local sys   = require("sys")

-- Open one or two CapTouch keys
local dev = touch.new("PA_20")                       -- single key, defaults
local dev = touch.new("PA_16", "PA_17")              -- two keys
local dev = touch.new("PA_16", {threshold=1600, interval_ms=100})
local dev = touch.new("PA_16", "PA_17", {
    threshold   = 1600,       -- CT_DiffThrehold  (0..4095, default 1600)
    mbias       = 0x800,      -- CT_MbiasCurrent common default (0..65535, default 0x800)
    n_noise_thr = 800,        -- CT_ETCNNoiseThr  (0..4095, default 800)
    p_noise_thr = 800,        -- CT_ETCPNoiseThr  (0..4095, default 800)
    interval_ms = 100,        -- scan interval in ms, converted to CT_ScanInterval (1..4000, default 60)
    ch_mbias    = {0x590, 0x560},  -- per-channel mbias overrides by key order (0 = use common mbias)
    name        = "mypad",
})

-- Wait for baseline to stabilise after init
sys.sleep_ms(1000)

-- Read all key states at once
local sample = dev:read()
-- sample.count         → number of keys
-- sample.any_pressed   → boolean
-- sample.pressed_count → integer
-- sample.keys[i]       → per-key table (see below)

-- Per-key table fields:
--   index     (1-based)
--   channel   (0-8)
--   pin       (raw PinName integer)
--   pressed   (boolean)
--   smooth    (latest averaged ADC count)
--   benchmark (baseline ADC count)
--   delta     (benchmark - smooth; positive when touched)
--   threshold (diff threshold for press detection)

-- Check a single key (1-based index)
local pressed = dev:is_pressed(1)

-- Get device name
local name = dev:name()

-- Close when done (also cleaned up by GC)
dev:close()
```

## Example: poll a touch key

```lua
local touch = require("touch")
local sys   = require("sys")

local dev = touch.new("PA_20")
sys.sleep_ms(1000)  -- wait for baseline

for i = 1, 20 do
    local s = dev:read()
    local k = s.keys[1]
    print(string.format("smooth=%d baseline=%d delta=%d pressed=%s",
        k.smooth, k.benchmark, k.delta, tostring(k.pressed)))
    sys.sleep_ms(100)
end
dev:close()
```

## Notes

- **Polling only**: no interrupt handler is registered. Call `read()` or
  `is_pressed()` to poll the current state.
- **Baseline calibration**: the hardware ETC circuit auto-calibrates the baseline.
  Wait at least 500–1000 ms after `touch.new()` before reading for accurate data.
- **Touch detection**: `pressed = (baseline - smooth) ≥ threshold`. Raw counts
  decrease when a finger touches the pad (increased capacitance).
- The CapTouch peripheral shares the ADC clock. The clock is enabled on the first
  `touch.new()` and disabled when the last handle is closed.
- Multiple simultaneous handles are supported; each manages its own channels.
  The CapTouch controller is re-initialized only on the first `touch.new()`.

## Hardware requirements for testing

- **No external hardware required** for basic validation — the test verifies that
  the API initializes and returns data without errors.
- To observe `pressed=true`, physically touch a conductive pad wired to PA_20 with
  a finger or a metal object connected to ground.

# magnetometer — require("magnetometer")

Chip-agnostic 3-axis magnetometer driver over I2C.
`require("magnetometer")` returns a flat table with one function: `new`.
Currently the only backend is the Bosch **BMM150**; more chips can be added
without changing the Lua API.

## API

```lua
-- Create a handle.  All bus parameters come from the caller (board.json).
--   sda      : SDA pin string ("PA_26") or integer PinName     (required)
--   scl      : SCL pin string ("PA_25") or integer PinName     (required)
--   i2c      : controller index 0 or 1                         (default 0)
--   addr     : 7-bit I2C address 0x10–0x13 for BMM150          (default 0x13)
--   freq     : I2C clock in Hz                                 (default 100000)
--   chip     : backend name, e.g. "bmm150"                     (default "bmm150")
--   int_gpio : pin wired to sensor INT, string or PinName      (optional)
local dev = magnetometer.new({ sda="PA_26", scl="PA_25", int_gpio="PB_8" })

-- Read all three axes + status in ONE conversion cycle.
-- Returns a table:
--   s.magnetic     = { x=, y=, z= }   compensated values in µT
--   s.temperature  = 0.0              BMM150 has no temperature sensor
--   s.status       = integer          BMM150_REG_INTERRUPT_STATUS bitfield
--   s.calibrated   = false            hard/soft-iron calibration not implemented
local s = dev:read()

-- Temperature (always 0 for BMM150).
local t = dev:read_temperature()

-- Raw interrupt status register (BMM150_REG_INTERRUPT_STATUS 0x4A).
-- Reads the register directly over I2C; does not require the INT pin to be wired.
local st = dev:read_int_status()

-- Backend chip name string.
local name = dev:name()   -- "bmm150"

-- Release the handle (I2C bus reference is dropped; controller clock stays on).
dev:close()
```

## Minimal example

```lua
local mag = require("magnetometer")
local sys = require("sys")
local dev = mag.new({ sda = "PA_26", scl = "PA_25" })
for _ = 1, 10 do
    local s = dev:read()
    print(s.magnetic.x, s.magnetic.y, s.magnetic.z)
    sys.sleep_ms(200)
end
dev:close()
```

## Wiring (BMM150 breakout, I2C mode)

| BMM150 pin | Connect to     | Note                                       |
|------------|----------------|--------------------------------------------|
| VCC        | 3V3            |                                            |
| GND        | GND            |                                            |
| SCL        | PA_25          | I2C0 clock (shared with OLED)              |
| SDA        | PA_26          | I2C0 data  (shared with OLED)              |
| INT        | PB_8 (optional)| Data-ready; poll via read_int_status()     |

The breakout board must have the **I2C solder bridge** closed (CSB pulled high).
SDO=GND → address 0x10; SDO=VCC → address 0x13.  If the primary address
fails, the driver automatically tries all four valid addresses (0x10–0x13).

## Concurrency & resources

**Resource lifecycle:**
- `new()` allocates chip state on the heap and acquires a reference to the
  shared I2C controller (the same refcounted lock as the `i2c` module).
- `close()` frees the state and drops the I2C reference.  The controller clock
  stays on (shared with other handles).  Always call `close()` when done.
- GC also calls `close()` automatically, but explicit `close()` is preferred.
- Re-opening after `close()` is safe; the same `new()` call works again.

**Per-call concurrency:**
- Each `read()` / `read_int_status()` call holds the per-controller I2C mutex
  for the duration of the I2C transaction.  Single API calls are thread-safe.
- Two Lua tasks can each have their own handle pointing to the same controller;
  their individual calls will not interleave on the bus.

**Multi-step atomicity:**
- A `read()` triggers one FORCED conversion and reads the result in a single
  Bosch SensorAPI call sequence.  The conversion and read together are NOT
  atomic from other tasks' perspective (another task could acquire the lock
  between two underlying I2C operations inside the Bosch API).
- For the typical use case (periodic independent samples), this is fine.
  If strict atomic snapshot across axes is required, serialize access at the
  application level.

**Handle sharing:**
- A handle is NOT safe to share across tasks.  Give each task its own handle
  from `new()`; handles are small (a few dozen bytes + chip state).

**Safe concurrent example:**
```lua
-- Task A and Task B each call new() independently — the bus lock prevents
-- their I2C transactions from interleaving.
local devA = magnetometer.new({ sda="PA_26", scl="PA_25" })
-- (in another task)
local devB = magnetometer.new({ sda="PA_26", scl="PA_25" })
-- Both devA:read() and devB:read() are individually safe.
```

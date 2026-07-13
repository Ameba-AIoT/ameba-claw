# imu — require("imu")

Chip-agnostic IMU driver: 3-axis accelerometer + 3-axis gyroscope + on-die
temperature, over I2C. `require("imu")` returns a flat table with one function:
`new`. Multi-backend design — one Lua API,
selectable chip backend. Currently the only backend is the InvenSense
**MPU-6050 (GY-521)**; more chips can be added without changing this API.

All bus traffic shares the `i2c` driver's per-controller lock, so an `imu`
handle and any `i2c` device on the **same controller** can never interleave
transactions or stomp each other's config.

## API

```lua
-- Create a handle bound to an I2C controller.
-- All pins/addresses come from the caller (board.json) — nothing is hard-coded.
--   sda   : SDA pin, "PA_N"/"PB_N"/... string or integer PinName    (required)
--   scl   : SCL pin, same form                                       (required)
--   i2c   : controller index 0 or 1                    (default 0)
--   addr  : 7-bit device address, 0x68 (AD0=GND) / 0x69 (AD0=VDDIO)  (default 0x68)
--   freq  : I2C clock in Hz                             (default 400000)
--   chip  : backend name, e.g. "mpu6050"               (default "mpu6050")
local dev = imu.new({ sda = "PA_26", scl = "PA_25", i2c = 0, addr = 0x68 })

-- Read status + all six axes + temperature in ONE 15-byte burst (from
-- INT_STATUS 0x3A), so every field belongs to the same measurement instant.
-- Values are RAW signed 16-bit integers (apply scaling below for units).
local s = dev:read()
--  s.accel  = { x=, y=, z= }  raw accelerometer counts
--  s.gyro   = { x=, y=, z= }  raw gyroscope counts
--  s.temp   =                  raw temperature counts
--  s.status =                  INT_STATUS bitfield (bit0 = data-ready)

-- Individual reads (each returns three raw integers).
local ax, ay, az = dev:read_accel()
local gx, gy, gz = dev:read_gyro()

-- Temperature in degrees Celsius (float): raw/340 + 36.53.
local celsius = dev:read_temperature()

-- INT_STATUS register (integer bitfield); bit0 = data-ready.
-- (read() already returns this as s.status from the same burst — this call is
--  for when you only want the status without a full sample.)
local st = dev:read_int_status()

-- WHO_AM_I register: 0x68 on a healthy MPU-6050.
local id = dev:who_am_i()

-- Backend name string.
local name = dev:name()          -- the selected chip, e.g. "mpu6050"

-- Release the handle (controller stays configured for the next new()).
dev:close()
```

## Scaling to physical units

The default full-scale ranges are the widest available:

| Quantity      | Full scale  | Sensitivity     | Convert with            |
|---------------|-------------|-----------------|-------------------------|
| Accelerometer | ±16 g       | 2048 LSB/g      | `g = raw / 2048.0`      |
| Gyroscope     | ±2000 dps   | 16.4 LSB/dps    | `dps = raw / 16.4`      |
| Temperature   | —           | 340 LSB/°C      | `°C = raw / 340 + 36.53`|

At rest the accelerometer reads ~1 g (≈2048 counts) on whichever axis points
along gravity, ~0 on the other two.

## Minimal example

```lua
local imu = require("imu")
local sys = require("sys")
local dev = imu.new({ sda = "PA_26", scl = "PA_25" })   -- addr defaults to 0x68
for _ = 1, 20 do
    local s = dev:read()
    print(string.format("a=(%.2f,%.2f,%.2f)g  t=%.1fC",
          s.accel.x/2048, s.accel.y/2048, s.accel.z/2048,
          s.temp/340 + 36.53))
    sys.sleep_ms(100)
end
dev:close()
```

## Wiring (GY-521 breakout)

| GY-521 pin | Connect to        | Note                                  |
|------------|-------------------|---------------------------------------|
| VCC        | 3V3               | onboard LDO also accepts 5 V          |
| GND        | GND               |                                       |
| SCL        | SCL pin (PA_25)   | I2C clock                             |
| SDA        | SDA pin (PA_26)   | I2C data                              |
| AD0        | GND → 0x68 / VCC → 0x69 | selects the LSB of the I2C address |
| INT        | (optional) any GPIO | data-ready interrupt; polling used by default |
| XCL / XDA  | leave open        | auxiliary master bus (not used)       |

Both pins must have pull-ups to 3V3 (the driver enables the internal pull-ups;
the GY-521 board also has its own).

## Concurrency & resources

- **Shares the `i2c` driver's per-controller lock** (via its C bus API), held
  for the *whole* register transaction. So this IMU and any `i2c` device on the
  same controller (e.g. an SH1106 OLED on I2C0) take the **same** lock and can
  never interleave mid-transfer or overwrite each other's config — there is one
  bus owner per controller, not one per module.
- The controller is configured on the **first** `new()`; a later `new()` that
  requests different pins/frequency on the same controller raises an error
  rather than silently reconfiguring a bus another device is using.
- **The I2C clock is left on** after `close()` — re-enabling costs nothing and
  disabling could cut off another flow sharing the controller. `close()` only
  invalidates the Lua handle.
- **Not safe to share a handle across tasks**: give each task its own handle
  from `new()`. Handles are tiny.
- **Multi-step atomicity**: use `read()` when you need accel *and* gyro from the
  same instant — it is a single burst; separate `read_accel()`/`read_gyro()`
  calls sample at different times.

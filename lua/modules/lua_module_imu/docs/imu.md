# imu — require("imu")

Chip-agnostic IMU driver: 3-axis accelerometer + 3-axis gyroscope + on-die
temperature, over I2C. `require("imu")` returns a flat table with one function:
`new`. Multi-backend design — one Lua API, selectable chip backend.

Supported backends: **MPU-6050** (InvenSense, default) and **BMI270** (Bosch).

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
--   addr  : 7-bit device address (default 0x68)
--   freq  : I2C clock in Hz                             (default 400000)
--   chip  : backend name: "mpu6050" or "bmi270"        (default "mpu6050")
local dev = imu.new({ sda = "PA_26", scl = "PA_25", chip = "mpu6050" })
-- BMI270 example (SCL=PA_6, SDA=PA_7):
local dev = imu.new({ sda = "PA_7",  scl = "PA_6",  chip = "bmi270" })

-- Read status + all six axes + temperature in ONE burst (MPU-6050: from
-- INT_STATUS 0x3A; BMI270: from ACC_X_LSB 0x0C, which also covers
-- INT_STATUS_0), so every field belongs to the same measurement instant.
-- Values are RAW signed 16-bit integers (apply scaling below for units).
local s = dev:read()
--  s.accel  = { x=, y=, z= }  raw accelerometer counts
--  s.gyro   = { x=, y=, z= }  raw gyroscope counts
--  s.temp   =                  raw temperature counts
--  s.status =                  INT_STATUS bitfield, meaning is chip-specific:
--                               MPU-6050 bit0 = data-ready; BMI270 is the
--                               feature-engine INT_STATUS_0 bits, which read
--                               0x00 unless a feature interrupt (sig-motion,
--                               step counter, tap, ...) is enabled — this
--                               driver doesn't enable any, so don't use it as
--                               a liveness check, watch accel/gyro instead.

-- Individual reads (each returns three raw integers).
local ax, ay, az = dev:read_accel()
local gx, gy, gz = dev:read_gyro()

-- Temperature in degrees Celsius (float): raw/340 + 36.53.
local celsius = dev:read_temperature()

-- INT_STATUS register, same value and bit meaning as s.status above (this
-- call is for when you only want the status without a full sample).
local st = dev:read_int_status()

-- WHO_AM_I register: 0x68 on a healthy MPU-6050.
local id = dev:who_am_i()

-- Backend name string.
local name = dev:name()          -- the selected chip, e.g. "mpu6050"

-- Release the handle (controller stays configured for the next new()).
dev:close()
```

## Scaling to physical units

Default full-scale ranges (both chips use the same Lua scaling):

| Chip    | Quantity      | Full scale  | Sensitivity      | Convert with              |
|---------|---------------|-------------|------------------|---------------------------|
| MPU6050 | Accelerometer | ±16 g       | 2048 LSB/g       | `g = raw / 2048.0`        |
| MPU6050 | Gyroscope     | ±2000 dps   | 16.4 LSB/dps     | `dps = raw / 16.4`        |
| MPU6050 | Temperature   | —           | 340 LSB/°C       | `°C = raw/340 + 36.53`    |
| BMI270  | Accelerometer | ±16 g       | 2048 LSB/g       | `g = raw / 2048.0`        |
| BMI270  | Gyroscope     | ±2000 dps   | 16.384 LSB/dps   | `dps = raw / 16.384`      |
| BMI270  | Temperature   | —           | 512 LSB/°C       | `°C = 23 + raw / 512.0`   |

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

## Wiring

**MPU-6050 (GY-521)**

| Pin  | Connect to             | Note                              |
|------|------------------------|-----------------------------------|
| VCC  | 3V3                    |                                   |
| GND  | GND                    |                                   |
| SCL  | PA_25                  | I2C clock                         |
| SDA  | PA_26                  | I2C data                          |
| AD0  | GND (addr 0x68)        |                                   |
| INT  | (optional) any GPIO    | polling used by default           |

**BMI270**

| Pin  | Connect to             | Note                              |
|------|------------------------|-----------------------------------|
| VCC  | 3V3                    |                                   |
| GND  | GND                    |                                   |
| SCL  | PA_6                   | I2C clock                         |
| SDA  | PA_7                   | I2C data                          |
| SDO  | GND (addr 0x68)        | or VDD for addr 0x69              |
| INT  | PA_8 (optional)        | not used in driver; polling only  |

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

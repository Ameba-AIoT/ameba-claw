# IMU test (MPU-6050 / BMI270)

Hardware-in-the-loop test for the `imu` Lua module on AmebaGreen2 (RTL8721F).
Supports two backends selectable at runtime via the `chip` parameter.

## Hardware setup

**MPU-6050 (GY-521)**

| GY-521 | Board  | Note              |
|--------|--------|-------------------|
| VCC    | 3V3    |                   |
| GND    | GND    |                   |
| SCL    | PA_25  | I2C0 clock        |
| SDA    | PA_26  | I2C0 data         |
| AD0    | GND    | address 0x68      |

Leave INT, XCL, XDA unconnected.

**BMI270**

| BMI270 | Board  | Note              |
|--------|--------|-------------------|
| VCC    | 3V3    |                   |
| GND    | GND    |                   |
| SCL    | PA_6   | I2C0 clock        |
| SDA    | PA_7   | I2C0 data         |
| SDO    | GND    | address 0x68      |
| INT    | PA_8   | optional; unused  |

Both chips require 4.7 kΩ pull-ups on SCL and SDA to 3V3 (or rely on the
internal pull-ups enabled by lua_driver_i2c).

## Run

From the AT console:

```
# MPU-6050
AT+CLAW=imu,mpu6050                             # default pins (PA_26/PA_25)
AT+CLAW=imu,mpu6050,PA_26,PA_25                 # explicit sda,scl
AT+CLAW=imu,mpu6050,PA_26,PA_25,0,0x68,20,200   # + count=20 reads, interval=200 ms

# BMI270
AT+CLAW=imu,bmi270                              # default pins for bmi270 (PA_7/PA_6)
AT+CLAW=imu,bmi270,PA_7,PA_6                    # explicit sda,scl
AT+CLAW=imu,bmi270,PA_7,PA_6,0,0x68,20,200      # + count=20 reads, interval=200 ms
```

Parameters: `chip` / `sda` / `scl` / `i2c` / `addr` / `count` / `interval`.
Defaults: `count=10`, `interval=100 ms`.

Note: BMI270 initialization takes ~200 ms (config file upload). `new()` will
block during this time before returning the handle.

## Test cases

| Step | Operation | Expected |
|------|-----------|----------|
| 1 | `new()` | Returns handle, no error |
| 2 | `name()` | Returns chip string (e.g. "bmi270") |
| 3 | `who_am_i()` | 0x68 (MPU6050) or 0x24 (BMI270) |
| 4 | `read()` | accel/gyro/temp/status all numbers; accel magnitude > 500 |
| 5 | `read_accel()` | Three numbers |
| 6 | `read_gyro()` | Three numbers |
| 7 | `read_temperature()` | −10 … 85 °C |
| 8 | `read_int_status()` | Integer |
| 9 | 10× repeated sampling | No errors |
| 10 | `close()` | Handle invalidated |
| 11 | Any method after close | Error "invalid or closed" |
| 12 | `new()` with bogus pin | Error "invalid pin" |
| 13 | Re-open + read | Returns valid data |

Final line on success: `success`

## Concurrency & resources

All bus traffic goes through the shared `lua_driver_i2c` per-controller lock.
The handle holds one refcount on the I2C controller. `close()` (or GC) releases
that refcount; the bus clock stays on. One controller handles one bus at a time;
a second `new()` with different pins on the same controller errors rather than
reconfiguring.

BMI270 `new()` uploads the 8 KB feature-engine config and blocks ~200 ms. Keep
handles long-lived (open once, read many times) to avoid repeated init overhead.

If `new()` reports "bmi270 not found", check wiring, SDO strap, and pull-ups.

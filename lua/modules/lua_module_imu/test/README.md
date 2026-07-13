# IMU (MPU-6050 / GY-521) test

Hardware-in-the-loop test for the `imu` Lua module on AmebaGreen2 (RTL8721F).

## Hardware setup

Wire a GY-521 (MPU-6050) breakout to the board's I2C0 pins (from
`board.json`, EV721FL0_R03_BreadBoard):

| GY-521 | Board  |
|--------|--------|
| VCC    | 3V3    |
| GND    | GND    |
| SCL    | PA_25  |
| SDA    | PA_26  |
| AD0    | GND (address 0x68) |

Leave INT, XCL, XDA unconnected.

## Run

From the AT console:

```
AT+CLAW=imu,mpu6050                             # default pins from board.json (PA_26/PA_25)
AT+CLAW=imu,mpu6050,PA_26,PA_25                 # explicit sda,scl
AT+CLAW=imu,mpu6050,PA_26,PA_25,1,0x69          # explicit sda,scl,i2c,addr
AT+CLAW=imu,mpu6050,PA_26,PA_25,0,0x68,20,200   # + count=20 reads, interval=200 ms
```

`chip` / `sda` / `scl` / `i2c` / `addr` / `count` / `interval` are all parameters
(nothing is hard-coded); the extra CLI fields let you retarget I2C1 or address
0x69, or change the repeated-sampling loop (step 7: how many reads and the gap in
ms), without rebuilding. Omitted fields fall back to their defaults
(`count=10`, `interval=100`).

## Expected output

The test runs `new → name/who_am_i → read (accel+gyro+temp+status in one burst)
→ read_accel/read_gyro → read_temperature → read_int_status → 10× sampling →
close → closed-handle errors → invalid-pin error → re-open`. On success the last
line printed is:

```
success
```

Sanity checks the script enforces:

- `who_am_i()` returns `0x68`.
- At rest one accelerometer axis has magnitude > 500 counts (~gravity).
- `read_temperature()` is within −10…85 °C.
- Calling any method after `close()` errors with "invalid or closed".
- `new()` with a bogus pin ("PZ_99") errors with "invalid pin".

If `new()` reports `MPU6050 not found ... WHO_AM_I=0x..`, check wiring, the AD0
address strap, and the pull-ups on SDA/SCL.

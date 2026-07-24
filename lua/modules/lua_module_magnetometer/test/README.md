# lua_module_magnetometer

BMM150 3-axis magnetometer Lua driver for AmebaGreen2 (RTL8721F).

## Hardware

| Sensor       | Bosch BMM150 3-axis magnetometer (I2C mode) |
|--------------|---------------------------------------------|
| Interface    | I2C — shared with OLED on i2c0              |
| I2C addr     | 0x10 (SDO=GND, CSB bridge=I2C); auto-probes 0x12 if needed |
| Power        | 3.3 V                                       |
| INT pin      | PB_8 (optional, digital input, polled only) |

## Wiring

```
BMM150  →  RTL8721F (EV721FL0_R03_BreadBoard)
VCC     →  3V3
GND     →  GND
SCL     →  PA_25  (I2C0, shared with SH1106 OLED)
SDA     →  PA_26  (I2C0, shared with SH1106 OLED)
INT     →  PB_8   (optional)
```

**I2C mode selection:** the breakout board has a solder bridge pad labelled
"I2C".  Bridge it to connect CSB to VDDIO (I2C mode).  The "SPI" pad must be
open.  SDO left floating or tied to GND → address 0x10.

## API reference

All parameters are verified against `src/lua_module_magnetometer.c`.

### `magnetometer.new(opts)`

| Field      | Type           | Default  | Description                              |
|------------|----------------|----------|------------------------------------------|
| `sda`      | string/integer | required | SDA pin, e.g. `"PA_26"`                 |
| `scl`      | string/integer | required | SCL pin, e.g. `"PA_25"`                 |
| `i2c`      | integer        | `0`      | Controller index (0 or 1)               |
| `addr`     | integer        | `0x10`   | 7-bit I2C address (0x10–0x13)           |
| `freq`     | integer        | `100000` | I2C clock Hz                             |
| `chip`     | string         | `"bmm150"` | Backend name                           |
| `int_gpio` | string/integer | optional | INT pin, e.g. `"PB_8"`                  |

Returns a handle.  Probe tries alternate addresses (0x10–0x13) automatically
if the primary address fails.

### Handle methods

| Method                | Returns                          | Notes                              |
|-----------------------|----------------------------------|------------------------------------|
| `read()`              | table (see below)                | Triggers one FORCED conversion     |
| `read_temperature()`  | number (always 0.0)              | BMM150 has no temperature output   |
| `read_int_status()`   | integer                          | Reads BMM150_REG_INTERRUPT_STATUS  |
| `name()`              | `"bmm150"`                       |                                    |
| `close()`             | —                                | Drops I2C ref; chip state freed    |

`read()` return table:

| Field          | Type    | Description                                |
|----------------|---------|--------------------------------------------|
| `magnetic`     | table   | `{x=, y=, z=}` compensated values in µT   |
| `temperature`  | number  | Always `0.0`                               |
| `status`       | integer | BMM150_REG_INTERRUPT_STATUS bitfield       |
| `calibrated`   | boolean | Always `false` (calibration not implemented)|

## Test script

### Run

```
AT+CLAW=magnetometer
AT+CLAW=magnetometer,PA_26,PA_25
AT+CLAW=magnetometer,PA_26,PA_25,0,0x10,PB_8,10,200
```

Argument order: `sda, scl, i2c, addr, int_gpio, count, interval_ms`

### Test cases

| # | What                        | Expected                              |
|---|-----------------------------|---------------------------------------|
| 1 | `new()` with valid opts     | Returns handle, no error              |
| 2 | `name()`                    | Returns `"bmm150"`                    |
| 3 | `read()`                    | Table with all fields; at least one axis ≠ 0 |
| 4 | `read_temperature()`        | Returns `0.0`                         |
| 5 | `read_int_status()`         | Returns integer                       |
| 6 | Repeated sampling           | 10 reads printed, no errors           |
| 7 | `close()`                   | No error                              |
| 8 | Method on closed handle     | Raises "invalid or closed"            |
| 9 | Invalid pin `"PZ_99"`       | Raises "invalid pin"                  |
|10 | Re-open after close         | Second `new()` succeeds               |

## Concurrency & resources

**Init → operation → deinit cycle:**
```
new()        — acquires I2C controller reference (refcounted)
             — allocates chip-private state (heap, freed by close/GC)
             — configures INT GPIO as input if int_gpio given
             — probes chip: power-on, bmm150_init(), FORCED mode
read() / ... — each call holds the I2C controller mutex for the transaction only
close()      — drops I2C reference, frees chip state; INT GPIO left as input
             — further calls on handle raise "invalid or closed"
new() again  — safe to call immediately after close()
```

**Locks:** one `rtos_mutex` per I2C controller, owned by `lua_driver_i2c`.
All transactions (read, write) through the Bosch SensorAPI hold this mutex for
the duration of the entire I2C operation sequence.

**ISR safety:** all paths run in a Lua task context, never an ISR.

**Handle sharing:** not safe across tasks.  Each task should call `new()` to
get its own handle.  Two handles on the same controller share the bus lock and
will not interleave.

**Test case 10 (resource re-use):** verifies that `close()` correctly releases
the I2C reference, allowing a fresh `new()` to succeed on the same controller.

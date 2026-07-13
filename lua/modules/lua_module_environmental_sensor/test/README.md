# lua_module_environmental_sensor

DHT11 temperature and humidity sensor driver for AmebaGreen2 (RTL8721F).

## API reference

Parameters verified line-by-line against `src/lua_module_environmental_sensor.c`.

| Function | Parameters | Returns | Notes |
|---|---|---|---|
| `environmental_sensor.new(opts)` | `opts.pin` — pin string or integer | handle | only required field |
| `handle:read()` | — | `{temperature=N, humidity=N}` | float, C / % |
| `handle:read_temperature()` | — | number | degrees Celsius |
| `handle:read_humidity()` | — | number | relative humidity % |
| `handle:read_raw()` | — | temp_raw, humi_raw | integers ×10 (e.g. 250 = 25.0 C) |
| `handle:name()` | — | `"dht11"` | |
| `handle:close()` | — | — | marks handle invalid |

## Wiring

```
DHT11 Pin 1 (VCC)  -> 3.3 V
DHT11 Pin 2 (DATA) -> PB_8  (+ 4.7 kΩ pull-up to 3.3 V recommended)
DHT11 Pin 4 (GND)  -> GND
```

The internal GPIO pull-up (~50 kΩ) works for cables shorter than ~20 cm.
For longer cables use an external 4.7 kΩ pull-up resistor.

## Running the test

```
AT+CLAW=environmental_sensor
```

Or with an explicit pin:

```
AT+CLAW=environmental_sensor,pin=PB_8
```

Expected output (values vary by environment):

```
[environmental_sensor] opening pin=PB_8
[environmental_sensor] name: dht11
[environmental_sensor] read()
  temperature: 25.0 C
  humidity:    60.0 %
[environmental_sensor] read_temperature()
  temperature: 25.0 C
[environmental_sensor] read_humidity()
  humidity:    60.0 %
[environmental_sensor] read_raw()
  temp_raw=250  humi_raw=600
[environmental_sensor] close()
[environmental_sensor] re-open after close
  re-open ok: T=25.0 C  RH=60.0 %
success
```

## Test cases

| # | Action | Expected |
|---|---|---|
| 1 | `read()` after 2 s | table with temperature and humidity floats |
| 2 | `read_temperature()` | same value as `read().temperature` |
| 3 | `read_humidity()` | same value as `read().humidity` |
| 4 | `read_raw()` | two integers = float × 10 |
| 5 | `close()` then `new()` + `read()` | handle re-initialises; no resource error |
| 6 | sensor disconnected | `luaL_error` "no response" |

## Concurrency & resources

- **Lifecycle**: `new()` allocates an 8-byte userdata with no hardware side
  effects. `close()` marks it invalid. No peripheral clocks or DMA channels
  are acquired — the GPIO clock is enabled on the first `read()` and stays on.
- **Mutex scope**: the module mutex is held for the full 1-wire transaction
  (~20 ms host start + ~5 ms data read). Concurrent Lua tasks calling `read()`
  on any handle are serialized; the second caller blocks until the first
  completes.
- **IRQ disable scope**: `__disable_irq()` spans only the ~5 ms 40-bit read
  phase. The 20 ms host start pulse runs with interrupts enabled.
- **Re-use test** (test case 5) verifies the `init → operation → deinit`
  cycle: after `close()`, a new `new()` on the same pin must succeed and
  return valid readings, confirming no stale state is left.

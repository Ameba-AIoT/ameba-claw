# environmental_sensor — require("environmental_sensor")

DHT11 temperature and humidity sensor driver (1-wire bit-bang over GPIO).
`require("environmental_sensor")` returns a flat table with one function: `new`.

## API

```lua
-- Create a handle bound to a GPIO pin.
-- pin: string "PA_N" / "PB_N" / "PC_N" or integer PinName value.
local sensor = environmental_sensor.new({ pin = "PB_8" })

-- Read both values at once.
local t = sensor:read()          -- returns {temperature=N, humidity=N}  (float, C / %)

-- Read individual values.
local temp = sensor:read_temperature()  -- float, degrees Celsius
local humi = sensor:read_humidity()     -- float, relative humidity %

-- Read raw integers (x10, consistent with DHT22 format).
local temp_raw, humi_raw = sensor:read_raw()   -- e.g. 250, 600 for 25.0 C / 60 %

-- Sensor type string.
local name = sensor:name()       -- always "dht11"

-- Close the handle (no persistent resources; safe to call multiple times).
sensor:close()
```

## Minimal example

```lua
local env = require("environmental_sensor")
local sys = require("sys")
local s = env.new({ pin = "PB_8" })
sys.sleep_ms(2000)            -- DHT11 needs >= 1 s after power-on before first read
local t = s:read()
print(t.temperature, t.humidity)
s:close()
```

## Concurrency & resources

- **No persistent hardware resources** between reads. `close()` just marks the
  handle invalid; the GPIO clock stays on (RCC is always-on after first use).
- **Single module-level mutex** serialises all `read*()` calls across Lua tasks.
  Concurrent reads on *different* pins are queued, not interleaved.
- **IRQs are disabled** only for the timing-critical ~5 ms 40-bit read phase
  (the OS remains unaffected for the 20 ms host start pulse which runs with
  interrupts on).
- **Not safe to share a handle across tasks**: pass each task its own handle
  created with `new()`. Handles are cheap (8 bytes each).
- **Multi-step atomicity**: each `read_temperature()` / `read_humidity()` is a
  full independent DHT11 transaction. If you need temperature *and* humidity
  from the same sample, call `read()` once, not `read_temperature()` +
  `read_humidity()` separately.
- **Minimum inter-read interval**: DHT11 requires >= 1 second between reads.
  Use `sys.sleep_ms(2000)` between calls to be safe.

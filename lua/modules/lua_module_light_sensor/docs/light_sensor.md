# light_sensor — require("light_sensor")

LM393 + LDR (Light Dependent Resistor) photoresistor sensor module driver.
`require("light_sensor")` returns a flat table with one function: `new`.

## Hardware

The LM393 comparator drives DO LOW when light intensity exceeds the
potentiometer threshold, and HIGH when below:

- **DO = 0** → bright (light above threshold)
- **DO = 1** → dark   (light below threshold)

Adjust the blue potentiometer to set the trigger threshold. Wire the module:

```
Module VCC  → 3.3 V
Module GND  → GND
Module DO   → any GPIO input pin (see board.json device "light_sensor" for your board's pin)
```

> **Pin comes from board.json.** Look up the DO pin at runtime from the
> `"light_sensor"` device entry in `board.json` — do NOT hardcode a pin name.
> ADC is NOT applicable; this is a digital-only module (3-pin variant, no AO).
> No `board_query_peripheral` call is needed: board.json already declares the pin.

## API

```lua
-- Create a handle bound to a GPIO pin.
-- do_pin: string "PA_N" / "PB_N" or integer PinName value.
-- Read from board.json: cfg.devices[*].id=="light_sensor" → params.pin
local sensor = light_sensor.new({ do_pin = pin_from_board_json })

-- Read the raw DO level: 0 = bright, 1 = dark.
local level = sensor:read()

-- Convenience predicates (return boolean).
local bright = sensor:is_bright()   -- true when DO == 0
local dark   = sensor:is_dark()     -- true when DO == 1

-- Sensor type string.
local name = sensor:name()   -- always "lm393_ldr"

-- Close the handle (no persistent resources; safe to call multiple times).
sensor:close()
```

## Minimal example

```lua
local light = require("light_sensor")
local file  = require("file")
local cjson = require("cjson")
local sys   = require("sys")

-- Read pin from board.json device entry (no hardcoded pin names)
local function board_pin()
    local ok, s = pcall(file.read, "board.json")
    if not (ok and s) then return nil end
    local ok2, cfg = pcall(cjson.decode, s)
    if not (ok2 and cfg and cfg.devices) then return nil end
    for _, d in ipairs(cfg.devices) do
        if d.id == "light_sensor" and d.params and d.params.pin then
            return d.params.pin
        end
    end
    return nil
end

local pin = board_pin()
if not pin then error("light_sensor device not found in board.json") end

local s = light.new({ do_pin = pin })
for _ = 1, 5 do
    local level = s:read()
    print(level == 0 and "bright" or "dark")
    sys.sleep_ms(500)
end
s:close()
```

## AT command (test)

```
AT+CLAW=light_sensor           -- pin from board.json "light_sensor" device
AT+CLAW=light_sensor,PA_26     -- explicit pin override
```

## Concurrency & resources

- **No persistent hardware resources** between reads. `close()` marks the
  handle invalid; the GPIO clock stays on (RCC is always-on after first use).
- **Single module-level mutex** serialises all `read*()` calls across Lua tasks.
- **No IRQ disabled** — GPIO level reads are non-critical and do not require
  disabling interrupts.
- **Not safe to share a handle across tasks**: give each task its own handle.
  Handles are cheap (8 bytes each).

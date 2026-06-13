# Lua PWM

This module provides PWM output via `require("pwm")` for Ameba RTOS.

## How to call

- Import with `local pwm = require("pwm")`
- Create a handle with `local h = pwm.new({pin="PA_6", timer_idx=4, channel=0, frequency_hz=1000, duty_percent=50})`
- Start output with `h:start()` or `h:set_enabled(true)`
- Change duty cycle with `h:set_duty(percent)`
- Change frequency with `h:set_frequency(hz)`
- Query channel count with `h:get_channel_count()` — always 1
- Stop output with `h:stop()` or `h:set_enabled(false)`
- Release resources with `h:close()`

## Config table

| Field | Required | Default | Description |
|-------|----------|---------|-------------|
| `pin` | Yes | — | GPIO pin name, e.g. `"PA_6"` |
| `timer_idx` | Yes | — | PWM timer index: `4`, `5`, `6`, or `7` |
| `channel` | Yes | — | Timer channel: `0`, `1`, `2`, or `3` |
| `frequency_hz` | No | `1000` | PWM frequency in Hz |
| `duty_percent` | No | `50` | Initial duty cycle 0–100 |

## Hardware notes (RTL8721F)

The following pins support PWM output. The user must ensure the pin matches the
selected timer and channel.

| Timer | CH0 | CH1 | CH2 | CH3 |
|-------|-----|-----|-----|-----|
| TIM4  | PA_6  | PA_7  | PA_8  | PA_10 |

TIM5–TIM7 also support PWM on other pins; refer to the chip pinmux table.

## Example

```lua
local pwm = require("pwm")
local sys  = require("sys")

local h = pwm.new({
    pin          = "PA_6",
    timer_idx    = 4,
    channel      = 0,
    frequency_hz = 1000,
    duty_percent = 50,
})

h:start()
sys.sleep_ms(500)

h:set_duty(25)
sys.sleep_ms(500)

h:set_frequency(2000)
h:set_duty(75)
sys.sleep_ms(500)

h:stop()
h:close()
```

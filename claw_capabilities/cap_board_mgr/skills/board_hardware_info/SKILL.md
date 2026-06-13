---
name: board_hardware_info
description: "Inspect this board's hardware: devices, interfaces, peripherals and free pins. ACTIVATE THIS FIRST before writing or running any hardware/GPIO/I2C/SPI/sensor script."
compatibility: RTL8721F
metadata:
  cap_groups: board
  manage_mode: readonly
  category: hardware
---
# board_hardware_info

Hardware gate for this device. Activating this skill makes the board-inspection
tools visible. Always consult them before writing or running any script that
touches hardware (GPIO, I2C, SPI, UART, ADC, PWM, audio, RTC, sensors) so you
never assume a peripheral or pin that this board does not have.

## How to query the hardware

After activating this skill, these tools become callable (they are hidden until
activation — that is the hardware gate):

- `board_list_devices()` — list every device on the board (id, name, type, chip,
  interface id). Start here to see what is physically present (e.g. an OLED, a
  sensor).
- `board_get_device(id)` — full detail for one device: chip, the interface pin
  assignments (sda/scl/etc.), driver params (address, resolution, offsets),
  notes and a usage_guide. Call this before writing driver/Lua code for a device.
- `board_query_peripheral(peripheral)` — ask whether a peripheral type is
  supported and which pins/instances are free. Pass a type
  (`i2c`/`spi`/`uart`/`adc`/`pwm`/`gpio`/`ir`/`audio`/`rtc`) or an instance name
  (`SPI0`, `UART1`, `TIM4`). Returns `supported`, per-instance status
  (free/occupied + pins) and `available_pins`. Returns `{"supported":false}`
  when the chip does not support it.

## Workflow before a hardware script

1. Activate this skill.
2. `board_list_devices()` — is the device I need already wired? If yes,
   `board_get_device(id)` for its pins/params and use those exactly.
3. If you need a raw peripheral (not a pre-wired device), call
   `board_query_peripheral(type)` and pick a pin from `available_pins` only.
4. Only then write/run the Lua script (see `builtin_lua_modules` for module APIs).

Never invent a pin or claim a peripheral exists without confirming it here.

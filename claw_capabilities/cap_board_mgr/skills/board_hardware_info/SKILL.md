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
  **If the result contains `"lua_module": "<name>"`, you MUST call
  `file_read("rolfs:/docs/<name>.md")` immediately — that doc has the exact Lua API.
  Do NOT read `i2c.md` or `spi.md` first; read the module doc first.**
- `board_query_peripheral(peripheral)` — ask whether a peripheral type is
  supported and which pins/instances are free. Pass a type
  (`i2c`/`spi`/`uart`/`adc`/`pwm`/`gpio`/`ir`/`audio`/`rtc`) or an instance name
  (`SPI0`, `UART1`, `TIM4`). Returns `supported`, per-instance status
  (free/occupied + pins) and `available_pins`. Returns `{"supported":false}`
  when the chip does not support it.

## Workflow before a hardware script

1. Activate this skill.
2. **You MUST call `board_list_devices()` first — never guess or infer a
   device id from the board name, description, or prior knowledge.**
   The board may have multiple variants of the same peripheral type (e.g.
   two display options); only `board_list_devices()` tells you which are
   registered. After getting the list, call `board_get_device(id)` for the
   one you need.
   **If the result has `"lua_module": "<name>"`, immediately read
   `rolfs:/docs/<name>.md` — do this before reading any other doc (e.g. i2c.md).**
3. If you need a raw peripheral (not a pre-wired device), call
   `board_query_peripheral(type)` and pick a pin from `available_pins` only.
4. Only then write/run the Lua script. Use `require("<lua_module>")` — not raw I2C/SPI.
   (See `builtin_lua_modules` for the full module index.)

Never invent a pin or claim a peripheral exists without confirming it here.

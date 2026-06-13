# oled_sh1106.lua

Reusable pure-Lua SH1106 OLED driver for I2C-connected SH1106 panels.

The SH1106 is command-compatible with the SSD1306 but has a 132-column RAM
while only 128 columns are visible, so the visible window is centred starting
at column 2 (`col_offset`). It also supports page addressing only, so `show()`
re-programmes the page/column cursor before each page write. The API mirrors
`ssd1306.lua` so the two drivers are drop-in interchangeable.

## Require

```lua
local oled_sh1106 = require("oled_sh1106")
```

On Ameba this module is registered into `package.preload` by the test
provisioner before the script runs, so `require("oled_sh1106")` resolves
without a filesystem searcher.

## Dependencies

- `i2c` module
- An opened I2C device handle for the OLED address, usually `0x3C`

## Constructor

```lua
local oled = oled_sh1106.new(dev, opts)
```

`dev` must be an I2C device handle with a `write` method. On Ameba the
control byte (`0x00` for commands, `0x40` for data) is passed as the `mem`
argument of `dev:write(data, mem)`, which the driver prepends to the burst.

`opts`:
- `width`: `128` by default
- `height`: `64` by default (also supports `32`)
- `addr`: metadata only, `0x3C` by default
- `col_offset`: `2` by default (SH1106 132-column RAM offset)
- `contrast`: `0x66` by default
- `external_vcc`: `false` by default
- `segment_remap`: `true` by default
- `com_scan_dec`: `true` by default

Supported sizes are `128x64` and `128x32`.

## Methods

- `oled:init()`: run the power-on initialisation sequence.
- `oled:clear(color)`: clear the framebuffer to lit or unlit pixels.
- `oled:pixel(x, y, color)`: set one pixel.
- `oled:fill_rect(x, y, w, h, color)`: fill a rectangle.
- `oled:draw_char(x, y, ch, color, sx, sy)`: draw one 5x7 ASCII character.
  Each font pixel becomes an `sx`-by-`sy` block. Pass a single `sx` for
  uniform scaling (`sx=2` -> 10x14), or `sx, sy` for non-uniform scaling
  (`sx=1, sy=2` -> tall & narrow; `sx=2, sy=1` -> short & wide). Both default
  to `1`.
- `oled:draw_text(x, y, text, color, sx, sy)`: draw ASCII text (`\n` wraps to
  a new line). Advance is `6*sx` px per character, line height `8*sy` px.
- `oled:invert(enable)`: enable or disable display inversion.
- `oled:contrast(value)`: set contrast from `0` to `255`.
- `oled:show()`: flush the framebuffer to the panel.
- `oled:close()`: turn the panel off and mark the display closed.

`color` is truthy for lit pixels and `false` or `nil` for cleared pixels.

> Note: `close()` sends the display-off command (`0xAE`). If you want the
> rendered content to stay visible after your script ends, do not call
> `close()` — just release the I2C handles.

## Example

```lua
local i2c         = require("i2c")
local oled_sh1106 = require("oled_sh1106")

-- I2C0: new(idx, sda, scl) → SDA=PA_26, SCL=PA_25, address 0x3C
local bus = i2c.new(0, "PA_26", "PA_25", 400000)
local dev = bus:device(0x3C)

local oled = oled_sh1106.new(dev, { width = 128, height = 64 })
oled:init()
oled:clear(false)
oled:draw_text(2, 0,  "Hello,", true)
oled:draw_text(2, 16, "Ameba Claw", true)
oled:show()

-- Leave the panel on; just release the bus.
dev:close()
bus:close()
```

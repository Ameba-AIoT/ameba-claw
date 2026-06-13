-- SH1106 OLED display demo: "Hello, Ameba Claw"
-- I2C0 on PA_25 (SCL) / PA_26 (SDA), address 0x3C
--
-- The init sequence, framebuffer and font live in the shared driver
-- lib/oled_sh1106.lua so they are defined exactly once. This script only
-- wires up the I2C bus and draws the demo text.
local i2c         = require('i2c')
local oled_sh1106 = require('oled_sh1106')

local ADDR = 0x3C

-- SDA=PA_26, SCL=PA_25, 400 kHz
local bus = i2c.new(0, "PA_26", "PA_25", 400000)
local dev = bus:device(ADDR)

-- Font scale comes from the AT command: AT+CLAW=i2c,sh1106[,sx[,sy]].
-- The C runner injects SH1106_SX / SH1106_SY as globals (0 = unspecified).
--   none given     -> sx=1, sy=2 (default "medium" look)
--   only sx given  -> uniform (sy = sx)
--   both given     -> non-uniform sx, sy
local sx = tonumber(SH1106_SX) or 0
local sy = tonumber(SH1106_SY) or 0
if sx <= 0 and sy <= 0 then
    sx, sy = 1, 2
elseif sy <= 0 then
    sy = sx
elseif sx <= 0 then
    sx = sy
end

local oled = oled_sh1106.new(dev, { width = 128, height = 64, addr = ADDR })
oled:init()
oled:clear(false)
-- Stack two lines, spacing derived from the vertical scale.
local line_h = 8 * sy
oled:draw_text(4, 2, "Hello,", true, sx, sy)
oled:draw_text(4, 2 + line_h + 2, "Ameba Claw", true, sx, sy)
oled:show()

print(string.format("SH1106: scale sx=%d sy=%d", sx, sy))

-- Leave the panel on so the message stays visible; just release the bus.
dev:close()
bus:close()

print("SH1106: Hello, Ameba Claw")

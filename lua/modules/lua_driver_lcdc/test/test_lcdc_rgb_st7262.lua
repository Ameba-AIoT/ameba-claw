-- test_lcdc_rgb_st7262.lua
-- LCDC RGB interface test: st7262 800x480 panel / RTL8721F eval board
--
-- Trigger: AT+CLAW=lcdc,rgb,st7262
--
-- Precondition (provided by lua_lcdc_test_provision.c):
--   _lcdc_fb_addr  global integer, C code carves an RGB565 framebuffer from
--                  PSRAM top and injects it via lua_setglobal().
--
-- Requires: lcdc, gpio, sys

local lcdc = require('lcdc')
local gpio = require('gpio')
local sys  = require('sys')

-- ----------------------------------------------------------------
-- Board configuration: st7262 / RTL8721F eval board
-- ----------------------------------------------------------------

local fb_addr = _lcdc_fb_addr

local PIN_BLEN = 0x23   -- PB_3  backlight, active high
local PIN_DISP = 0x11   -- PA_17 display-on, active high

local PANEL_CFG = {
    fb_addr      = fb_addr,
    width        = 800,
    height       = 480,
    vsw          = 1,
    vbp          = 4,
    vfp          = 6,
    hsw          = 4,
    hbp          = 40,
    hfp          = 40,
    refresh_freq = 40,
    input_fmt    = 'rgb565',
    output_fmt   = 'bgr888',
    if_width     = '24bit',
}

local PINMUX_CFG = {
    d0  = 0x2F,  d1  = 0x31,  d2  = 0x35,  d3  = 0x32,  -- PB15,PB17,PB21,PB18
    d4  = 0x06,  d5  = 0x08,  d6  = 0x07,  d7  = 0x0A,  -- PA6, PA8, PA7, PA10
    d8  = 0x29,  d9  = 0x2B,  d10 = 0x2A,  d11 = 0x30,  -- PB9, PB11,PB10,PB16
    d12 = 0x36,  d13 = 0x37,  d14 = 0x2E,  d15 = 0x2C,  -- PB22,PB23,PB14,PB12
    d16 = 0x16,  d17 = 0x19,  d18 = 0x1D,  d19 = 0x24,  -- PA22,PA25,PA29,PB4
    d20 = 0x25,  d21 = 0x26,  d22 = 0x27,  d23 = 0x28,  -- PB5, PB6, PB7, PB8
    hsync = 0x10,  vsync = 0x0D,  dclk = 0x09,  de = 0x0E,
}

-- ----------------------------------------------------------------
-- Helpers
-- ----------------------------------------------------------------

local W, H = 800, 480

-- Flush framebuffer to screen and hold for ms milliseconds
local function show(label, ms)
    lcdc.update()
    print(label)
    sys.sleep_ms(ms)
end

-- Color palette (RGB888)
local R   = {255,   0,   0}
local G   = {  0, 255,   0}
local B   = {  0,   0, 255}
local C   = {  0, 255, 255}
local M   = {255,   0, 255}
local Y   = {255, 255,   0}
local WHT = {255, 255, 255}
local BLK = {  0,   0,   0}

-- ----------------------------------------------------------------
-- Hardware init
-- ----------------------------------------------------------------

print(string.format('[lcdc rgb] fb_addr = 0x%08X', fb_addr))

gpio.set_direction(PIN_BLEN, 'output') gpio.set_level(PIN_BLEN, 1)
gpio.set_direction(PIN_DISP, 'output') gpio.set_level(PIN_DISP, 1)

lcdc.pinmux(PINMUX_CFG)

print('[lcdc rgb] init st7262 800x480')
local ok, err = pcall(function()
    if not lcdc.rgb_init(PANEL_CFG) then error('rgb_init returned false') end
end)
if not ok then
    print('[lcdc rgb] init failed: ' .. tostring(err))
    gpio.set_level(PIN_BLEN, 0) gpio.set_level(PIN_DISP, 0)
    return
end

-- ----------------------------------------------------------------
-- Pattern 1: solid colours (3 s each)
-- ----------------------------------------------------------------

fill_color(255, 0, 0)   show('[rgb] p1 solid red',   3000)
fill_color(0, 255, 0)   show('[rgb] p1 solid green', 3000)
fill_color(0, 0, 255)   show('[rgb] p1 solid blue',  3000)

-- ----------------------------------------------------------------
-- Pattern 2: horizontal stripes (8 bands × 60 px, 3 s)
-- R / white / G / white / B / white / Y / white
-- ----------------------------------------------------------------

local hpal = {R, WHT, G, WHT, B, WHT, Y, WHT}
for i, c in ipairs(hpal) do
    fill_rect(0, (i-1)*60, W, 60, c[1], c[2], c[3])
end
show('[rgb] p2 horizontal stripes (8x60px)', 3000)

-- ----------------------------------------------------------------
-- Pattern 3: vertical stripes (8 bands × 100 px, 3 s)
-- R / G / B / C / M / Y / white / black
-- ----------------------------------------------------------------

local vpal = {R, G, B, C, M, Y, WHT, BLK}
for i, c in ipairs(vpal) do
    fill_rect((i-1)*100, 0, 100, H, c[1], c[2], c[3])
end
show('[rgb] p3 vertical stripes (8x100px)', 3000)

-- ----------------------------------------------------------------
-- Pattern 4: checkerboard (8 cols × 8 rows, 100×60 px cells, 3 s)
-- ----------------------------------------------------------------

fill_color(0, 0, 0)
for row = 0, 7 do
    for col = 0, 7 do
        if (row + col) % 2 == 0 then
            fill_rect(col*100, row*60, 100, 60, 255, 255, 255)
        end
    end
end
show('[rgb] p4 checkerboard (8x8 cells)', 3000)

-- ----------------------------------------------------------------
-- Pattern 5: SMPTE colour bars (8 vertical bars × 100 px, 3 s)
-- white / yellow / cyan / green / magenta / red / blue / black
-- ----------------------------------------------------------------

local smpte = {WHT, Y, C, G, M, R, B, BLK}
for i, c in ipairs(smpte) do
    fill_rect((i-1)*100, 0, 100, H, c[1], c[2], c[3])
end
show('[rgb] p5 SMPTE colour bars', 3000)

-- ----------------------------------------------------------------
-- Pattern 6: crosshatch grid (black bg + white lines, 3 s)
-- horizontal lines every 60 px, vertical lines every 100 px
-- ----------------------------------------------------------------

fill_color(0, 0, 0)
for y = 60, 420, 60 do
    fill_rect(0, y, W, 2, 255, 255, 255)
end
for x = 100, 700, 100 do
    fill_rect(x, 0, 2, H, 255, 255, 255)
end
show('[rgb] p6 crosshatch grid', 3000)

-- ----------------------------------------------------------------
-- Status and teardown
-- ----------------------------------------------------------------

local sync = lcdc.rgb_get_sync_status()
print(string.format('[lcdc rgb] sync hs=%d vs=%d', sync.hs, sync.vs))

local info = lcdc.get_info()
print(string.format('[lcdc rgb] w=%d h=%d init=%s fb=0x%08X',
    info.width, info.height, tostring(info.initialized), info.fb_addr))

lcdc.deinit()
gpio.set_level(PIN_BLEN, 0) gpio.set_level(PIN_DISP, 0)

print('success')

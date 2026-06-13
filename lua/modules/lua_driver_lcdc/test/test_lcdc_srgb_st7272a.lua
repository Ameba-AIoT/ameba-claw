-- test_lcdc_srgb_st7272a.lua
-- LCDC SRGB (8-bit serial RGB) interface test: ST7272A 320x240 panel
--
-- Trigger: AT+CLAW=lcdc,srgb,st7272a
--
-- Precondition (provided by lua_lcdc_test_provision.c):
--   _lcdc_fb_addr  global integer, C code carves an RGB888 framebuffer from
--                  PSRAM top and injects it via lua_setglobal().
--
-- Pin reference: example/peripheral/raw/Display/LCDC/raw_lcdc_srgb_st7272a/
--   example_raw_lcdc_srgb_st7272a.c
--
-- Requires: lcdc, gpio, sys

local lcdc = require('lcdc')
local gpio = require('gpio')
local sys  = require('sys')

-- ----------------------------------------------------------------
-- Board configuration: pins from example_raw_lcdc_srgb_st7272a.c
-- ----------------------------------------------------------------

local fb_addr = _lcdc_fb_addr

local PIN_BLEN  = 0x23   -- PB_3  backlight, active high
local PIN_RESET = 0x36   -- PB_22 reset, active high

local PINMUX_CFG = {
    d0=0x06, d1=0x08, d2=0x07, d3=0x0A,   -- PA6,PA8,PA7,PA10
    d4=0x0B, d5=0x09, d6=0x11, d7=0x10,   -- PA11,PA9,PA17,PA16
    hsync=0x35, vsync=0x31, dclk=0x2E, de=0x0E,
}

local PANEL_CFG = {
    fb_addr=fb_addr, width=320, height=240,
    if_width='8bit', input_fmt='rgb888', output_fmt='bgr888',
    vsw=4, vbp=8, vfp=8, hsw=4, hbp=39, hfp=8, refresh_freq=35,
    en_pol=1, hs_pol=0, vs_pol=0, dclk_edge=1,
}

-- ----------------------------------------------------------------
-- Helpers
-- ----------------------------------------------------------------

local W, H = 320, 240

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

print(string.format('[lcdc srgb] fb_addr = 0x%08X', fb_addr))

gpio.set_direction(PIN_RESET, 'output')
gpio.set_level(PIN_RESET, 1)
sys.sleep_ms(100)

gpio.set_direction(PIN_BLEN, 'output')
gpio.set_level(PIN_BLEN, 1)

lcdc.pinmux(PINMUX_CFG)

print('[lcdc srgb] init (8-bit, 320x240, 35Hz)')
local ok, err = pcall(function()
    if not lcdc.rgb_init(PANEL_CFG) then error('rgb_init returned false') end
end)
if not ok then
    print('[lcdc srgb] init failed: ' .. tostring(err))
    gpio.set_level(PIN_BLEN, 0)
    return
end

-- ----------------------------------------------------------------
-- Pattern 1: solid colours (3 s each)
-- ----------------------------------------------------------------

fill_color(255, 0, 0)   show('[srgb] p1 solid red',   3000)
fill_color(0, 255, 0)   show('[srgb] p1 solid green', 3000)
fill_color(0, 0, 255)   show('[srgb] p1 solid blue',  3000)

-- ----------------------------------------------------------------
-- Pattern 2: horizontal stripes (8 bands × 30 px, 3 s)
-- R / white / G / white / B / white / Y / white
-- ----------------------------------------------------------------

local hpal = {R, WHT, G, WHT, B, WHT, Y, WHT}
for i, c in ipairs(hpal) do
    fill_rect(0, (i-1)*30, W, 30, c[1], c[2], c[3])
end
show('[srgb] p2 horizontal stripes (8x30px)', 3000)

-- ----------------------------------------------------------------
-- Pattern 3: vertical stripes (8 bands × 40 px, 3 s)
-- R / G / B / C / M / Y / white / black
-- ----------------------------------------------------------------

local vpal = {R, G, B, C, M, Y, WHT, BLK}
for i, c in ipairs(vpal) do
    fill_rect((i-1)*40, 0, 40, H, c[1], c[2], c[3])
end
show('[srgb] p3 vertical stripes (8x40px)', 3000)

-- ----------------------------------------------------------------
-- Pattern 4: checkerboard (8 cols × 8 rows, 40×30 px cells, 3 s)
-- ----------------------------------------------------------------

fill_color(0, 0, 0)
for row = 0, 7 do
    for col = 0, 7 do
        if (row + col) % 2 == 0 then
            fill_rect(col*40, row*30, 40, 30, 255, 255, 255)
        end
    end
end
show('[srgb] p4 checkerboard (8x8 cells)', 3000)

-- ----------------------------------------------------------------
-- Pattern 5: SMPTE colour bars (8 vertical bars × 40 px, 3 s)
-- white / yellow / cyan / green / magenta / red / blue / black
-- ----------------------------------------------------------------

local smpte = {WHT, Y, C, G, M, R, B, BLK}
for i, c in ipairs(smpte) do
    fill_rect((i-1)*40, 0, 40, H, c[1], c[2], c[3])
end
show('[srgb] p5 SMPTE colour bars', 3000)

-- ----------------------------------------------------------------
-- Pattern 6: crosshatch grid (black bg + white lines, 3 s)
-- horizontal lines every 30 px, vertical lines every 40 px
-- ----------------------------------------------------------------

fill_color(0, 0, 0)
for y = 30, 210, 30 do
    fill_rect(0, y, W, 2, 255, 255, 255)
end
for x = 40, 280, 40 do
    fill_rect(x, 0, 2, H, 255, 255, 255)
end
show('[srgb] p6 crosshatch grid', 3000)

-- ----------------------------------------------------------------
-- Status and teardown
-- ----------------------------------------------------------------

local sync = lcdc.rgb_get_sync_status()
print(string.format('[lcdc srgb] sync hs=%d vs=%d', sync.hs, sync.vs))

local info = lcdc.get_info()
print(string.format('[lcdc srgb] w=%d h=%d init=%s fb=0x%08X',
    info.width, info.height, tostring(info.initialized), info.fb_addr))

lcdc.deinit()
gpio.set_level(PIN_BLEN, 0)

print('success')

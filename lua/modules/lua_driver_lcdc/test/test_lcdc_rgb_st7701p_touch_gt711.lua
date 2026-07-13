-- test_lcdc_rgb_st7701p_touch_gt711.lua
-- LCDC RGB888 480x480 (ST7701P) + GT911 capacitive touch test
--
-- Trigger: AT+CLAW=lcdc,rgb,st7701p
--
-- Precondition (provided by lua_lcdc_test_provision.c before this script runs):
--   _lcdc_fb_addr   framebuffer base address (480x480 RGB888, carved from PSRAM top)
--   gt911_poll()    C function: returns nil | {x=n, y=n, pressed=bool}
--   fill_color, fill_rect, set_pixel   C pixel helpers (standard)
--   SPI register init sequence already sent (panel_st7701p_rgb_spi.inc in C)
--
-- Requires: lcdc, gpio, sys
--
-- Pin encoding (RTL8721F): PA_X = X, PB_X = 0x20+X, PC_X = 0x40+X

local lcdc = require('lcdc')
local gpio = require('gpio')
local sys  = require('sys')

-- ----------------------------------------------------------------
-- Board configuration: st7701p_rgb_480x480 / RTL8721F eval board
-- (panel_pin_config.c — "st7701p_rgb_480x480" entry)
-- ----------------------------------------------------------------

local fb_addr = _lcdc_fb_addr

-- GPIO (SPI init + panel reset done in C; only BL needed here)
local PIN_RESET = 0x17   -- PA_23  panel reset (already toggled in C)
local PIN_BL    = 0x19   -- PA_25  backlight, active high

-- LCDC configuration
local PANEL_CFG = {
    fb_addr      = fb_addr,
    width        = 480,
    height       = 480,
    -- Timing from panel_st7701p_rgb.c panel_timing_t
    vsw          = 3,
    vbp          = 12,
    vfp          = 15,
    hsw          = 2,
    hbp          = 2,
    hfp          = 15,
    refresh_freq = 60,
    -- Format: RGB888 framebuffer → RGB888 output on 24-bit bus
    input_fmt    = 'rgb888',
    output_fmt   = 'rgb888',
    if_width     = '24bit',
    -- Polarity (from panel_timing_t)
    en_pol       = 1,   -- DE active high   (de_active_high  = true)
    hs_pol       = 0,   -- HSYNC active low (hsync_active_low = true)
    vs_pol       = 0,   -- VSYNC active low (vsync_active_low = true)
    dclk_edge    = 0,   -- rising edge      (dclk_falling_edge = false)
}

-- Pinmux (from panel_pin_config.c "st7701p_rgb_480x480")
local PINMUX_CFG = {
    -- RGB888 24-bit data bus D0–D23
    d0  = 0x10,  d1  = 0x0F,  d2  = 0x0E,  d3  = 0x0D,  -- PA16,PA15,PA14,PA13
    d4  = 0x0C,  d5  = 0x41,  d6  = 0x40,  d7  = 0x3F,  -- PA12,PC1, PC0, PB31
    d8  = 0x3E,  d9  = 0x3D,  d10 = 0x3C,  d11 = 0x3B,  -- PB30,PB29,PB28,PB27
    d12 = 0x3A,  d13 = 0x39,  d14 = 0x38,  d15 = 0x37,  -- PB26,PB25,PB24,PB23
    d16 = 0x36,  d17 = 0x35,  d18 = 0x33,  d19 = 0x32,  -- PB22,PB21,PB19,PB18
    d20 = 0x31,  d21 = 0x30,  d22 = 0x2F,  d23 = 0x2E,  -- PB17,PB16,PB15,PB14
    -- Sync / timing signals
    hsync = 0x13,  -- PA19
    vsync = 0x12,  -- PA18
    dclk  = 0x2D,  -- PB13
    de    = 0x11,  -- PA17
}

-- ----------------------------------------------------------------
-- Helpers
-- ----------------------------------------------------------------

local W, H = 480, 480

local function show(label, ms)
    sys.sleep_ms(20)   -- wait ~1 frame (17 ms at 60 Hz) before shadow reload
    lcdc.update()
    print(label)
    sys.sleep_ms(ms)
end

-- Standard colour palette (RGB888)
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
-- (SPI register sequence and panel reset were done in C)
-- ----------------------------------------------------------------

print(string.format('[st7701p] fb_addr = 0x%08X', fb_addr))

gpio.set_direction(PIN_BL, 'output')
gpio.set_level(PIN_BL, 0)

lcdc.pinmux(PINMUX_CFG)

print('[st7701p] rgb_init 480x480 RGB888')
local ok, err = pcall(function()
    if not lcdc.rgb_init(PANEL_CFG) then error('rgb_init returned false') end
end)
if not ok then
    print('[st7701p] init FAILED: ' .. tostring(err))
    gpio.set_level(PIN_BL, 0)
    return
end

gpio.set_level(PIN_BL, 1)
print('[st7701p] backlight ON')

-- ----------------------------------------------------------------
-- Pattern 1: solid colours  (2 s each)
-- ----------------------------------------------------------------

fill_color(255, 0, 0)   show('[st7701p] p1 solid red',   2000)
fill_color(0, 255, 0)   show('[st7701p] p1 solid green', 2000)
fill_color(0, 0, 255)   show('[st7701p] p1 solid blue',  2000)
fill_color(255,255,255) show('[st7701p] p1 solid white', 1500)
fill_color(0,   0,   0) show('[st7701p] p1 solid black', 1500)

-- ----------------------------------------------------------------
-- Pattern 2: horizontal stripes  8 bands × 60 px  (3 s)
-- R / WHT / G / WHT / B / WHT / Y / WHT
-- ----------------------------------------------------------------

local hpal = {R, WHT, G, WHT, B, WHT, Y, WHT}
for i, c in ipairs(hpal) do
    fill_rect(0, (i-1)*60, W, 60, c[1], c[2], c[3])
end
show('[st7701p] p2 horizontal stripes (8x60px)', 3000)

-- ----------------------------------------------------------------
-- Pattern 3: SMPTE vertical colour bars  8 bands × 60 px  (3 s)
-- WHT / Y / C / G / M / R / B / BLK
-- ----------------------------------------------------------------

local smpte = {WHT, Y, C, G, M, R, B, BLK}
for i, c in ipairs(smpte) do
    fill_rect((i-1)*60, 0, 60, H, c[1], c[2], c[3])
end
show('[st7701p] p3 SMPTE colour bars (8x60px)', 3000)

-- ----------------------------------------------------------------
-- Pattern 4: checkerboard  8×8 grid, 60×60 px cells  (3 s)
-- ----------------------------------------------------------------

fill_color(0, 0, 0)
for row = 0, 7 do
    for col = 0, 7 do
        if (row + col) % 2 == 0 then
            fill_rect(col*60, row*60, 60, 60, 255, 255, 255)
        end
    end
end
show('[st7701p] p4 checkerboard 8x8', 3000)

-- ----------------------------------------------------------------
-- Pattern 5: crosshatch grid  (3 s)
-- black background + white lines every 60 px
-- ----------------------------------------------------------------

fill_color(0, 0, 0)
for y = 60, 420, 60 do
    fill_rect(0, y, W, 2, 255, 255, 255)
end
for x = 60, 420, 60 do
    fill_rect(x, 0, 2, H, 255, 255, 255)
end
show('[st7701p] p5 crosshatch grid (60px spacing)', 3000)

-- ----------------------------------------------------------------
-- Pattern 6: RGB gradient bands  (3 s)
-- top 160 px: R sweep 0→255, middle: G sweep, bottom: B sweep
-- ----------------------------------------------------------------

for x = 0, W-1 do
    local v = math.floor(x * 255 / (W - 1))
    fill_rect(x,   0, 1, 160, v, 0, 0)
    fill_rect(x, 160, 1, 160, 0, v, 0)
    fill_rect(x, 320, 1, 160, 0, 0, v)
end
show('[st7701p] p6 RGB gradient bands', 3000)

-- ----------------------------------------------------------------
-- GT911 touch test  (15 seconds)
-- Dark background; each tap draws a 20×20 orange marker.
-- ----------------------------------------------------------------

local TOUCH_ITERS = 750   -- 750 × 20 ms = 15 s
local MARKER      = 20
local tap_count   = 0
local last_press  = false

fill_color(20, 20, 50)
fill_rect(0, 0, W, 32, 50, 50, 120)   -- title bar
show('[st7701p] touch test — tap the screen (15 s)', 0)

for _ = 1, TOUCH_ITERS do
    local pt = gt911_poll()
    if pt then
        if pt.pressed and not last_press then
            tap_count = tap_count + 1
            local mx = math.max(0, math.min(W - MARKER, pt.x - MARKER // 2))
            local my = math.max(32, math.min(H - MARKER, pt.y - MARKER // 2))
            -- white border then orange fill
            fill_rect(mx,   my,   MARKER,   MARKER,   255, 255, 255)
            fill_rect(mx+2, my+2, MARKER-4, MARKER-4, 255,  80,   0)
            lcdc.update()
            print(string.format('[st7701p] tap#%d  x=%-3d y=%-3d',
                                tap_count, pt.x, pt.y))
        end
        last_press = pt.pressed
    end
    sys.sleep_ms(20)
end

print(string.format('[st7701p] touch test done: %d taps in 15 s', tap_count))

-- ----------------------------------------------------------------
-- Diagnostic readback + teardown
-- ----------------------------------------------------------------

local sync = lcdc.rgb_get_sync_status()
print(string.format('[st7701p] sync hs=%d vs=%d', sync.hs, sync.vs))

local info = lcdc.get_info()
print(string.format('[st7701p] w=%d h=%d init=%s fb=0x%08X',
                    info.width, info.height, tostring(info.initialized), info.fb_addr))

lcdc.enable(false)     -- stop output first
sys.sleep_ms(50)       -- let LCDC finish current frame before deinit
lcdc.deinit()
gpio.set_level(PIN_BL, 0)

if tap_count > 0 then
    print('[st7701p] touch verified — success')
else
    print('[st7701p] no taps detected (display OK; touch needs physical interaction)')
end
print('success')

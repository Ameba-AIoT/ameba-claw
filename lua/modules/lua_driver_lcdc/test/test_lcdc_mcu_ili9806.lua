-- test_lcdc_mcu_ili9806.lua
-- LCDC MCU (8080 parallel) interface test: ILI9806 480x800 panel
--
-- Trigger: AT+CLAW=lcdc,mcu,ili9806
--
-- Precondition (provided by lua_lcdc_test_provision.c):
--   _lcdc_fb_addr  global integer, C code carves an RGB888 framebuffer from
--                  PSRAM top and injects it via lua_setglobal().
--
-- Pin reference: example/peripheral/raw/Display/LCDC/raw_lcdc_mcu_ili9806/
--   example_raw_lcdc_mcu_ili9806.c  bsp_mcu_com.h  bsp_mcu_ili9806.h
--
-- Requires: lcdc, gpio, sys

local lcdc = require('lcdc')
local gpio = require('gpio')
local sys  = require('sys')

-- ----------------------------------------------------------------
-- Board configuration: pins from example_raw_lcdc_mcu_ili9806.c
--                      and bsp_mcu_ili9806.h
-- ----------------------------------------------------------------

local fb_addr = _lcdc_fb_addr

-- Backlight (LCD_BLEN_MCU = PB_3 = 0x23), active high
local PIN_BLEN = 0x23

-- Panel reset (MCU_RESET_PIN = PA_8 = 0x08), active low
local PIN_RST  = 0x08

-- Interface mode select (IM[3:0] = 0010 -> 24-bit parallel)
-- MCU_IM3_PIN=PA_14=0x0E  MCU_IM2_PIN=PA_13=0x0D
-- MCU_IM1_PIN=PA_16=0x10  MCU_IM0_PIN=PA_17=0x11
local PIN_IM3  = 0x0E   -- PA_14
local PIN_IM2  = 0x0D   -- PA_13
local PIN_IM1  = 0x10   -- PA_16
local PIN_IM0  = 0x11   -- PA_17

-- LCDC MCU signal pins (24-bit data bus + control)
-- Source: lcdc_pinmux_config() in example_raw_lcdc_mcu_ili9806.c
local PINMUX_CFG = {
    -- Data bus D0-D23
    d0  = 0x1A,  d1  = 0x18,  d2  = 0x17,  d3  = 0x16,  -- PA26,PA24,PA23,PA22
    d4  = 0x19,  d5  = 0x1D,  d6  = 0x24,  d7  = 0x25,  -- PA25,PA29,PB4, PB5
    d8  = 0x26,  d9  = 0x27,  d10 = 0x28,  d11 = 0x29,  -- PB6, PB7, PB8, PB9
    d12 = 0x2B,  d13 = 0x2A,  d14 = 0x30,  d15 = 0x36,  -- PB11,PB10,PB16,PB22
    d16 = 0x37,  d17 = 0x2E,  d18 = 0x2C,  d19 = 0x2F,  -- PB23,PB14,PB12,PB15
    d20 = 0x31,  d21 = 0x35,  d22 = 0x32,  d23 = 0x06,  -- PB17,PB21,PB18,PA6
    -- Control signals
    dcx = 0x09,  -- PA_9  (LCD_MCU_DCX)
    wr  = 0x0B,  -- PA_11 (LCD_MCU_WR)
    rd  = 0x0A,  -- PA_10 (LCD_MCU_RD)
    cs  = 0x07,  -- PA_7  (LCD_MCU_CSX)
}

-- ILI9806 LCDC init params (24-bit parallel, RGB888)
local PANEL_CFG = {
    width      = 480,
    height     = 800,
    if_width   = '24bit',
    input_fmt  = 'rgb888',
    output_fmt = 'rgb888',
    wrpulw     = 1,
    wr_pol     = 0,  -- LCDC_MCU_WR_PUL_RISING_EDGE_FETCH
    rd_pol     = 0,  -- LCDC_MCU_RD_PUL_RISING_EDGE_FETCH
    rs_pol     = 0,  -- LCDC_MCU_RS_PUL_LOW_LEV_CMD_ADDR
    dma_burst  = 2,  -- LCDC_DMA_BURSTSIZE_4X64BYTES
}

-- ----------------------------------------------------------------
-- ILI9806 init sequence
-- Source: ILI9806_Init() in bsp_mcu_ili9806.c
-- ----------------------------------------------------------------

local function wc(cmd)  lcdc.mcu_io_write_cmd(cmd)  end
local function wd(data) lcdc.mcu_io_write_data(data) end

local function ili9806_init()
    wc(0xFF) wd(0xFF) wd(0x98) wd(0x06)   -- EXTC: unlock extended command set

    -- GIP 1
    wc(0xBC)
    wd(0x01) wd(0x0F) wd(0x61) wd(0xFF) wd(0x01) wd(0x01) wd(0x0B) wd(0x10)
    wd(0x37) wd(0x63) wd(0xFF) wd(0xFF) wd(0x01) wd(0x01) wd(0x00) wd(0x00)
    wd(0xFF) wd(0x52) wd(0x01) wd(0x00) wd(0x40)

    -- GIP 2
    wc(0xBD)
    wd(0x01) wd(0x23) wd(0x45) wd(0x67) wd(0x01) wd(0x23) wd(0x45) wd(0x67)

    -- GIP 3
    wc(0xBE)
    wd(0x00) wd(0x01) wd(0xAB) wd(0x60) wd(0x22) wd(0x22) wd(0x22) wd(0x22) wd(0x22)

    wc(0xC7) wd(0x36)                      -- VCOM control
    wc(0xED) wd(0x7F) wd(0x0F)             -- EN_volt_reg
    wc(0xC0) wd(0x0F) wd(0x0B) wd(0x0A)   -- Power control 1
    wc(0xFC) wd(0x08)                      -- AVDD internal pumping
    wc(0xDF) wd(0x00) wd(0x00) wd(0x00) wd(0x00) wd(0x00) wd(0x20)
    wc(0xF3) wd(0x74)                      -- DVDD voltage
    wc(0xB4) wd(0x00) wd(0x00) wd(0x00)   -- Inversion type
    wc(0xF7) wd(0x82)                      -- Resolution control: 480x800
    wc(0xB1) wd(0x00) wd(0x13) wd(0x13)   -- Frame rate
    wc(0xF2) wd(0x80) wd(0x04) wd(0x40) wd(0x28)
    wc(0xC1) wd(0x17) wd(0x88) wd(0x88) wd(0x20)  -- Power control 2

    -- Positive gamma
    wc(0xE0)
    wd(0x00) wd(0x0A) wd(0x12) wd(0x10) wd(0x0E) wd(0x20) wd(0xCC) wd(0x07)
    wd(0x06) wd(0x0B) wd(0x0E) wd(0x0F) wd(0x0D) wd(0x15) wd(0x10) wd(0x00)

    -- Negative gamma
    wc(0xE1)
    wd(0x00) wd(0x0B) wd(0x13) wd(0x0D) wd(0x0E) wd(0x1B) wd(0x71) wd(0x06)
    wd(0x06) wd(0x0A) wd(0x0F) wd(0x0E) wd(0x0F) wd(0x15) wd(0x0C) wd(0x00)

    wc(0x2A) wd(0x00) wd(0x00) wd(0x01) wd(0xDF)  -- Column address set (0-479)
    wc(0x2B) wd(0x00) wd(0x00) wd(0x03) wd(0x1F)  -- Row address set (0-799)
    wc(0x3A) wd(0x77)                              -- Pixel format: 24bpp RGB888
    wc(0x36) wd(0x00)                              -- Memory access control

    wc(0x11)                                       -- Sleep out
    sys.sleep_ms(120)
    wc(0x29)                                       -- Display on
    sys.sleep_ms(20)
    wc(0x2C)                                       -- Memory write
end

-- ----------------------------------------------------------------
-- Test body
-- ----------------------------------------------------------------

local W, H = 480, 800

-- Flush framebuffer via DMA and hold for ms milliseconds
local function show(label, ms)
    lcdc.mcu_dma_trigger()
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

print(string.format('[lcdc mcu] fb_addr = 0x%08X', fb_addr))
print('[lcdc mcu] configuring pins (ILI9806 / example_raw_lcdc_mcu_ili9806)')

-- Backlight on: PB_3
gpio.set_direction(PIN_BLEN, 'output')
gpio.set_level(PIN_BLEN, 1)

-- Reset: PA_8, high -> low(10ms) -> high, wait 120ms
gpio.set_direction(PIN_RST, 'output')
gpio.set_level(PIN_RST, 1)
sys.sleep_ms(2)
gpio.set_level(PIN_RST, 0)
sys.sleep_ms(10)
gpio.set_level(PIN_RST, 1)
sys.sleep_ms(120)

-- IM[3:0] = 0010 -> 24-bit parallel mode
gpio.set_direction(PIN_IM3, 'output') gpio.set_level(PIN_IM3, 0)
gpio.set_direction(PIN_IM2, 'output') gpio.set_level(PIN_IM2, 0)
gpio.set_direction(PIN_IM1, 'output') gpio.set_level(PIN_IM1, 1)
gpio.set_direction(PIN_IM0, 'output') gpio.set_level(PIN_IM0, 0)
sys.sleep_ms(50)

-- Configure LCDC signal pinmux
lcdc.pinmux(PINMUX_CFG)

-- Init LCDC MCU mode
print('[lcdc mcu] init LCDC MCU mode')
local ok, err = pcall(function()
    local ret = lcdc.mcu_init(PANEL_CFG)
    if not ret then error('mcu_init returned false') end
end)
if not ok then
    print('[lcdc mcu] mcu_init failed: ' .. tostring(err))
    gpio.set_level(PIN_BLEN, 0)
    return
end

-- Send ILI9806 init sequence via IO mode
print('[lcdc mcu] sending ILI9806 init sequence')
ili9806_init()

sys.sleep_ms(100)

-- Confirm full-screen window before DMA (CASET 0..479, RASET 0..799)
wc(0x2A) wd(0x00) wd(0x00) wd(0x01) wd(0xDF)
wc(0x2B) wd(0x00) wd(0x00) wd(0x03) wd(0x1F)
sys.sleep_ms(50)
lcdc.mcu_set_pre_cmd(0x2C)
lcdc.mcu_dma_start(fb_addr, 1, 2)

-- ----------------------------------------------------------------
-- Pattern 1: solid colours (3 s each)
-- ----------------------------------------------------------------

fill_color(255, 0, 0)   show('[mcu] p1 solid red',   3000)
fill_color(0, 255, 0)   show('[mcu] p1 solid green', 3000)
fill_color(0, 0, 255)   show('[mcu] p1 solid blue',  3000)

-- ----------------------------------------------------------------
-- Pattern 2: horizontal stripes (8 bands × 100 px, 3 s)
-- R / white / G / white / B / white / Y / white
-- ----------------------------------------------------------------

local hpal = {R, WHT, G, WHT, B, WHT, Y, WHT}
for i, c in ipairs(hpal) do
    fill_rect(0, (i-1)*100, W, 100, c[1], c[2], c[3])
end
show('[mcu] p2 horizontal stripes (8x100px)', 3000)

-- ----------------------------------------------------------------
-- Pattern 3: vertical stripes (8 bands × 60 px, 3 s)
-- R / G / B / C / M / Y / white / black
-- ----------------------------------------------------------------

local vpal = {R, G, B, C, M, Y, WHT, BLK}
for i, c in ipairs(vpal) do
    fill_rect((i-1)*60, 0, 60, H, c[1], c[2], c[3])
end
show('[mcu] p3 vertical stripes (8x60px)', 3000)

-- ----------------------------------------------------------------
-- Pattern 4: checkerboard (8 cols × 8 rows, 60×100 px cells, 3 s)
-- ----------------------------------------------------------------

fill_color(0, 0, 0)
for row = 0, 7 do
    for col = 0, 7 do
        if (row + col) % 2 == 0 then
            fill_rect(col*60, row*100, 60, 100, 255, 255, 255)
        end
    end
end
show('[mcu] p4 checkerboard (8x8 cells)', 3000)

-- ----------------------------------------------------------------
-- Pattern 5: SMPTE colour bars (8 vertical bars × 60 px, 3 s)
-- white / yellow / cyan / green / magenta / red / blue / black
-- ----------------------------------------------------------------

local smpte = {WHT, Y, C, G, M, R, B, BLK}
for i, c in ipairs(smpte) do
    fill_rect((i-1)*60, 0, 60, H, c[1], c[2], c[3])
end
show('[mcu] p5 SMPTE colour bars', 3000)

-- ----------------------------------------------------------------
-- Pattern 6: crosshatch grid (black bg + white lines, 3 s)
-- horizontal lines every 100 px, vertical lines every 60 px
-- ----------------------------------------------------------------

fill_color(0, 0, 0)
for y = 100, 700, 100 do
    fill_rect(0, y, W, 2, 255, 255, 255)
end
for x = 60, 420, 60 do
    fill_rect(x, 0, 2, H, 255, 255, 255)
end
show('[mcu] p6 crosshatch grid', 3000)

-- ----------------------------------------------------------------
-- Status and teardown
-- ----------------------------------------------------------------

local status = lcdc.mcu_get_run_status()
print(string.format('[lcdc mcu] run_status = 0x%08X', status))

local info = lcdc.get_info()
print(string.format('[lcdc mcu] width=%d height=%d initialized=%s fb_addr=0x%08X',
    info.width, info.height, tostring(info.initialized), info.fb_addr))

-- Teardown
lcdc.deinit()
gpio.set_level(PIN_BLEN, 0)

print('success')

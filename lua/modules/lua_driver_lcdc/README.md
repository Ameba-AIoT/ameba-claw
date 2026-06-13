# lua_driver_lcdc — LCDC Controller Driver

Lua driver for the RTL8721F LCDC controller, supporting RGB parallel,
8-bit serial RGB (SRGB), and MCU (8080 parallel) interfaces.

## Hardware Requirements

- RTL8721F SoC with LCDC peripheral
- External PSRAM (framebuffer is carved from the top of the 8 MB PSRAM region)

No board-specific parameters are built into the driver. All pin assignments,
panel timing, and auxiliary GPIO (backlight, reset, display-on) are passed
by the caller.

## API Reference

```lua
local lcdc = require("lcdc")
```

### Common APIs

```lua
-- Configure LCDC signal pins (all fields optional; only provided pins are muxed).
-- Pin values are GPIO pin indices (e.g. 0x2F = PB_15).
lcdc.pinmux({
    d0 = pin, d1 = pin, ..., d23 = pin,  -- data bus (RGB: up to 24-bit; MCU: up to 24-bit)
    hsync = pin,  vsync = pin,            -- RGB sync signals
    dclk  = pin,  de    = pin,            -- clock and data-enable
    dcx = pin,  wr = pin,  rd = pin,  cs = pin,  -- MCU control signals
})

-- Trigger DMA shadow reload (RGB/SRGB mode: pushes framebuffer to screen).
lcdc.update()

-- Change DMA base address and trigger reload.
lcdc.set_framebuf(addr)

-- Return current DMA base address.
lcdc.get_framebuf_addr()   -- → addr (integer)

-- Gate LCDC output on/off.
lcdc.enable(true/false)

-- Return current scan position.
lcdc.get_cur_pos()         -- → x, y

-- Interrupt management.
lcdc.get_int_status()               -- → bitmask
lcdc.clear_int(mask)
lcdc.int_config(mask, true/false)   -- enable/disable interrupt sources
lcdc.set_line_int_pos(line)         -- line-interrupt trigger position
lcdc.set_underflow_mode(mode[, errdata])

-- Return driver state.
lcdc.get_info()
-- → { initialized, fb_addr, width, height,
--     INT_FRD, INT_LINE, INT_DMA_UDF, INT_FRM_START }

-- Disable LCDC hardware.
-- Note: auxiliary GPIOs (backlight, display-on, reset) are the caller's
-- responsibility; turn them off after deinit().
lcdc.deinit()
```

### RGB / SRGB Mode

```lua
-- Initialise RGB or SRGB interface.
-- width, height, vsw/vbp/vfp, hsw/hbp/hfp, refresh_freq are REQUIRED
-- (no defaults — they are panel-specific).
-- Returns true on success, false on failure (use pcall to catch errors).
local ok = lcdc.rgb_init({
    fb_addr      = addr,          -- framebuffer address (PSRAM) [required]
    width        = 800,           -- panel width  (pixels)        [required]
    height       = 480,           -- panel height (pixels)        [required]
    vsw          = 1,             -- vertical sync width (lines)  [required]
    vbp          = 4,             -- vertical back porch          [required]
    vfp          = 6,             -- vertical front porch         [required]
    hsw          = 4,             -- horizontal sync width        [required]
    hbp          = 40,            -- horizontal back porch        [required]
    hfp          = 40,            -- horizontal front porch       [required]
    refresh_freq = 40,            -- frame rate (Hz)              [required]
    input_fmt    = "rgb565",      -- framebuffer format    (optional)
    output_fmt   = "bgr888",      -- panel wire format     (optional)
    if_width     = "24bit",       -- RGB bus width         (optional)
    en_pol       = 1,             -- DE polarity: 1=high, 0=low  (optional)
    hs_pol       = 0,             -- HSYNC polarity              (optional)
    vs_pol       = 0,             -- VSYNC polarity              (optional)
    dclk_edge    = 1,             -- 1=falling, 0=rising         (optional)
    dma_burst    = 1,             -- 0=64B, 1=128B, 2=256B       (optional)
})

-- Supported input_fmt:  rgb565, bgr565, rgb888, bgr888,
--                       argb8888, abgr8888, argb1555, argb4444, rgb666
-- Supported output_fmt: rgb888, bgr888, rgb565, bgr565, rgb666, bgr666
-- Supported if_width:   "6bit", "8bit", "16bit", "18bit", "24bit"

-- Return HSYNC/VSYNC status.
lcdc.rgb_get_sync_status()  -- → { hs = n, vs = n }
```

### MCU (8080 parallel) Mode

```lua
-- Initialise MCU interface.
-- Returns true on success, false on failure (use pcall to catch errors).
local ok = lcdc.mcu_init({
    width      = 480,        -- panel width  (pixels)          [required]
    height     = 800,        -- panel height (pixels)          [required]
    if_width   = "24bit",    -- bus width                      [required]
    input_fmt  = "rgb888",   -- framebuffer format             [required]
    output_fmt = "rgb888",   -- panel wire format              [required]
    wrpulw     = 1,          -- WR pulse width (clock cycles)  [optional]
    wr_pol     = 0,          -- WR polarity (0=rising-fetch)   [optional]
    rd_pol     = 0,          -- RD polarity                    [optional]
    rs_pol     = 0,          -- RS/DCX polarity                [optional]
    dma_burst  = 2,          -- 0=64B, 1=128B, 2=256B         [optional]
})

-- IO-mode (register access, no DMA):
lcdc.mcu_io_write_cmd(cmd)        -- send command byte
lcdc.mcu_io_write_data(data)      -- send data byte
lcdc.mcu_io_read()                -- → data byte

-- DMA mode (framebuffer transfer):
lcdc.mcu_set_pre_cmd(cmd)             -- command sent before each DMA frame
lcdc.mcu_reset_pre_cmd()              -- clear the pre-frame command
lcdc.mcu_dma_start(fb_addr, trigger_mode, burst)
    -- trigger_mode: 1=manual trigger, 0=auto-repeat
    -- burst: 0=64B, 1=128B, 2=256B
lcdc.mcu_dma_trigger()                -- push one frame (trigger mode)
lcdc.mcu_get_run_status()             -- → status bitmask
```

## Test-only Framebuffer Helpers

`fill_color`, `fill_rect`, and `set_pixel` are **not** part of the driver API.
They are C helpers registered as plain Lua globals by `lua_lcdc_test_provision.c`
before each test script runs, and are only available inside those test scripts.

```lua
fill_color(r, g, b)              -- fill entire framebuffer
fill_rect(x, y, w, h, r, g, b)  -- fill a rectangle
set_pixel(x, y, r, g, b)        -- set one pixel
```

## Framebuffer

Allocated at runtime from the **top of PSRAM** (`0x60800000 − fb_size`)
so it never collides with the FreeRTOS heap that grows from the PSRAM base.
The address is passed to the driver via `fb_addr` in the init config table.

Typical sizes:
| Panel       | Resolution | Format  | Size    |
|-------------|------------|---------|---------|
| st7262      | 800×480    | RGB565  | 768 KB  |
| ili9806     | 480×800    | RGB888  | 1125 KB |
| st7272a     | 320×240    | RGB888  | 225 KB  |

## Tests

### RGB — st7262 800×480

```
AT+CLAW=lcdc,rgb,st7262
```

Displays 6 geometric patterns (solid colours, h-stripes, v-stripes,
checkerboard, SMPTE bars, crosshatch), 3 s each. Prints `success`.

### SRGB — ST7272A 320×240

```
AT+CLAW=lcdc,srgb,st7272a
```

Same 6-pattern sequence adapted for 320×240. Prints `success`.

### MCU (8080) — ILI9806 480×800

```
AT+CLAW=lcdc,mcu,ili9806
```

Sends ILI9806 init sequence via IO mode, then displays 6 patterns via DMA.
Prints `success`.

### Pin Mapping — st7262 (RTL8721F eval board)

| Signal | Pin   | Signal | Pin   |
|--------|-------|--------|-------|
| D0     | PB_15 | D12    | PB_22 |
| D1     | PB_17 | D13    | PB_23 |
| D2     | PB_21 | D14    | PB_14 |
| D3     | PB_18 | D15    | PB_12 |
| D4     | PA_6  | D16    | PA_22 |
| D5     | PA_8  | D17    | PA_25 |
| D6     | PA_7  | D18    | PA_29 |
| D7     | PA_10 | D19    | PB_4  |
| D8     | PB_9  | D20    | PB_5  |
| D9     | PB_11 | D21    | PB_6  |
| D10    | PB_10 | D22    | PB_7  |
| D11    | PB_16 | D23    | PB_8  |
| HSYNC  | PA_16 | VSYNC  | PA_13 |
| DCLK   | PA_9  | DE     | PA_14 |
| BLEN   | PB_3  | DISP   | PA_17 |

*BLEN and DISP are plain GPIOs managed by the test script, not LCDC pinmux.*

### Pin Mapping — ILI9806 MCU (RTL8721F eval board)

| Signal | Pin   | Signal | Pin   |
|--------|-------|--------|-------|
| D0     | PA_26 | D12    | PB_11 |
| D1     | PA_24 | D13    | PB_10 |
| D2     | PA_23 | D14    | PB_16 |
| D3     | PA_22 | D15    | PB_22 |
| D4     | PA_25 | D16    | PB_23 |
| D5     | PA_29 | D17    | PB_14 |
| D6     | PB_4  | D18    | PB_12 |
| D7     | PB_5  | D19    | PB_15 |
| D8     | PB_6  | D20    | PB_17 |
| D9     | PB_7  | D21    | PB_21 |
| D10    | PB_8  | D22    | PB_18 |
| D11    | PB_9  | D23    | PA_6  |
| DCX    | PA_9  | WR     | PA_11 |
| RD     | PA_10 | CS     | PA_7  |
| BLEN   | PB_3  | RST    | PA_8  |

### Pin Mapping — ST7272A SRGB (RTL8721F eval board)

| Signal | Pin   | Signal | Pin   |
|--------|-------|--------|-------|
| D0     | PA_6  | D4     | PA_11 |
| D1     | PA_8  | D5     | PA_9  |
| D2     | PA_7  | D6     | PA_17 |
| D3     | PA_10 | D7     | PA_16 |
| HSYNC  | PB_21 | VSYNC  | PB_17 |
| DCLK   | PB_14 | DE     | PA_14 |
| BLEN   | PB_3  | RESET  | PB_22 |

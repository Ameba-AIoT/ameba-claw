# led_strip — require("led_strip")

WS2812 / WS2812B addressable RGB LED strip driver for RTL8721F. Implemented as a
C module (`src/lua_module_led_strip.c`) that drives the WS2812 one-wire waveform
directly via SPI DMA (SSI + GDMA), without going through the `spi` Lua driver.

**Encoding** — 2.5 MHz SPI (100 MHz / div 40), 1 SPI bit = 400 ns, MSB-first:

| WS2812 bit | SPI pattern | High | Low |
|---|---|---|---|
| `1` | `0b110` | 800 ns | 400 ns |
| `0` | `0b100` | 400 ns | 800 ns |

One colour byte → 3 SPI bytes; one RGB pixel → 9 SPI bytes (GRB wire order).

Connect the strip **DIN to the MOSI pin** only. SCLK / MISO / CS are not wired.

## Constructor

```lua
local led_strip = require("led_strip")
local strip = led_strip.new({
    spi    = 1,           -- required: SPI controller index, 0 or 1
    mosi   = "PB_8",      -- required: MOSI pin (string "PA_x"/"PB_x" or raw PinName integer)
    count  = 15,          -- required: number of LEDs (>= 1)
    pinmux = "dedicated", -- optional: "dedicated" (default) or "full" (full-matrix pinmux)
})
```

Returns a strip handle (all pixels cleared on creation). Errors on controller already
in use, invalid parameters, or OOM.

## Methods

```lua
strip:set_pixel(i, r, g, b)       -- pixel i (1-based), r/g/b 0..255. Buffered.
strip:set_pixel_hsv(i, h, s, v)   -- pixel i (1-based), h 0..359, s/v 0..255. Buffered.
strip:fill(r, g, b)               -- set every pixel to one colour (RGB). Buffered.
strip:fill_hsv(h, s, v)           -- set every pixel to one colour (HSV). Buffered.
strip:clear()                      -- set every pixel off. Buffered.
strip:show()                       -- encode + push one frame via SPI DMA, then latch. Blocking.
strip:close()                      -- release handle and free buffers.

led_strip.stop_requested()         -- module fn: true once AT+CLAW=led,off was issued
                                   --   (loop scripts poll this to exit cleanly)
```

`set_pixel` / `set_pixel_hsv` / `fill` / `fill_hsv` / `clear` only touch an
in-RAM buffer; nothing changes on the strip until `show()` is called.

## Reading config from board.json

```lua
local function board_cfg()
    local ok, s = pcall(file.read, "board.json")
    if ok and s then
        local ok2, cfg = pcall(cjson.decode, s)
        if ok2 and cfg and cfg.devices then
            for _, d in ipairs(cfg.devices) do
                if d.id == "led_strip" and d.params then
                    return d.params.spi or 1,
                           d.params.mosi or "PB_8",
                           d.params.count or 15
                end
            end
        end
    end
    return 1, "PB_8", 15
end

local spi_idx, mosi, n = board_cfg()
local strip = led_strip.new({spi = spi_idx, mosi = mosi, count = n})
```

## Notes

- Colour order on the wire is GRB (handled internally; always pass r, g, b).
- DMA buffer layout: `[32 × 0x00 leading reset (~102 µs)] [count × 9 pixel bytes] [96 × 0x00 trailing reset (~307 µs)]`.
  Both reset regions are transmitted as real zero bytes on MOSI (not via sleep or SPI
  idle level). The trailing reset latches the frame; the leading reset re-addresses
  the strip to pixel 1. Both WS2812 (≥50 µs) and WS2812B (≥280 µs) minimums are met.
- Each SPI controller (0 or 1) allows only one strip handle at a time; opening a second
  handle on the same controller returns an error.
- Keep brightness modest unless the strip is adequately powered.

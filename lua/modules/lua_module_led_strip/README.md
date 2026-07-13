# lua_module_led_strip

WS2812 / WS2812B addressable RGB LED strip driver for RTL8721F, implemented as a
C Lua module that reuses the `spi` driver for the DMA transfer.

- `src/lua_module_led_strip.c` — the module (`require("led_strip")`) + AT runners
- `docs/led_strip.md` — LLM-facing API reference (staged into `rolfs:/docs`)
- `test/test_led_strip.lua` — test: reads MOSI from `board.json`, demo/loop modes

## How it works

WS2812 has no clock line — each bit is a single timed pulse. SPI runs at
6.25 MHz (`div=16`, 1 SPI bit = 160 ns) and one SPI byte encodes one WS2812 bit:
`0xF8` (`11111000`, ~0.8 µs high) = "1", `0xE0` (`11100000`, ~0.48 µs high) = "0".
Each 24-bit GRB pixel expands to 24 SPI bytes, pushed in one DMA burst on MOSI.

Wire the strip **DIN → MOSI**. Only MOSI is configurable, via `board.json`
(`devices[].id == "led_strip"`, `params.mosi`); SCLK/MISO/CS are fixed internally
(`PB_7` / unused / `PB_10`) and ignored by the strip.

**First-LED fix:** the WS2812 reset is transmitted **in-band** as ~320 µs of
`0x00` bytes (`LED_RESET_LEN`) both before and after the pixel data — not via a
sleep or the SPI idle level (the SSI idles MOSI high, so a sleep gives no reset,
the strip never re-addresses to pixel 1, and the first one or two pixels stay
dark). 320 µs clears the WS2812B 280 µs minimum. The leading run also absorbs the
SPI/DMA start-up glitch.

## AT commands

```
AT+CLAW=led            -- one-shot demo, 15 pixels (MOSI from board.json)
AT+CLAW=led,30         -- one-shot demo, 30 pixels
AT+CLAW=led,loop       -- continuous animation in the background (returns immediately)
AT+CLAW=led,loop,30    -- continuous, 30 pixels
AT+CLAW=led,off        -- stop the animation and turn the strip off
```

The demo/loop scripts read the pixel count from the global `NUM_LEDS` (injected
by the AT runner) and the MOSI pin from `board.json`. The demo prints
`[led_strip] PASS`.

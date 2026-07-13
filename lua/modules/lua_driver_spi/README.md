# lua_driver_spi

SPI master/slave driver for RTL8721F.

- `docs/spi.md` — LLM-facing API reference (concise, for use in Lua script context)
- `test/README.md` — Human-facing reference with wiring diagrams and test-case table
- `lib/lcd_spi_st7789.lua` — ST7789V LCD library (uses `spi` module + flat `gpio` API); `require("lcd_spi_st7789")`

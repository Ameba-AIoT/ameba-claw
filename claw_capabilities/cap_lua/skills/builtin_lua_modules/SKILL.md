---
name: builtin_lua_modules
description: "Index of the built-in Lua modules available to skill scripts. Activate this when writing or editing a Lua skill to look up exact module APIs instead of guessing."
compatibility: RTL8721F
metadata:
  manage_mode: readonly
  category: authoring
---
# builtin_lua_modules

Reference index for the Lua modules a skill script may use. Do NOT guess a
function signature — read the one doc you need with `file_read`.

## Three-layer lazy loading

1. Startup injects only this skill's summary.
2. Activating this skill injects this index (you are reading it).
3. Read a single module's compact API on demand:
   `file_read("rolfs:/docs/<module>.md")`

Read only the module(s) your script actually needs — this keeps the context
small. The docs are read-only built-ins under `rolfs:/docs/`.

## Module index

| Module   | require           | Doc to read                |
|----------|-------------------|----------------------------|
| gpio     | require("gpio")   | rolfs:/docs/gpio.md        |
| button   | require("button") | rolfs:/docs/button.md      |
| display  | require("display")| rolfs:/docs/display.md     |
| lvgl     | require("lvgl")   | rolfs:/docs/lvgl.md        |
| touch    | require("touch")  | rolfs:/docs/touch.md       |
| event    | require("event")  | rolfs:/docs/event.md       |
| i2c      | require("i2c")    | rolfs:/docs/i2c.md         |
| spi      | require("spi")    | rolfs:/docs/spi.md         |
| led_strip| require("led_strip") | rolfs:/docs/led_strip.md |
| environmental_sensor | require("environmental_sensor") | rolfs:/docs/environmental_sensor.md |
| light_sensor | require("light_sensor") | rolfs:/docs/light_sensor.md |
| imu      | require("imu")    | rolfs:/docs/imu.md         |
| rtc      | require("rtc")    | rolfs:/docs/rtc.md         |
| sys      | require("sys")    | rolfs:/docs/sys.md         |
| cjson    | require("cjson")  | rolfs:/docs/cjson.md       |
| timer    | require("timer")  | rolfs:/docs/timer.md       |
| cap      | require("cap")    | rolfs:/docs/cap.md         |
| file     | require("file")   | rolfs:/docs/file.md        |
| audio    | require("audio")  | rolfs:/docs/audio.md       |
| udp      | require("udp")    | rolfs:/docs/udp.md         |
| usb_msc  | require("usb_msc")| rolfs:/docs/usb_msc.md     |
| lib/resp       | require("resp")           | rolfs:/lib/resp.md         |
| lib/oled_sh1106| require("oled_sh1106")    | rolfs:/lib/oled_sh1106.md  |
| lib/gesture    | require("gesture")        | rolfs:/docs/touch.md       |

## Rules

- Only `require()` the modules listed above. C modules and Lua libs both use
  bare names (e.g. `require("gpio")`, `require("oled_sh1106")`). The legacy
  `require("lib/<name>")` form also works for Lua libs. Arbitrary file paths
  cannot be required.
- Pin name format: PA_0..PA_31, PB_0..PB_31 (e.g. "PA_25").
- For board-specific pins/peripherals, activate `board_hardware_info` first.

## Cap-first: when NOT to use Lua modules

Some tasks must use C-layer caps instead of Lua modules — using Lua
here is incorrect and will not work:

| Task | Use this cap | NOT the Lua module |
|------|--------------|--------------------|
| Real-time peer-to-peer audio streaming | `audio_stream_start` / `audio_stream_rx_start` / `audio_stream_tx_start` | `audio` module |
| LAN peer discovery | `net_discover_start` / `net_discover_peer` | `udp` module (custom loop) |

Read the cap docs before writing any audio or network code:
- `rolfs:/docs/audio_stream.md`
- `rolfs:/docs/net_discover.md`

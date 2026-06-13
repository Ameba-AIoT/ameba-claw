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
function signature — read the one doc you need with `read_file`.

## Three-layer lazy loading

1. Startup injects only this skill's summary.
2. Activating this skill injects this index (you are reading it).
3. Read a single module's compact API on demand:
   `read_file("rolfs:/docs/<module>.md")`

Read only the module(s) your script actually needs — this keeps the context
small. The docs are read-only built-ins under `rolfs:/docs/`.

## Module index

| Module   | require           | Doc to read                |
|----------|-------------------|----------------------------|
| gpio     | require("gpio")   | rolfs:/docs/gpio.md        |
| i2c      | require("i2c")    | rolfs:/docs/i2c.md         |
| rtc      | require("rtc")    | rolfs:/docs/rtc.md         |
| sys      | require("sys")    | rolfs:/docs/sys.md         |
| cjson    | require("cjson")  | rolfs:/docs/cjson.md       |
| timer    | require("timer")  | rolfs:/docs/timer.md       |
| cap      | require("cap")    | rolfs:/docs/cap.md         |
| file     | require("file")   | rolfs:/docs/file.md        |
| audio    | require("audio")  | rolfs:/docs/audio.md       |
| udp      | require("udp")    | rolfs:/docs/udp.md         |
| usb_msc  | require("usb_msc")| rolfs:/docs/usb_msc.md     |
| lib/resp       | require("lib/resp")       | rolfs:/lib/resp.md         |
| lib/oled_sh1106| require("lib/oled_sh1106")| rolfs:/lib/oled_sh1106.md  |

## Rules

- Only `require()` the C modules listed above, or blessed Lua libs as
  `require("lib/<name>")` (resolves to `rolfs:/lib/<name>.lua`). Arbitrary
  file paths cannot be required.
- Pin name format: PA_0..PA_31, PB_0..PB_31 (e.g. "PA_25").
- For board-specific pins/peripherals, activate `board_hardware_info` first.

## Cap-first: when NOT to use Lua modules

Some tasks must use C-layer caps instead of Lua modules — using Lua
here is incorrect and will not work:

| Task | Use this cap | NOT the Lua module |
|------|--------------|--------------------|
| Real-time peer audio stream (walkie-talkie) | `audio_stream_rx_start` / `audio_stream_tx_start` | `audio` module |
| LAN peer discovery | `net_discover_peer` | `udp` module (custom loop) |

Read the cap docs before writing any audio or network code:
- `rolfs:/docs/audio_stream.md`
- `rolfs:/docs/net_discover.md`

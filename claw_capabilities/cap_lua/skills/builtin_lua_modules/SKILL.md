---
name: builtin_lua_modules
description: "Built-in Lua module index. Read BEFORE writing Lua: time via sys.time() (epoch) or get_local_time cap (formatted); reply via event.notify not print; string.format %d needs an integer."
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

## Runtime constraints — read before designing your script

Available standard libraries: only `table`, `string`, `math`, `package`, and
base functions. NOT available (all `nil`): `coroutine`, `io`, `os`, `debug`,
`utf8`. Also removed from base: `load`/`loadfile`/`dofile`,
`rawget`/`rawset`/`rawequal`/`rawlen`.

- No `os.time`/`os.date` → use `sys.time()` (epoch) or the `get_local_time` cap
  (formatted local time).
- No `io.*` file I/O → use the `file` module.
- No `coroutine` → for concurrency use the `thread` module (below), not coroutines.

Concurrency = the `thread` module. Skill scripts get native job orchestration
(`thread.run`/`start`/`list`/`get`/`stop`) plus cross-job synchronization
(`thread.sync`: queue/semaphore/lock). All jobs share ONE concurrency budget.
Decide up front whether your design needs background jobs; if so, read
`rolfs:/docs/thread.md` before writing any code.

## Module index

| Module   | require           | Use it for                                                                    | Doc to read                |
|----------|-------------------|-------------------------------------------------------------------------------|----------------------------|
| gpio     | require("gpio")   | Read/write a single pin level, set pull/direction, register edge interrupts    | rolfs:/docs/gpio.md        |
| button   | require("button") | Debounced key input on a GPIO: single / double / long-press events            | rolfs:/docs/button.md      |
| pwm      | require("pwm")    | Generate PWM output: LED dimming, servo angle, buzzer tone / frequency         | rolfs:/docs/pwm.md         |
| ir       | require("ir")     | Infrared TX/RX: send and receive raw IR waveforms (e.g. NEC remote codes)      | rolfs:/docs/ir.md          |
| basictimer | require("basictimer") | Hardware timers TIM0–TIM3: periodic counting and overflow IRQ counter     | rolfs:/docs/basictimer.md  |
| i2c      | require("i2c")    | Talk to I2C peripherals: read/write registers on a bus device                  | rolfs:/docs/i2c.md         |
| spi      | require("spi")    | Talk to SPI peripherals: full-duplex byte transfers                            | rolfs:/docs/spi.md         |
| rtc      | require("rtc")    | Real-time clock: set/get wall-clock time, alarms                               | rolfs:/docs/rtc.md         |
| display  | require("display")| Low-level framebuffer drawing on the on-board screen                           | rolfs:/docs/display.md     |
| lvgl     | require("lvgl")   | Build a GUI with LVGL widgets (labels, buttons, charts) on the screen          | rolfs:/docs/lvgl.md        |
| touch    | require("touch")  | Read the GT911 touch panel: touch points / gestures                            | rolfs:/docs/touch.md       |
| uart     | require("uart")   | Send/receive bytes over a UART port; blocking read with timeout                | rolfs:/docs/uart.md        |
| captouch | require("captouch") | Self-capacitance touch detection on SoC pins; read press/release state        | rolfs:/docs/captouch.md    |
| adc      | require("adc")    | Read analog voltages on ADC input channels                                     | rolfs:/docs/adc.md         |
| thermal  | require("thermal")| Read the on-chip temperature sensor in °C; track power-on / min / max values  | rolfs:/docs/thermal.md     |
| led_strip| require("led_strip") | Drive addressable RGB LED strips (WS2812-style): per-pixel color            | rolfs:/docs/led_strip.md   |
| environmental_sensor | require("environmental_sensor") | Read temperature / humidity / pressure sensor         | rolfs:/docs/environmental_sensor.md |
| light_sensor | require("light_sensor") | Read ambient light level (lux)                                     | rolfs:/docs/light_sensor.md |
| imu      | require("imu")    | Read the 6-axis IMU: accelerometer + gyroscope + temperature                   | rolfs:/docs/imu.md         |
| magnetometer | require("magnetometer") | Read the 3-axis magnetometer (compass heading)                     | rolfs:/docs/magnetometer.md |
| audio    | require("audio")  | Record from the mic / play audio through the speaker                           | rolfs:/docs/audio.md       |
| usb_msc  | require("usb_msc")| Access a USB mass-storage device (read/write files on a USB drive)             | rolfs:/docs/usb_msc.md     |
| usb_uvc  | require("usb_uvc")| Access a USB UVC camera device (capture frames)                                | rolfs:/docs/usb_uvc.md     |
| event    | require("event")  | Reply to the user (`event.notify`) and send to a specific channel/chat         | rolfs:/docs/event.md       |
| thread   | require("thread") | Launch/manage concurrent sub-jobs and synchronize them (queue/semaphore/lock)  | rolfs:/docs/thread.md      |
| timer    | require("timer")  | Schedule a Lua callback to run once or repeatedly after a delay                | rolfs:/docs/timer.md       |
| udp      | require("udp")    | Send/receive UDP datagrams over the network                                    | rolfs:/docs/udp.md         |
| sys      | require("sys")    | System info: epoch time (`sys.time`), uptime/millis, reboot                    | rolfs:/docs/sys.md         |
| file     | require("file")   | Read/write files on the VFS (config, data, inter-job hand-off)                 | rolfs:/docs/file.md        |
| cjson    | require("cjson")  | Encode/decode JSON; build the `{ok=...}` result string returned by `run()`     | rolfs:/docs/cjson.md       |
| wifi     | require("wifi")   | Connect to a Wi-Fi AP (STA mode), check connectivity                           | rolfs:/docs/wifi.md        |
| cap      | require("cap")    | Call a C-layer capability by id (`cap.call`), list caps (`cap.list`)           | rolfs:/docs/cap.md         |
| lib/resp       | require("resp")           | Lua lib: RESP protocol helpers for Redis-compatible commands         | rolfs:/lib/resp.md         |
| lib/oled_sh1106| require("oled_sh1106")    | Lua lib: drive an SH1106 OLED over I2C (text/pixels)              | rolfs:/lib/oled_sh1106.md  |
| lib/gesture    | require("gesture")        | Lua lib: recognize swipe gestures from touch input               | rolfs:/docs/touch.md       |

## Rules

- Only `require()` the modules listed above. C modules and Lua libs both use
  bare names (e.g. `require("gpio")`, `require("oled_sh1106")`). The legacy
  `require("lib/<name>")` form also works for Lua libs. Arbitrary file paths
  cannot be required.
- Pin name format: PA_0..PA_31, PB_0..PB_31 (e.g. "PA_25").
- For board-specific pins/peripherals, activate `board_hardware_info` first.
- `string.format` is Lua's C-style `sprintf`: `%d` requires an **integer**.
  `/` division always yields a float (`10/2` → `5.0`), which `%d` rejects — use
  `//` or `math.floor()` first, or format with `%s` / `%.0f`.

## Common intents — where to look first

Map your intent to the right module/cap, then read that one doc. These are the
spots scripts most often get wrong:

| I want to… | Look at |
|------------|---------|
| Send a message back to the user who triggered this script | `event` module — `rolfs:/docs/event.md` (`event.notify(text)` replies to the triggering channel; `event.send(channel, chat_id, text)` targets a specific one). **`print()` only goes to the script log, never to the user.** |
| Get the current date/time | `sys.time()` returns a UTC Unix timestamp (like `os.time()`). For a ready-formatted **local** time string (with timezone applied), call the `get_local_time` cap via `cap.call` (see `rolfs:/docs/cap.md`) — simpler than formatting the epoch yourself. `sys.millis()`/`sys.uptime()` are since-boot only. |
| Call a C-layer capability | `cap.call(name, json)`. `name` must be the **exact bracketed id** listed by `cap.list()` — not the plugin dir (`cap_im_*`), group/family (`im_*`), or channel name. A wrong name returns `capability not found: … Did you mean: …?` — follow the suggestion. |

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

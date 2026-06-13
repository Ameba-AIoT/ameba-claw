# i2c  —  require("i2c")

Lua C module. Two-level master (bus → device) + standalone slave.
All errors raised via `error()` — wrap in `pcall` if recovery needed.

## Constraints

- `idx`: `0` or `1`. idx 0 max 400 kHz; any idx max 3400 kHz.
- `freq_hz`: 1..3400000, default 100000.
- `addr7`: 7-bit address (int). `mem_addr`: optional 8-bit register offset.

**⚠️ Parameter order: `new(idx, sda, scl)` — SDA is 2nd, SCL is 3rd. Never swap.**
Always get pin assignments from the user's hardware spec or `board_get_device()` — never guess or hardcode.

## Master

```lua
bus  = i2c.new(idx, sda, scl [, freq_hz])
list = bus:scan()                   -- table of 7-bit addrs that ACK
dev  = bus:device(addr7)            -- bind device handle
       bus:close()

n   = dev:read_byte([mem_addr])     -- mem => write-mem + restart-read 1
s   = dev:read(len [, mem_addr])    -- -> binary string
      dev:write_byte(val [, mem_addr])   -- mem => wire [mem, val]
      dev:write(data [, mem_addr])  -- data: string or int-array table
a   = dev:address()
      dev:close()
```

## Slave

```lua
s    = i2c.new_slave(idx, sda, scl, addr7 [, freq_hz])
str  = s:read(len [, timeout_ms])
sent = s:write(data)
       s:close()
```

## Examples

Read 2 bytes from reg 0x00 (e.g. MPU-6050 at 0x68, SDA=<your_sda>, SCL=<your_scl>):
```lua
local bus = i2c.new(0, "<sda_pin>", "<scl_pin>", 400000)
local dev = bus:device(0x68)
local raw = dev:read(2, 0x00)
dev:close(); bus:close()
```

Send a raw command byte sequence (e.g. SH1106 OLED at 0x3C):
```lua
-- dev:write(data)  sends data bytes as-is (no mem_addr prefix).
-- For SH1106: first byte 0x00 = command stream, rest = commands.
local bus = i2c.new(0, "<sda_pin>", "<scl_pin>", 400000)
local dev = bus:device(0x3C)
dev:write({0x00, 0xAE})          -- 0x00=cmd prefix, 0xAE=display off
dev:write({0x00, 0xAF})          -- 0xAF=display on
-- Send a data page: 0x40=data prefix, then 128 bytes of pixel data
local page = {0x40}
for i = 1, 128 do page[#page+1] = 0xFF end  -- all pixels on
dev:write(page)
dev:close(); bus:close()
```

## ⚠️ State isolation reminder

Every `lua_run` call creates a fresh state. This means:
- Bus and device handles do NOT persist — re-open `i2c.new` and re-bind `bus:device` in every script that needs I2C access.
- Lua objects built on top of I2C (OLED instance, sensor driver) are also destroyed — their internal state (framebuffer bytes, calibration data) lives in a Lua table that is gone.
- Re-create all hardware objects at the start of every call; persist display framebuffers and application state to VFS files between calls.

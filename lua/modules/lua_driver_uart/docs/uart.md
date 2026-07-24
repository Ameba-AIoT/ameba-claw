# uart  —  require("uart")

UART serial port driver for RTL8721F. `require("uart")` returns a flat table
with one constructor (`uart.new`). The constructor returns a **handle** (an
opaque userdata object) through which all operations are performed.
All functions raise via `error()` on bad arguments or hardware errors; wrap in
`pcall` if recovery is needed.

## API

```lua
-- Open a UART port
local h = uart.new(port, tx_pin, rx_pin, baud [, opts])
-- port    : 0..3 (UART0–UART3)
-- tx_pin  : string or PinName integer (e.g. "PA_25")
-- rx_pin  : string or PinName integer (e.g. "PA_26")
-- baud    : positive integer (e.g. 115200)
-- opts    : optional table:
--   { data_bits = 8,      -- 7 or 8 (default 8)
--     parity    = "none", -- "none" | "odd" | "even" (default "none")
--     stop_bits = 1 }     -- 1 or 2 (default 1)

-- Instance methods (call as h:method())
local s   = h:read(len [, timeout_ms])       -- read up to len bytes; returns string (may be shorter on timeout)
local s   = h:read_line([max_len [, timeout_ms]])  -- read until '\n' or max_len bytes; returns string
local n   = h:write(data)                    -- data: string or table of byte integers; returns bytes sent
local v   = h:available()                    -- 0 or 1 (bool): 1 if at least one byte is in RX FIFO
            h:flush_input()                  -- discard all bytes currently in RX FIFO
            h:set_loopback(enable)           -- true: connect TX to RX internally (hardware loopback)
            h:close()                        -- release hardware; port can be reopened immediately after
```

### Parameter summary

| Function       | arg1          | arg2               | arg3      | arg4      | arg5        | Returns        |
|----------------|---------------|--------------------|-----------|-----------|-------------|----------------|
| `uart.new`     | port (0–3)    | tx_pin             | rx_pin    | baud      | opts (opt)  | handle         |
| `read`         | len (1–4096)  | timeout_ms (def 0) | —         | —         | —           | string         |
| `read_line`    | max_len (opt) | timeout_ms (opt)   | —         | —         | —           | string         |
| `write`        | string/table  | —                  | —         | —         | —           | bytes_sent int |
| `available`    | —             | —                  | —         | —         | —           | 0 or 1         |
| `flush_input`  | —             | —                  | —         | —         | —           | —              |
| `set_loopback` | enable (bool) | —                  | —         | —         | —           | —              |
| `close`        | —             | —                  | —         | —         | —           | —              |

### Notes

- `available()` returns `0` or `1` (boolean semantics), **not** a byte count.
  `UART_Readable()` in fwlib reports "at least one byte present".
- `read()` with `timeout_ms=0` returns immediately with whatever is already in
  the FIFO (may be an empty string).
- `read_line()` stops when it encounters `'\n'` or when `max_len` bytes have
  been read; the `'\n'` is included in the returned string.

## Examples

```lua
local uart = require("uart")
local sys  = require("sys")

-- Open UART0 on PA_25 (TX) / PA_26 (RX) at 115200 baud
local h = uart.new(0, "PA_25", "PA_26", 115200)

-- Hardware loopback self-test
h:set_loopback(true)
h:flush_input()
h:write("ping")
sys.sleep_ms(5)
print(h:read(4, 200))  -- → "ping"
h:set_loopback(false)
h:close()
```

```lua
-- Read a line from an external device
local h = uart.new(0, "PA_25", "PA_26", 9600)
local line = h:read_line(256, 3000)
print("got: " .. line)
h:close()
```

## Concurrency & resources

### Resource lifecycle

Each UART port has an exclusive owner: only ONE handle can be open per port at
a time. `uart.new()` raises an error if the port is already in use. Calling
`close()` immediately releases the hardware (deinit + clock off) so another
call to `uart.new()` on the same port can follow right after. If `close()` is
never called, the `__gc` metamethod releases the hardware when the handle is
garbage-collected.

### Concurrent-call contract

- **One mutex per UART controller** serializes all API calls on a given port.
  Individual API calls (`read`, `write`, `available`, `flush_input`,
  `set_loopback`) are **thread-safe**: concurrent Lua tasks may call them
  through the same handle and they will be correctly serialized.
- **Multi-step sequences are NOT atomic.** A `write` followed by a `read` is
  two separate lock acquisitions; another task could call `flush_input` between
  them. To keep a transaction exclusive, your script must be the sole user of
  the handle (which is the typical case given the exclusive-ownership model).
- **Handle sharing across tasks:** a handle created in one Lua task MAY be
  passed to another task (e.g. via a shared Lua table). Each method call
  acquires and releases the lock independently, so calls are serialized but
  not atomically grouped.
- **Safe example** (single owner, sequential reads):
  ```lua
  -- Only one task holds 'h'; no sharing needed.
  local h = uart.new(0, "PA_25", "PA_26", 115200)
  h:write("cmd\r\n")
  local resp = h:read(32, 1000)
  h:close()
  ```
- **Concurrent tasks on different ports** are fully independent (each port has
  its own mutex).

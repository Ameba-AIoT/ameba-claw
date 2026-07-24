# lua_driver_uart

UART driver test for RTL8721F (AmebaGreen2). Tests run via the embedded Lua
interpreter; trigger with `AT+CLAW=uart,<mode>` over the serial console.

## API reference

Parameters verified line-by-line against `src/lua_driver_uart.c`.

### `uart.new(port, tx_pin, rx_pin, baud [, opts])` → handle

| Parameter  | Type            | Description                        | Default |
|------------|-----------------|------------------------------------|---------|
| `port`     | integer (0–3)   | UART controller index              | —       |
| `tx_pin`   | string/PinName  | TX pad (e.g. `"PA_25"`)            | —       |
| `rx_pin`   | string/PinName  | RX pad (e.g. `"PA_26"`)            | —       |
| `baud`     | integer > 0     | Baud rate (e.g. `115200`)          | —       |
| `opts`     | table (optional)| `data_bits`, `parity`, `stop_bits` | 8N1     |

`opts` fields: `data_bits` (7 or 8, default 8), `parity` ("none"/"odd"/"even",
default "none"), `stop_bits` (1 or 2, default 1).

### Handle methods

| Method                       | Returns          | Description                                   |
|------------------------------|------------------|-----------------------------------------------|
| `h:read(len [, timeout_ms])` | string           | Read up to `len` bytes; returns bytes received (may be < len on timeout) |
| `h:read_line([max, timeout])`| string           | Read until `'\n'` or `max` bytes; `'\n'` is included |
| `h:write(data)`              | integer          | Send string or `{byte, ...}` table; returns bytes sent |
| `h:available()`              | 0 or 1           | 1 if RX FIFO has at least one byte (bool semantics) |
| `h:flush_input()`            | —                | Discard all bytes in RX FIFO                  |
| `h:set_loopback(enable)`     | —                | `true`: wire TX→RX internally (hardware loopback) |
| `h:close()`                  | —                | Deinit + free port; port is reopenable immediately |

## Hardware setup

Two-board modes require physical wires between board-A (COM4) and board-B (COM9):

```
board-A (COM4)              board-B (COM9)
  PA_25 (TX) ──── Wire A ──── PA_6  (RX)
  PA_26 (RX) ──── Wire B ──── PA_7  (TX)
  GND        ─────────────── GND
```

Single-board modes (`loopback`, `loopback_baud`, `loopback_opts`, `loopback_port`)
use internal hardware loopback and require no external wiring.

## Test modes

### Single-board modes (no wiring required)

#### `loopback`

```
AT+CLAW=uart,loopback
```

UART0 on PA_25/PA_26, hardware loopback enabled. Five test cases:

| # | Name              | Validates                                                     |
|---|-------------------|---------------------------------------------------------------|
| 1 | write + read      | `write()` returns correct byte count; `read()` returns exact loopback data |
| 2 | available()       | Returns 0 before TX; returns 1 after TX                       |
| 3 | flush_input       | RX FIFO cleared; `available()` returns 0                      |
| 4 | read_line         | Stops at `'\n'`; includes terminator in result                |
| 5 | resource recycle  | `close()` releases hardware; same port can be reopened and used |

Expected output:
```
[uart] opened UART0
[uart] test 1: write + read
[uart] loopback send: Ameba UART loopback test!
[uart] write byte count: ok
[uart] loopback recv: Ameba UART loopback test!
[uart] read loopback: ok
[uart] test 2: available()
[uart] available before write: ok
[uart] loopback send: X
[uart] available after write: ok
[uart] test 3: flush_input
[uart] loopback send: ABCDEFGH
[uart] available after flush: ok
[uart] test 4: read_line
[uart] loopback send: line data test string\nmore data after newline
[uart] loopback recv line: line data test string\n
[uart] read_line ends at newline: ok
[uart] test 5: resource recycle
[uart] reopen after close: ok
[uart] loopback send: Reopen verification byte
[uart] loopback recv: Reopen verification byte
[uart] write+read after reopen: ok
success
```

#### `loopback_baud`

```
AT+CLAW=uart,loopback_baud
```

UART0 on PA_25/PA_26, hardware loopback. Sweeps five baud rates:
9600 / 38400 / 115200 / 921600 / 3000000.

#### `loopback_opts`

```
AT+CLAW=uart,loopback_opts
```

UART0 on PA_25/PA_26 at 115200, hardware loopback. Four frame-format combos:
7N1 / 8N2 / 8O1 / 8E1.

#### `loopback_port`

```
AT+CLAW=uart,loopback_port
```

Hardware loopback across three UART controllers at 115200:
UART1 (PA_25/PA_26), UART2 (PA_25/PA_26), UART3 (PA_7/PA_6).

---

### Two-board modes (wiring required, see Hardware setup)

Start the **echo side first**, then start the **sender within 90 seconds**.

#### Basic echo — UART0, 115200

board-A sends, board-B echoes:

```
board-B: AT+CLAW=uart,rx
board-A: AT+CLAW=uart,tx
```

board-B sends, board-A echoes (reversed):

```
board-A: AT+CLAW=uart,rx2
board-B: AT+CLAW=uart,tx2
```

Covers: write+read, available(), read_line over a real wire.

#### Extended echo — baud + opts + UART1/2/3

board-A sends, board-B echoes:

```
board-B: AT+CLAW=uart,rxe
board-A: AT+CLAW=uart,txe
```

board-B sends, board-A echoes (reversed):

```
board-A: AT+CLAW=uart,rxe2
board-B: AT+CLAW=uart,txe2
```

Each txe/rxe pair runs 10 sub-tests in lock-step:

| Sub-test    | Port | Baud   | Format |
|-------------|------|--------|--------|
| baud 9600   | 0    | 9600   | 8N1    |
| baud 38400  | 0    | 38400  | 8N1    |
| baud 921600 | 0    | 921600 | 8N1    |
| opts 7N1    | 0    | 115200 | 7N1    |
| opts 8N2    | 0    | 115200 | 8N2    |
| opts 8O1    | 0    | 115200 | 8O1    |
| opts 8E1    | 0    | 115200 | 8E1    |
| port 1      | 1    | 115200 | 8N1    |
| port 2      | 2    | 115200 | 8N1    |
| port 3      | 3    | 115200 | 8N1    |

## Concurrency & resources

### Init → operation → deinit lifecycle

1. **Init**: `uart.new(port, tx, rx, baud)` enables the RCC clock, configures
   pinmux, calls `UART_Init()` + `UART_SetBaud()` + `UART_RxCmd(ENABLE)`, and
   increments the controller reference count (refcnt = 1). A mutex per UART
   controller is created once in `lua_driver_uart_init()` at boot time, before
   any concurrent Lua execution starts.

2. **Exclusive ownership**: only ONE handle per UART port. `uart.new()` on an
   already-open port raises `"uart N: port already in use"`.

3. **Operation**: every method (read/write/available/flush_input/set_loopback)
   acquires the controller mutex for its duration. `read()` and `read_line()`
   hold the mutex during the entire polling loop; the wait is bounded by the
   caller-supplied `timeout_ms` argument. The mutex is always released before
   any Lua value is pushed to the stack.

4. **Deinit**: `close()` immediately decrements refcnt, calls `UART_DeInit()`,
   and disables the RCC clock when refcnt hits zero. The port is then free to
   be reopened. `__gc` is a safety net for handles that are garbage-collected
   without an explicit `close()`.

### Lock-safety guarantees

- `lua_newuserdata`, `luaL_error`, and all `luaL_check*` calls happen **before**
  `rtos_mutex_take` (CONC-02). A longjmp from any of these paths cannot leave
  the mutex locked.
- The GC path uses `RTOS_MAX_DELAY` to acquire the lock. This is safe because
  all paths that hold the mutex are bounded: `read()/read_line()` are bounded
  by their `timeout_ms` argument; `write/available/flush/set_loopback` are
  near-instantaneous.
- Test case 5 (resource recycle) verifies the full init→deinit→reinit cycle:
  after `close()` the port can be reopened and data transferred again without
  any "resource exhausted" or hardware-init failures.

### ISR safety

The driver is not ISR-safe. All operations must be called from a Lua task
context. The RX FIFO is read by polling (`UART_Readable` / `UART_CharGet`);
there is no interrupt-driven path.

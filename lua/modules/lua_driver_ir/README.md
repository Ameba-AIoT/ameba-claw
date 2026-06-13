# lua_driver_ir

Lua IR (infrared) driver for Ameba RTOS (RTL8721F).

Uses the raw fwlib IR API (`ameba_ir.h`) — no HAL/mbed layer.  
NEC protocol encoding/decoding is handled at the Lua test layer (`test/`).

> **WARNING: TX and RX are mutually exclusive.**  
> The RTL8721F IR peripheral has a single hardware block that can only
> operate in **one mode at a time** — either TX or RX, never both simultaneously.  
> Calling `send_raw()` while a `receive()` is in progress (or vice versa) will
> forcibly reconfigure the hardware and corrupt the ongoing operation.  
> Always call `dev:close()` (or finish the current operation) before switching
> direction.

## API

### `ir.new([tx_pin,] [rx_pin,] [opts])` → `dev`

Open the IR peripheral and configure pinmux.  
Either pin may be `nil` to configure only one direction.

| Parameter | Type | Description |
|-----------|------|-------------|
| `tx_pin` | string \| nil | TX pin, e.g. `"PA_25"`, or `nil` to skip TX pinmux |
| `rx_pin` | string \| nil | RX pin, e.g. `"PA_26"`, or `nil` to skip RX pinmux |
| `opts.carrier_hz` | integer | Carrier frequency in Hz (default 38000, valid range 25000–500000) |

Pin group reference for RTL8721F:

| Group | TX pin | RX pin |
|-------|--------|--------|
| S0    | PA_25  | PA_26  |
| S2    | PB_25  | PB_26  |

### `dev:send_raw(symbols [, mode])`

Send a custom waveform.

`symbols` is a Lua array where each element is `{level=0|1, duration_us=N}`:
- `level=1` — carrier burst; `level=0` — silence.
- `duration_us` — duration in microseconds.

`mode` selects the TX implementation (default `"poll"`):

| `mode` | Mechanism | CPU during TX |
|--------|-----------|---------------|
| `"poll"` | Task busy-polls `IR_GetTxFIFOFreeLen()` to refill FIFO | Occupied |
| `"intr"` | ISR refills FIFO on `IR_BIT_TX_FIFO_LEVEL_INT`; task blocks on RTOS semaphore until `IR_BIT_TX_FIFO_EMPTY_INT` | Freed |

Internally converts duration to carrier clock cycles.

### `dev:receive([timeout_ms])` → `symbols` | `nil, "timeout"`

Capture a raw IR frame. **Requires an external IR source** (physical remote
control or another IR transmitter). Returns `nil, "timeout"` on timeout.

Returned symbols are `{level=0|1, duration_us=N}` — compatible with
`send_raw()` for record-and-replay.

RX uses a 1 MHz sample clock (1 count = 1 µs).  
Frame end is detected by 5 ms of silence (`IR_RX_COUNT_LOW_LEVEL` threshold).

### `dev:info()` → table

Returns `{carrier_hz, tx_pin, rx_pin}`. Pin fields are `nil` if that direction
was not configured on `new()`.

### `dev:close()`

Disable IR hardware and release the clock gate.

## Test

Tests are in `test/` and triggered manually (not auto-run on boot):

| File | Trigger | Description |
|------|---------|-------------|
| `test_ir_tx_poll.lua` | `AT+CLAW=ir,tx,poll` | TX cross-test, polling mode: PA_25 → Dplus RX PA_27 |
| `test_ir_tx_intr.lua` | `AT+CLAW=ir,tx,intr` | TX cross-test, interrupt mode: PA_25 → Dplus RX PA_27 |
| `test_ir_rx.lua` | `AT+CLAW=ir,rx` | RX cross-test: receives on PA_26 from Dplus TX PA_26, 15 s timeout |
| `lua_ir_test_provision.c` | (boot) | Embeds all scripts as C strings; `lua_driver_ir_provision()` writes them to VFS on boot; `lua_ir_run()` routes `AT+CLAW=ir,<mode>` to the correct script via a dedicated FreeRTOS task |
| `ir_nec_protocol.c` | — | C reference implementation of NEC encoding (not built, for comparison only) |

`AT+CLAW=ir,tx` (no sub-mode) is kept as a backward-compatible alias for `tx,poll`.

Cross-test wiring (already connected):
- RTL8721F TX **PA_25** ↔ Dplus RX **PA_27**
- RTL8721F RX **PA_26** ↔ Dplus TX **PA_26**

NEC encoding (`test/ir_nec_protocol.c`) is provided as a C reference; the Lua
tests implement encoding in pure Lua via `send_raw()`.

## Hardware notes

- RTL8721F IR peripheral: one TX + one RX, exclusive TX/RX modes.
- `send_raw` and `receive` automatically reconfigure the hardware for the
  appropriate mode.
- Only one IR device instance is meaningful at a time (single peripheral).
- **TX poll** (`send_raw(symbols, "poll")`): task busy-polls `IR_GetTxFIFOFreeLen()`
  to refill the FIFO in chunks, then waits a fixed 100 ms post-TX delay.
- **TX intr** (`send_raw(symbols, "intr")`): `IR_BIT_TX_FIFO_LEVEL_INT` fires when
  FIFO drops to ≤ 15 entries; ISR refills from the pre-built symbol buffer.
  `IR_BIT_TX_FIFO_EMPTY_INT` fires when all data is consumed; ISR gives a semaphore
  that unblocks the Lua task. A 2 ms tail delay covers the last symbol still in the
  hardware shift register.
- **RX is interrupt-driven**: the ISR drains the RX FIFO into a module-level
  buffer on each FIFO-level/full interrupt; a `IR_BIT_RX_CNT_THR_INT` (5 ms
  low-level silence) signals frame end and gives the RTOS semaphore that
  unblocks `receive()`.

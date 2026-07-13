# lua_driver_ir

Infrared TX/RX Lua driver for Ameba RTOS (RTL8721F). Wraps the raw fwlib IR
API (`ameba_ir.h`). NEC protocol encoding/decoding is implemented in pure Lua
at the test layer; the driver only exposes raw symbol arrays.

> **TX and RX are mutually exclusive.** The IR peripheral has a single
> hardware block that can operate in only one mode at a time. `send_raw()` and
> `receive()` each reconfigure the hardware; a per-peripheral mutex ensures
> concurrent callers serialise correctly.

## API

### `ir.new(tx_pin, rx_pin [, opts])` → `dev`

Open the IR peripheral. Either pin may be `nil` to skip that direction.

| Parameter | Type | Description |
|-----------|------|-------------|
| `tx_pin` | string \| nil | TX pin name, e.g. `"PA_25"`, or `nil` |
| `rx_pin` | string \| nil | RX pin name, e.g. `"PA_26"`, or `nil` |
| `opts.carrier_hz` | integer | Carrier Hz (default `38000`, valid `25000–500000`) |

### `dev:send_raw(symbols [, mode])`

Send a custom waveform. `symbols` is a Lua array of `{level=0|1, duration_us=N}`.
Requires a `tx_pin` configured in `ir.new()` — errors immediately with
`"RX-only device"` if the handle was opened with `tx_pin = nil`.

| `mode` | Mechanism | CPU during TX |
|--------|-----------|---------------|
| `"poll"` (default) | Busy-poll `IR_GetTxFIFOFreeLen()` | Occupied |
| `"intr"` | ISR refills FIFO; task blocks on semaphore | Freed |

### `dev:receive([timeout_ms])` → `symbols` \| `nil, "timeout"`

Capture a raw IR frame (requires an external IR source). Default timeout 5000 ms.
Returns `{level, duration_us}` array on success; `nil, "timeout"` on timeout.
RX uses 1 MHz sample clock (1 count = 1 µs).
Requires an `rx_pin` configured in `ir.new()` — errors immediately with
`"TX-only device"` if the handle was opened with `rx_pin = nil`.

### `dev:info()` → table

Returns `{carrier_hz=N, tx_pin=N|nil, rx_pin=N|nil}`.

### `dev:close()`

Disable IR hardware and gate the peripheral clock.

## Test cases

| File | AT command | Description |
|------|-----------|-------------|
| `test_ir_tx_poll.lua` | `AT+CLAW=ir,tx,poll` | TX cross-test, polling mode |
| `test_ir_tx_intr.lua` | `AT+CLAW=ir,tx,intr` | TX cross-test, interrupt mode |
| `test_ir.lua` | `AT+CLAW=ir,tx` | TX (backward-compat alias for `tx,poll`) |
| `test_ir_rx.lua` | `AT+CLAW=ir,rx` | RX cross-test, 15 s timeout |
| `lua_ir_test_provision.c` | `AT+CLAW=ir,pin_check` | Pin direction guard: TX-only rejects `receive()`, RX-only rejects `send_raw()` — both must error immediately |

## Cross-test wiring (COM5 = green2, COM7 = dplus)

```
RTL8721F (green2, COM5)   RTL8721Dx (dplus, COM7)
    PA_25 (TX) ──────────── PA_27 (RX)
    PA_26 (RX) ──────────── PA_26 (TX)
    GND        ──────────── GND
```

Pin group reference for RTL8721F IR:

| Group | TX  | RX  |
|-------|-----|-----|
| S0    | PA_25 | PA_26 |
| S2    | PB_25 | PB_26 |

## Running the cross-test

1. **Dplus board (COM7) — run first:**
   ```
   AT+CLAW=ir,rx
   ```
   The RX script waits 15 s for an IR signal on PA_26.

2. **Green2 board (COM5) — run within 15 s:**
   ```
   AT+CLAW=ir,tx,poll    (or tx,intr)
   ```
   Sends NEC frame: addr=0x12, cmd=0x34.

3. **Expected output:**

   COM7 (rx):
   ```
   [ir_rx] test start - waiting for IR signal (15s timeout)
   [ir_rx] received N symbols
   [ir_rx] NEC addr=0x12 cmd=0x34
   [ir_rx] success
   ```

   COM5 (tx):
   ```
   [ir_tx_poll] test start
   [ir_tx_poll] tx=PA_25 carrier=38000Hz mode=poll
   [ir_tx_poll] NEC send addr=0x12 cmd=0x34 done (67 symbols)
   [ir_tx_poll] success
   ```

## Concurrency & resources

**Init → operation → deinit lifecycle:**
- `ir.new()` enables the IRDA peripheral clock, configures pinmux, initialises
  the peripheral in TX mode, and returns a handle.
- `send_raw()` / `receive()` each reconfigure the hardware for their mode
  unconditionally (TX init / RX init) before operating.
- `dev:close()` (explicit) or `__gc` (Lua GC) disables TX, disables RX, calls
  `IR_DeInit()`, and gates the clock.
- To verify resource release (step 14 pattern): close the device and call
  `ir.new()` again with the same pins — it must succeed without error.

**Locking model:**
- One `rtos_mutex_t s_ir_lock` guards the single IR peripheral.
- Created in `lua_driver_ir_init()` during single-threaded boot (before any
  concurrent Lua execution), so no race on first use.
- Held for the **entire duration** of `send_raw()`, `receive()`, and `close()`:
  - TX poll: task busy-polls + 100 ms post-TX delay while holding lock.
  - TX intr: task blocks on `s_tx.done_sema` (given by ISR) while holding lock.
  - RX: task blocks on `s_rx.end_sema` (given by ISR) while holding lock.
- ISR never takes the mutex — only gives semaphores → no deadlock.
- Lock timeout: `RTOS_MAX_DELAY` (safe because all operations are bounded).

**Critical-section purity (CONC-02):**
- `lua_newuserdata` in `new()` happens **before** `IR_LOCK()`.
- In `receive()`, `u32 count = s_rx.len` is captured inside the lock; the Lua
  table is built **after** `IR_UNLOCK()`.
- No `luaL_error` / `luaL_check*` / memory allocation inside any lock region.

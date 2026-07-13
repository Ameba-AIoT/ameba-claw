# ir — require("ir")

Infrared TX/RX driver for the RTL8721F. `require("ir")` returns a flat table
with one constructor; calling `ir.new()` returns a **handle** (device object).
Uses the raw fwlib IR API — no HAL/mbed layer. NEC encoding/decoding is done
at the Lua caller layer; this module only sends and receives raw symbol arrays.

> **TX and RX are mutually exclusive.** The IR peripheral has one hardware
> block. `send_raw()` and `receive()` each reconfigure the hardware; never call
> one while the other is in progress from a different Lua task — the driver
> serialises them via a mutex but they run sequentially, not concurrently.

## API

```lua
-- Open the IR peripheral (either or both directions)
dev = ir.new(tx_pin, rx_pin [, opts])
-- tx_pin, rx_pin: pin name string (e.g. "PA_25") or nil to skip that direction
-- opts.carrier_hz: integer, default 38000, valid 25000–500000

-- Send a raw waveform (TX mode) — requires tx_pin != nil in new()
dev:send_raw(symbols [, mode])
-- symbols: array of {level=0|1, duration_us=N}
--   level=1 → carrier burst; level=0 → silence
-- mode: "poll" (default) | "intr"
--   "poll" — task busy-polls FIFO; CPU occupied during TX
--   "intr" — ISR refills FIFO; task blocks on semaphore (CPU freed)
-- Errors if device was opened without a tx_pin (RX-only handle).

-- Receive a raw frame (RX mode) — requires rx_pin != nil in new()
local syms, err = dev:receive([timeout_ms])
-- timeout_ms: default 5000
-- returns: symbol array on success; nil, "timeout" on timeout
-- Errors if device was opened without an rx_pin (TX-only handle).

-- Query configuration
local t = dev:info()
-- returns: {carrier_hz=N, tx_pin=N|nil, rx_pin=N|nil}

-- Release hardware
dev:close()
```

## Pin groups (RTL8721F)

| Group | TX  | RX  |
|-------|-----|-----|
| S0    | PA_25 | PA_26 |
| S2    | PB_25 | PB_26 |

## Examples

TX with NEC encode (polling mode):
```lua
local ir = require("ir")
local dev = ir.new("PA_25", nil)
local syms = {}
-- ... fill syms with NEC-encoded {level, duration_us} entries ...
dev:send_raw(syms, "poll")
dev:close()
```

RX capture and print:
```lua
local ir = require("ir")
local dev = ir.new(nil, "PA_26")
local syms, err = dev:receive(10000)
if syms then
    for i, s in ipairs(syms) do
        print(i, s.level, s.duration_us)
    end
else
    print("no signal:", err)
end
dev:close()
```

## Concurrency & resources

**Resource lifecycle:** Each `ir.new()` enables the IRDA peripheral clock and
configures pinmux. `dev:close()` (or GC) disables the hardware and gates the
clock. There is only one IR peripheral; at most one useful handle should be
open at a time. To reuse after `close()`, call `ir.new()` again.

**Concurrency contract (for script authors):**

- **Single peripheral mutex:** one `rtos_mutex_t` guards the hardware for the
  full duration of every operation (`new` / `send_raw` / `receive` / `close`).
  Multiple concurrent Lua tasks can each call the IR API; they will execute
  **sequentially**, never simultaneously on the hardware.
- **Individual calls are thread-safe:** calling `send_raw()` from task A while
  task B calls `receive()` will not corrupt hardware state — task B blocks on
  the mutex until task A finishes, then reconfigures the peripheral for RX.
- **Multi-call sequences are NOT atomic:** there is no guarantee that a
  `send_raw()` immediately followed by `receive()` from the same script will
  complete without another task's operation in between. If exclusive ownership
  of a complete TX→RX exchange is needed, run it in a single dedicated task.
- **Handles are NOT shareable across tasks:** a `dev` handle should be created,
  used, and closed within the same Lua script execution context.
- **Bounded lock hold:** `receive()` holds the mutex while waiting for a frame
  (up to `timeout_ms`). Other tasks wanting IR will block for that duration.
  Choose `timeout_ms` values that match your application's latency budget.

Example showing safe concurrent usage:
```lua
-- Script A (TX task) and Script B (RX task) can run simultaneously;
-- the driver serialises them. Script B's receive() will wait for A's
-- send_raw() to finish before reconfiguring to RX mode.
-- Task A:
local dev = ir.new("PA_25", nil); dev:send_raw(nec_syms); dev:close()
-- Task B:
local dev = ir.new(nil, "PA_26"); local s = dev:receive(5000); dev:close()
```

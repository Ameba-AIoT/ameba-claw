# lua_driver_captouch

Self-capacitance touch-key driver for Ameba RTOS (RTL8721F / AmebaGreen2).

Uses fwlib raw API (`CapTouch_Init`, `CapTouch_Cmd`, `CapTouch_GetChAveData`,
`CapTouch_GetChBaseline`, `CapTouch_GetChDiffThres`, etc.) in polling mode — no
interrupts, no GDMA, no HAL layer. `require("captouch")` returns a table with a
`new()` constructor that returns a handle object.

## API

```lua
local captouch = require("captouch")
local sys = require("sys")

-- Constructor
-- captouch.new(pin1 [, pin2] [, opts])
local dev = captouch.new("PA_16", "PA_17", {
    threshold   = 1600,       -- CT_DiffThrehold, 1..4095 (default 1600); hw reg is 12-bit, values > 4095 are silently masked
    mbias       = 0x800,      -- common CT_MbiasCurrent 0..65535 (default 0x800)
    n_noise_thr = 800,        -- CT_ETCNNoiseThr 0..4095 (default 800)
    p_noise_thr = 800,        -- CT_ETCPNoiseThr 0..4095 (default 800)
    interval_ms = 60,         -- scan interval ms, 1..4000 (default 60)
    ch_threshold= {80, 50},   -- per-key diff threshold {key1, key2} (0 = use common)
    ch_mbias    = {0x590, 0x560},  -- per-key mbias {key1, key2} (0 = use common)
    name        = "mypad",    -- device name (default "touch_keys")
})

-- Parameters above verified line-by-line against src/lua_driver_captouch.c.

sys.sleep_ms(1000)  -- wait for hardware baseline calibration (recommended >= 500 ms)

-- dev:read() → state table
local s = dev:read()
-- s.count         integer  number of configured keys
-- s.any_pressed   boolean
-- s.pressed_count integer
-- s.keys[i]       per-key table (1-based):
--   .index     1-based key index
--   .channel   CapTouch channel number (0..8)
--   .pin       raw PinName integer
--   .pressed   boolean
--   .smooth    latest averaged ADC count
--   .benchmark baseline ADC count (auto-calibrated by ETC hardware)
--   .delta     benchmark - smooth (positive when touched)
--   .threshold current diff threshold for this key

-- dev:is_pressed(index) → boolean (1-based index)
local ok = dev:is_pressed(1)

-- dev:set_threshold(index, val) — update diff threshold at runtime (1-based, 1..65535)
dev:set_threshold(1, 100)

-- dev:set_mbias(index, val) — update mbias at runtime (1-based, 0..65535)
dev:set_mbias(1, 0x500)

-- dev:set_scan_interval(val) — raw register value 0..4095
dev:set_scan_interval(60)

-- dev:get_ch_status(index) → integer (1=channel enabled, 0=disabled)
local st = dev:get_ch_status(1)

-- dev:name() → string
local name = dev:name()

-- dev:close() — release channels; disable peripheral when last handle closes
dev:close()
```

## Test

The suite is in `test_captouch.lua` (also embedded in `lua_captouch_test_provision.c`
— the two MUST stay in sync). It is written to VFS at boot and triggered on demand.

Trigger via AT command on the serial console:

```
AT+CLAW=captouch          -- interactive test: touch PA_17 and PA_16 with a finger
AT+CLAW=captouch,ext      -- ext raw-monitor test: no physical touch required
```

### Wiring

**No external hardware required** for `ext` mode (baseline sanity check only).

For `interactive` mode, physically touch each of the two test pads with a finger:

| Pin   | CapTouch Channel | Role           |
|-------|-----------------|----------------|
| PA_16 | CH4             | key index 1    |
| PA_17 | CH3             | key index 2    |

No pull resistors or external components are needed. Bare copper pads or even a
finger touching the exposed SoC pad traces are sufficient.

> **SWD note:** PA_18 and PA_19 share the SWD debug port. If a debugger is
> connected, avoid those pins. PA_16 and PA_17 are safe.

### Test cases

| Mode        | # | Check                              | What it exercises                                                 |
|-------------|---|------------------------------------|-------------------------------------------------------------------|
| interactive | 1 | PA_16 press + release              | `read()` detects delta ≥ threshold on CH4                        |
| interactive | 2 | PA_17 press + release              | `read()` detects delta ≥ threshold on CH3                        |
| interactive | — | both within 30 s                   | `any_pressed`, `pressed_count`, `is_pressed` (indirectly)        |
| ext         | 1 | 10 samples on PA_16 + PA_17        | baseline in range 50..60000 for each sample                      |
| ext         | 2 | `get_ch_status` per key            | channel enable status (1=enabled) is readable                    |
| both        | — | `close()` + re-open                | refcount decrement → peripheral disable → re-enable without hang |

Final output is `[captouch_test] success` on pass, or `[captouch_test] FAIL` with details.

## Concurrency & resources

**Init → operation → deinit lifecycle:**

1. **Init:** `captouch.new()` enables `APBPeriph_ADC` clock, runs `CapTouch_StructInit` +
   `CapTouch_Init`, disables all interrupts (polling only), and calls `CapTouch_Cmd(ENABLE)`.
   This path is protected by `s_ctc_lock` (mutex). Subsequent `captouch.new()` calls
   (additional channels) skip re-init and only enable the new channels under the lock.

2. **Operation:** `set_threshold()`, `set_mbias()`, and `set_scan_interval()` each
   take `s_ctc_lock` around the register write and release it immediately.
   `read()`, `is_pressed()`, and `get_ch_status()` do **not** take the mutex — they
   perform direct read-only register accesses (`CapTouch_GetChAveData`,
   `CapTouch_GetChBaseline`, `CapTouch_GetChDiffThres`, `CapTouch_GetChStatus`).
   Concurrent reads are safe in practice, but a read racing with a `set_*` call may
   observe the old threshold/mbias value. **Multi-call sequences are not atomic.**

3. **Deinit:** `dev:close()` (or GC `__gc`) disables the handle's channels under
   `s_ctc_lock`, decrements `s_ctc_refcount`, and when the count reaches zero calls
   `CapTouch_Cmd(DISABLE)` + gates the ADC clock. Re-open is fully supported:
   the test calls `close()` and then `captouch.new()` again to verify the peripheral
   can be re-initialized without hang or resource exhaustion.

**Lock scope and ISR:** There is no ISR in this driver (polling only). The mutex
`s_ctc_lock` is taken only from task context, never from ISR. `luaL_error` / GC paths
that call `close()` do so before or after the lock (the do_close helper takes the lock
internally), so no longjmp-inside-lock scenario exists.

**Handle sharing:** Each handle tracks its own `channel[]`, `pin[]`, and `closed`
flag. Handles must not be passed between `lua_run` tasks — those fields are not
protected against cross-task ownership transfer. Create a fresh handle per task.

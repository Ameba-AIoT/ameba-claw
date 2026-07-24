# lua_driver_thermal

Lua driver for the on-chip thermal sensor (TM peripheral) on Ameba RTOS (RTL8721F).

Uses fwlib raw API (`TM_StructInit`, `TM_Init`, `TM_GetTempResult`, `TM_GetCdegree`,
`TM_GetFdegree`, etc.) directly — no HAL layer dependency. `require("thermal")`
returns a module table; `thermal.new()` returns a handle with all read/manage methods.

## API

```lua
local thermal = require("thermal")

local thm = thermal.new()          -- enable TM clock + TM_Init (ref-counted); returns handle

-- Temperature reads (all return float)
local c    = thm:read()            -- current temperature in degrees Celsius
local f    = thm:read_f()          -- current temperature in degrees Fahrenheit
local c0   = thm:power_on_temp()   -- temperature at last power-on reset (°C)
local cmax = thm:max_temp()        -- peak temperature since last clear_max (°C)
local cmin = thm:min_temp()        -- trough temperature since last clear_min (°C)

-- Stat management
thm:clear_max()                    -- reset HW max-temperature latch
thm:clear_min()                    -- reset HW min-temperature latch

-- Lifecycle
thm:close()                        -- release handle; powers TM off when ref-count reaches 0
```

All functions raise via `error()` on hardware timeout. Parameter orders and defaults
verified line-by-line against `src/lua_driver_thermal.c`.

### Parameter summary

| Function | arg (after self) | Returns |
|----------|-----------------|---------|
| `thermal.new()` | — | handle |
| `thm:read()` | — | float (°C) |
| `thm:read_f()` | — | float (°F) |
| `thm:power_on_temp()` | — | float (°C) |
| `thm:max_temp()` | — | float (°C) |
| `thm:min_temp()` | — | float (°C) |
| `thm:clear_max()` | — | — (void) |
| `thm:clear_min()` | — | — (void) |
| `thm:close()` | — | — (void) |

## Test

Test script: `test/test_thermal.lua`, embedded as a C string via
`test/lua_thermal_test_provision.c` (auto-generated from the `.lua` at CMake
configure time as `thermal_test_lua.h`). On boot the script is written to
`vfs:test_thermal.lua`. Trigger via AT command:

```
AT+CLAW=thermal                    -- defaults: 5 reads, 200 ms interval
AT+CLAW=thermal,<count>            -- override read count
AT+CLAW=thermal,<count>,<interval_ms>  -- override both
```

Parameters are optional and positional; use -1 (or omit) to keep the default.
Examples:
- `AT+CLAW=thermal,10,1000` — 10 readings at 1-second intervals
- `AT+CLAW=thermal,3` — 3 readings at default 200 ms

### Wiring

**No external wiring required.** The thermal sensor is on-chip. Works on a bare
RTL8721F evaluation board with no additional components.

### Test cases

| # | Check | What it exercises |
|---|-------|-------------------|
| 1 | power-on temp | `thermal.new()`, `thm:power_on_temp()` |
| 2 | 5x read (Celsius + Fahrenheit) | `thm:read()`, `thm:read_f()` with 200 ms intervals |
| 3 | max / min stat | `thm:max_temp()`, `thm:min_temp()` |
| 4 | clear max and min | `thm:clear_max()`, `thm:clear_min()` |
| 5 | resource recycle | `thm:close()` then `thermal.new()` + `read()` + `close()` — verifies the hardware can be fully torn down and re-initialised without error |

Each step prints a result line. The final line is `[thermal_test] success` when all
checks pass, `[thermal_test] FAIL` otherwise.

All 9 exported APIs are covered: `new`, `read`, `read_f`, `power_on_temp`,
`max_temp`, `min_temp`, `clear_max`, `clear_min`, `close`.

## Concurrency & resources

**Resource model (init -> operation -> deinit):**

- **Init:** `thermal.new()` takes `s_thermal_lock`, increments `s_thermal_refcount`,
  and (on the first call) enables `APBPeriph_THM` clock then calls
  `TM_StructInit` + `TM_Init`. Subsequent `new()` calls share the same hardware.
- **Operation:** read functions (`read`, `read_f`, `max_temp`, etc.) call fwlib
  functions that poll `TM_PollDataValid` then read hardware registers. They do not
  modify shared state and run without the module lock.
- **Deinit:** `close()` takes `s_thermal_lock`, decrements `s_thermal_refcount`, and
  when the count reaches zero calls `TM_Cmd(DISABLE)` + disables the THM clock. The
  `__gc` metamethod mirrors `close()` for GC-based cleanup.

**Test case 5 verifies the full init -> operation -> deinit -> re-init cycle:** after
the first `close()`, a second `thermal.new()` successfully re-initialises the hardware
and reads a valid temperature, confirming the ref-count and RCC logic are correct.

**Lock scope:** `s_thermal_lock` is held only during `new()` and `close()` (RCC +
ref-count updates). Read operations run outside the lock. This means:
- Concurrent `new()` / `close()` calls from different Lua tasks are safe.
- Concurrent read calls are safe (no shared mutable state).
- There is no ISR; `TM_PollDataValid` is a busy-wait loop — do not call reads from
  interrupt context.

**Handle sharing:** do not pass a handle to a different Lua task. Each task should
call `thermal.new()` independently; the ref-count keeps the hardware alive as long as
any handle is open.

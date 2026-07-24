# thermal — require("thermal")

On-chip temperature sensor for the RTL8721F. `require("thermal")` returns a module
table with a single factory `new`. All reads are hardware real-time; no external
wiring is needed.

## API

```lua
local thermal = require("thermal")

local thm = thermal.new()          -- create handle; enables TM clock (ref-counted)

-- Temperature reads (all return float)
local c    = thm:read()            -- current temperature in degrees Celsius
local f    = thm:read_f()          -- current temperature in degrees Fahrenheit
local c0   = thm:power_on_temp()   -- temperature recorded at last power-on reset (°C)
local cmax = thm:max_temp()        -- peak temperature since last clear_max (°C)
local cmin = thm:min_temp()        -- trough temperature since last clear_min (°C)

-- Stat management
thm:clear_max()                    -- reset stored max (HW auto-re-latches next period)
thm:clear_min()                    -- reset stored min

-- Lifecycle
thm:close()                        -- release handle; HW powered down when ref-count hits 0
```

All functions raise via `error()` on hardware timeout; wrap in `pcall` if recovery is needed.

### Parameter summary

| Function | arg (after self) | Returns |
|----------|-----------------|---------|
| `thermal.new()` | — | handle |
| `thm:read()` | — | float (°C) |
| `thm:read_f()` | — | float (°F) |
| `thm:power_on_temp()` | — | float (°C) |
| `thm:max_temp()` | — | float (°C) |
| `thm:min_temp()` | — | float (°C) |
| `thm:clear_max()` | — | — |
| `thm:clear_min()` | — | — |
| `thm:close()` | — | — |

## Examples

Basic read:
```lua
local thermal = require("thermal")
local thm = thermal.new()
local ok, t = pcall(thm.read, thm)
if ok then print("temp: " .. t .. " °C") end
thm:close()
```

Polling loop:
```lua
local thermal = require("thermal")
local sys = require("sys")
local thm = thermal.new()
for i = 1, 10 do
    local ok, t = pcall(thm.read, thm)
    if ok then print(i, t) end
    sys.sleep_ms(500)
end
thm:close()
```

## Concurrency & resources

**Resources:** `thermal.new()` enables the TM peripheral clock (`APBPeriph_THM`) and
initialises the hardware on the **first** call; subsequent `new()` calls increment a
reference count without re-initialising. `thm:close()` decrements the count and, when
it reaches zero, calls `TM_Cmd(DISABLE)` and turns off the clock. The `__gc`
metamethod also calls close, so handles are cleaned up automatically if a script exits
without closing. There is no GDMA channel or timer allocation — only the clock.

**Concurrent use — calling contract:**

1. **Concurrent calls from multiple Lua tasks are safe for `new()` and `close()`**: a
   process-wide mutex (`s_thermal_lock`) protects reference-count updates and RCC
   calls. Individual read functions (`read`, `read_f`, `max_temp`, etc.) access
   hardware registers without modifying shared state and do not hold the lock; they
   may be called concurrently from different scripts.

2. **Cross-call sequences are NOT atomic**: a sequence such as `read()` then
   `clear_max()` is not atomic — another task may call `read()` or `clear_max()`
   between the two. Serialise at the script level if atomicity is required.

3. **Do not share a handle across Lua tasks**: each task should call `thermal.new()`
   independently. The ref-count keeps the hardware alive as long as any handle is open.

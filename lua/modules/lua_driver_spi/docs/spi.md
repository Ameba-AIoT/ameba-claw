# spi — require("spi")

SPI master/slave driver for RTL8721F. Backed by raw fwlib
(`SSI_*`, `GDMA_*`). Exposes handle objects with polling, interrupt, and
DMA transfer methods.

## Constructors

```lua
-- Master (generates clock).  miso may be nil for write-only devices (e.g. ST7789).
local m = spi.new(idx, sclk, mosi, miso, cs [, opts])

-- Slave (clock driven by master).
local s = spi.new_slave(idx, sclk, mosi, miso, cs [, opts])
```

| Parameter | Type / Value | Notes |
|-----------|-------------|-------|
| `idx`  | 0 or 1 | SPI peripheral index |
| `sclk` | string pin name e.g. `"PB_7"` | SCLK pin |
| `mosi` | string pin name | MOSI pin |
| `miso` | string pin name or `nil` | MISO pin; `nil` skips MISO pinmux (write-only) |
| `cs`   | string pin name | CS pin (hardware CS via pinmux) |
| `opts` | table, optional | `{div=N, cpol=0/1, cpha=0/1}` — div: clock divider (100 MHz / div), must be ≥ 2 and even; defaults: div=20 (5 MHz), cpol=0, cpha=0 |

Returns an SPI handle.  Errors on pin conflict or hardware fault.

## Methods (all handle methods)

```lua
-- Polling
dev:write(data)          -- string → nil. Master: TX+RX simultaneous (RX discarded).
                         -- Slave:  preload TX FIFO; master drives clock later.
dev:read(n)              -- integer → string. Master: send n dummy bytes, return RX.
                         -- Slave:  drain n bytes from RX FIFO (no TX write).

-- ISR-driven (non-blocking arm + blocking wait)
dev:write_intr(data)     -- string → nil. Arms TX ISR; blocks until TX FIFO empty.
dev:read_intr(n)         -- integer → string. Arms RX ISR; blocks until n bytes received.

-- DMA-driven
dev:write_dma(data)      -- string → nil. Master: arms TX+RX DMA; returns on RX done.
                         -- Slave:  arms TX DMA only; returns on TX done.
dev:read_dma(n)          -- integer → string. Master: arms RX+TX dummy DMA; returns RX.
                         -- Slave:  arms RX DMA only; blocks until n bytes received.

-- Lifecycle
dev:close()              -- Release hardware. Handle becomes invalid.
```

Transfer timeout: 5000 ms (all methods). `luaL_error` on timeout or hardware fault.
Per-call transfer size is limited only by available heap (no fixed upper bound).

## Example

```lua
-- Master write + slave read (SPI1 master ↔ SPI0 slave loopback)
local m = spi.new(1, "PB_7", "PB_8", "PB_9", "PB_10", {div=20})
local s = spi.new_slave(0, "PA_29", "PA_30", "PA_31", "PB_0")
m:write("\x01\x02\x03")
local rx = s:read(3)       -- returns "\x01\x02\x03"
m:close()
s:close()

-- Write-only master (ST7789, no MISO)
local lcd = spi.new(1, "PB_7", "PB_8", nil, "PB_10", {div=20})
lcd:write_dma(string.char(0x2C))  -- send command byte
lcd:close()
```

## Concurrency & resources

**Resources**: Each `spi.new` / `spi.new_slave` call consumes one hardware SPI
controller (SPI0 or SPI1). Both handles are reference-counted: opening the same
controller twice with identical config is allowed; the second `close()` releases
hardware. Opening with conflicting config raises an error.

DMA transfers allocate GDMA channels per-transfer and release them immediately on
completion (in the ISR callback on success, or via an abort helper on timeout) —
repeated `write_dma` / `read_dma` calls do not accumulate GDMA channel usage.

Always call `dev:close()` when done (or wrap in `pcall`/`local` scope) to free the
controller for other scripts.

**Concurrent access**: A per-controller mutex serialises all method calls. A call
from one Lua task will block (up to 5 s) if another task is mid-transfer on the
same controller. Never share one handle across tasks; each task should call
`spi.new` with the same pins — the reference count allows this.

**Multi-step operations**: Each method call is atomic. Sequences across multiple
calls (e.g. `write` then `read`) are NOT atomic — another task can interleave
between them. If you need atomic multi-step access (e.g. slave preload then master
read), use a single-task design or an external Lua mutex.

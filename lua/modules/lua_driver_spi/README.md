# lua_driver_spi

Lua SPI driver for Ameba RTOS (RTL8721F).

Uses fwlib raw API (`SSI_*`, `GDMA_*`) directly — no HAL layer dependency.

## Hardware

| Port | Role   | MOSI  | MISO  | SCLK  | CS    |
|------|--------|-------|-------|-------|-------|
| SPI0 | Slave  | PA30  | PA31  | PA29  | PB0   |
| SPI1 | Master | PB8   | PB9   | PB7   | PB10  |

For loopback tests, connect SPI1 pins to SPI0 pins as shown above.

## API

### Constructor

```lua
local dev = spi.new(index, mosi, miso, sclk, cs, role [, opts])
```

| Parameter | Type             | Description                          |
|-----------|-----------------|--------------------------------------|
| `index`   | integer (0 or 1) | SPI peripheral index                 |
| `mosi`    | string / integer | MOSI pin, e.g. `"PB8"`               |
| `miso`    | string / integer | MISO pin                             |
| `sclk`    | string / integer | Clock pin                            |
| `cs`      | string / integer | Chip-select pin                      |
| `role`    | integer (0 or 1) | `0` = slave, `1` = master            |
| `opts`    | table (optional) | `{speed=Hz, bits=4-16, mode=0-3}`    |

### Polling methods

```lua
dev:write(data [, timeout_ms])        -- push bytes to TX FIFO; returns bytes written
dev:read(n [, timeout_ms])            -- drain up to n bytes from RX FIFO; returns string
dev:transfer(tx_data [, timeout_ms])  -- full-duplex (master): send + receive; returns string
```

### DMA methods

```lua
dev:recv_dma_start(n)                 -- arm DMA RX for n bytes (non-blocking)
dev:send_dma(data [, timeout_ms])     -- DMA TX, blocking until bus is idle; returns true
dev:recv_dma_finish([timeout_ms])     -- wait for DMA RX done; returns received string
```

### Interrupt methods

```lua
dev:recv_it_start(n)                  -- arm interrupt RX for n bytes (non-blocking)
dev:recv_it_finish([timeout_ms])      -- wait for interrupt RX done; returns received string
```

### Configuration

```lua
dev:set_frequency(hz)        -- change clock rate (requires DISABLE/ENABLE cycle)
dev:set_format(bits, mode)   -- change frame size and SPI mode
dev:close()                  -- release hardware and free index
```

## Test scripts

Load from the REPL with `dofile("vfs:<filename>")` after copying to VFS, or run
via the MCP test flow:

| File                 | Mode      | Description                            |
|----------------------|-----------|----------------------------------------|
| `spi_polling.lua`    | Polling   | Full-duplex master↔slave loopback      |
| `spi_dma.lua`        | DMA       | Master TX DMA + slave RX DMA           |
| `spi_interrupt.lua`  | Interrupt | Master TX polling + slave RX interrupt |

Each script prints `success` on pass, `fail` on mismatch.

## Notes

- Maximum data length per call: 64 bytes (limited by `SPI_XFER_BUF_SIZE`).
- DMA buffers are placed in `.nocache.data` section via `SRAM_NOCACHE_DATA_SECTION`.
- Only one Lua object per SPI index is allowed at a time.
- `set_frequency` / `set_format` briefly disable then re-enable the SPI.
- 16-bit frame mode (`bits > 8`) is supported by the hardware but the test
  scripts use 8-bit frames only.

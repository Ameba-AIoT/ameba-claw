# lua_driver_spi

SPI master/slave driver for RTL8721F.  Raw fwlib (`SSI_*`, `GDMA_*`) — no HAL layer.

## API reference

Parameters verified line-by-line against `src/lua_driver_spi.c`.

### Constructors

```lua
spi.new(idx, sclk, mosi, miso, cs [, opts])        -- master
spi.new_slave(idx, sclk, mosi, miso, cs [, opts])   -- slave
```

| # | Parameter | Type | Notes |
|---|-----------|------|-------|
| 1 | `idx` | integer 0–1 | SPI peripheral index |
| 2 | `sclk` | pin name string | e.g. `"PB_7"` |
| 3 | `mosi` | pin name string | |
| 4 | `miso` | pin name string **or** `nil` | `nil` → skip MISO pinmux (write-only mode) |
| 5 | `cs` | pin name string | Hardware CS (pinmux) |
| 6 | `opts` | table (optional) | `{div=N, cpol=0/1, cpha=0/1}` — div≥2, even; default div=20 → 5 MHz |

### Methods

| Method | Args | Returns | Notes |
|--------|------|---------|-------|
| `write(data)` | string | — | Polling TX. Master: TX+RX simultaneous, RX discarded. Slave: preload TX FIFO. |
| `read(n)` | integer | string | Polling RX. Master: send n dummy bytes, return RX. Slave: drain RX FIFO only. |
| `write_intr(data)` | string | — | ISR TX. Arms TX ISR, blocks until TX FIFO empty. |
| `read_intr(n)` | integer | string | ISR RX. Arms RX ISR, blocks until n bytes received. |
| `write_dma(data)` | string | — | DMA TX. Master: arms TX+RX DMA (RX discarded). Slave: TX DMA only. |
| `read_dma(n)` | integer | string | DMA RX. Master: TX dummy + RX DMA. Slave: RX DMA only. |
| `close()` | — | — | Release hardware ref. Handle becomes invalid. |

Transfer timeout: 5000 ms.  Per-call size limited only by available heap.

## Concurrency & resources

**Lifecycle** (init → operation → deinit): `spi.new`/`spi.new_slave` initialises
the hardware SPI peripheral on first open (reference-counted). Each `close()` decrements
the reference; hardware is powered down when the count reaches zero. Opening the same
controller twice with the same config is safe (ref counted); opening with conflicting
config raises an error.

**GDMA channel lifecycle**: DMA methods (`write_dma`, `read_dma`) allocate GDMA
channels per-transfer via `GDMA_ChnlAlloc` and release them in the ISR completion
callback via `GDMA_ChnlFree`. Channels are never leaked across calls — repeated
DMA calls in a loop are safe even with the 8-channel hardware limit.

**Mutex**: A per-controller FreeRTOS mutex serialises all method calls. Concurrent
calls from different tasks on the same controller are serialised (one blocks up to 5 s).

**Handle sharing**: Do NOT share one handle between tasks. Each task should call
`spi.new` independently — the ref-count allows multiple opens with the same config.

## Wiring (loopback on one board)

```
SPI1 master          SPI0 slave
---------            ----------
PB_7  (SCLK)  <-->  PA_29 (SCLK)
PB_8  (MOSI)  <-->  PA_30 (MOSI)
PB_9  (MISO)  <-->  PA_31 (MISO)
PB_10 (CS)    <-->  PB_0  (CS)
GND           ---   GND
```

ST7789 LCD wiring (write-only, no loopback):

```
RTL8721F pin   ST7789 pin
------------   ----------
PB_7           SCL / CLK
PB_8           SDA / MOSI
PB_10          CS
PB_9           DC (GPIO, not SPI)
PA_25          RES (GPIO reset, active low)
PA_26          BLK (GPIO backlight, active high)
3.3 V          VCC
GND            GND
```

## AT commands

```
AT+CLAW=spi,poll    -- Polling: write/read loopback + write_intr/read_intr
AT+CLAW=spi,intr    -- ISR: Part A two-task master->slave, Part B single-task slave->master
AT+CLAW=spi,dma     -- DMA: Part A two-task 256 B, Part B single-task preload
AT+CLAW=spi,st7789  -- ST7789: colour fills + vertical bars ↔ concentric rects (10 rounds)
```

## Test cases

| ID | AT command | What it verifies |
|----|------------|-----------------|
| poll-1 | `spi,poll` | Master `write` → slave `read` (16 bytes, data integrity) |
| poll-2 | `spi,poll` | Slave `write` (preload) → master `read` (8 bytes, data integrity) |
| poll-3 | `spi,poll` | Slave `write_intr` (preload ISR) → master `read_intr` (8 bytes) |
| poll-4 | `spi,poll` | `close()` + re-open: verifies hardware is fully released and reusable |
| intr-A | `spi,intr` | Two-task: master `write_intr` + slave `read_intr` (16 bytes, master→slave) |
| intr-B | `spi,intr` | Single-task: slave `write_intr` preload + master `read_intr` (16 bytes, slave→master) |
| dma-A  | `spi,dma`  | Two-task: master `write_dma` + slave `read_dma` (256 bytes, pattern check) |
| dma-B  | `spi,dma`  | Single-task: slave `write_dma` preload + master `read_dma` (8 bytes) |
| st7789 | `spi,st7789` | ST7789 init + vertical bars ↔ concentric rects (10 rounds) |

All tests print `PASS` on success, `FAIL: <error>` on failure.

## Standalone test scripts (single-task, run via dofile)

| File | What it tests |
|------|--------------|
| `test_spi_polling.lua` | Polling write+read (both directions), write_intr+read_intr, close/reopen; full data compare |
| `test_spi_interrupt.lua` | Slave write_intr preload → master read_intr; byte-level data compare |
| `test_spi_dma.lua` | Slave write_dma preload → master read_dma (64 bytes); byte-level data compare |
| `test_spi_st7789.lua` | ST7789 bars/concentric switching (10 rounds); run via AT+CLAW=spi,st7789 (requires LCD hardware) |

Note: the master→slave direction for intr and DMA requires two tasks and is covered
by the AT+CLAW commands. The standalone scripts cover the slave-preload direction only.

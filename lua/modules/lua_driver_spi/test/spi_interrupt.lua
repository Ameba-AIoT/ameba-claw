-- SPI Interrupt Mode Test
--
-- Hardware: SPI1 (master) connected to SPI0 (slave)
-- Wiring:
--   PB7  (SPI1_SCLK) --> PA29 (SPI0_SCLK)
--   PB8  (SPI1_MOSI) --> PA30 (SPI0_MOSI)
--   PB9  (SPI1_MISO) <-- PA31 (SPI0_MISO)
--   PB10 (SPI1_CS)   --> PB0  (SPI0_CS)
--
-- Master TX via polling; slave RX via interrupt. 256-byte transfer.
-- Run from REPL: dofile("vfs:spi_interrupt.lua")

local spi = require("spi")

print("[SPI Interrupt] test start (256 bytes)")

local master = spi.new(1, "PB_8", "PB_9", "PB_7", "PB_10", 1, {speed = 1000000, bits = 8, mode = 0})
local slave  = spi.new(0, "PA_30", "PA_31", "PA_29", "PB_0",  0, {speed = 1000000, bits = 8, mode = 0})

-- 256-byte ascending pattern: 0x00 .. 0xFF
local tx_data = {}
for i = 1, 256 do
    tx_data[i] = (i - 1) % 256
end

-- Arm slave interrupt RX before master sends (non-blocking)
slave:recv_it_start(256)

-- Master writes to TX FIFO; hardware drives SCLK, slave ISR captures bytes
master:write(tx_data, 2000)

-- Wait for slave interrupt handler to collect all 256 bytes
local slave_rx = slave:recv_it_finish(2000)

local fail_cnt = 0
for i = 1, 256 do
    local s = slave_rx:byte(i) or 0
    if s ~= tx_data[i] then
        fail_cnt = fail_cnt + 1
        if fail_cnt <= 4 then
            print(string.format("[FAIL] slave rx[%d]: got 0x%02X, expected 0x%02X", i, s, tx_data[i]))
        end
    end
end

master:close()
slave:close()

if fail_cnt == 0 then
    print("[SPI Interrupt] success (256 bytes)")
else
    print(string.format("[SPI Interrupt] fail (%d/256 bytes mismatched)", fail_cnt))
end

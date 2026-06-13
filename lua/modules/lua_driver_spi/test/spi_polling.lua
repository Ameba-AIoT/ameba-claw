-- SPI Polling Mode Test
--
-- Hardware: SPI1 (master) connected to SPI0 (slave)
-- Wiring:
--   PB7  (SPI1_SCLK) --> PA29 (SPI0_SCLK)
--   PB8  (SPI1_MOSI) --> PA30 (SPI0_MOSI)
--   PB9  (SPI1_MISO) <-- PA31 (SPI0_MISO)
--   PB10 (SPI1_CS)   --> PB0  (SPI0_CS)
--
-- Run from REPL: dofile("vfs:spi_polling.lua")

local spi = require("spi")
local sys = require("sys")

print("[SPI Polling] test start")

local master = spi.new(1, "PB_8", "PB_9", "PB_7", "PB_10", 1, {speed = 1000000, bits = 8, mode = 0})
local slave  = spi.new(0, "PA_30", "PA_31", "PA_29", "PB_0",  0, {speed = 1000000, bits = 8, mode = 0})

local master_tx = {0xAA, 0xBB, 0xCC, 0xDD}
local slave_tx  = {0x11, 0x22, 0x33, 0x44}

-- Pre-load slave TX FIFO with the response it will clock out to master
slave:write(slave_tx)

-- Master drives SCLK, full-duplex: sends master_tx, receives slave_tx
local master_rx = master:transfer(master_tx, 1000)

-- Slave RX FIFO now holds what master sent; drain it
local slave_rx = slave:read(4, 100)

local ok = true

for i = 1, 4 do
    local m = master_rx:byte(i) or 0
    local s = slave_rx:byte(i) or 0
    if m ~= slave_tx[i] then
        ok = false
        print(string.format("[FAIL] master rx[%d]: got 0x%02X, expected 0x%02X", i, m, slave_tx[i]))
    end
    if s ~= master_tx[i] then
        ok = false
        print(string.format("[FAIL] slave rx[%d]: got 0x%02X, expected 0x%02X", i, s, master_tx[i]))
    end
end

master:close()
slave:close()

if ok then
    print("[SPI Polling] success")
else
    print("[SPI Polling] fail")
end

-- SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
-- SPDX-License-Identifier: Apache-2.0

-- test_spi_dma.lua — SPI DMA loopback test (single task).
-- Tests slave:write_dma preload → master:read_dma with byte-level data comparison.
-- The reverse direction (master:write_dma → slave:read_dma) requires two tasks
-- and is covered by AT+CLAW=spi,dma Part A.
--
-- Hardware (RTL8721F, same-board loopback):
--   PB_7  (SPI1 SCLK) <-> PA_29 (SPI0 SCLK)
--   PB_8  (SPI1 MOSI) <-> PA_30 (SPI0 MOSI)
--   PB_9  (SPI1 MISO) <-> PA_31 (SPI0 MISO)
--   PB_10 (SPI1 CS)   <-> PB_0  (SPI0 CS)
--
-- Run via: dofile("vfs:test_spi_dma.lua")

-- Build 64-byte ascending pattern string
local parts = {}
for i = 1, 64 do parts[i] = string.char((i - 1) % 256) end
local SLAVE_TX = table.concat(parts)

local m = spi.new(1, "PB_7", "PB_8", "PB_9", "PB_10")
local s = spi.new_slave(0, "PA_29", "PA_30", "PA_31", "PB_0")

local ok, err = pcall(function()

    -- Case 1: slave write_dma preload → master read_dma (64 bytes); byte-level compare
    s:write_dma(SLAVE_TX)
    local rx = m:read_dma(#SLAVE_TX)
    local fail = 0
    for i = 1, #SLAVE_TX do
        if rx:byte(i) ~= SLAVE_TX:byte(i) then
            fail = fail + 1
            if fail <= 4 then
                print(string.format("[dma] byte[%d]: got 0x%02X expected 0x%02X",
                                    i, rx:byte(i), SLAVE_TX:byte(i)))
            end
        end
    end
    assert(fail == 0,
        string.format("case1 mismatch: %d/%d bytes wrong", fail, #SLAVE_TX))
    print(string.format("[dma] 1. slave:write_dma + master:read_dma OK (%d bytes)", #SLAVE_TX))

    m:close()
    s:close()
    print("[dma] 2. close OK")
end)

if s then pcall(function() s:close() end) end
if m then pcall(function() m:close() end) end

if ok then
    print("[test_spi_dma] PASS")
else
    print("[test_spi_dma] FAIL: " .. tostring(err))
end

-- SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
-- SPDX-License-Identifier: Apache-2.0

-- test_spi_interrupt.lua — SPI ISR loopback test (single task).
-- Tests slave:write_intr preload → master:read_intr with data comparison.
-- The reverse direction (master:write_intr → slave:read_intr) requires two tasks
-- and is covered by AT+CLAW=spi,intr.
--
-- Hardware (RTL8721F, same-board loopback):
--   PB_7  (SPI1 SCLK) <-> PA_29 (SPI0 SCLK)
--   PB_8  (SPI1 MOSI) <-> PA_30 (SPI0 MOSI)
--   PB_9  (SPI1 MISO) <-> PA_31 (SPI0 MISO)
--   PB_10 (SPI1 CS)   <-> PB_0  (SPI0 CS)
--
-- Run via: dofile("vfs:test_spi_interrupt.lua")

local SLAVE_TX = "\xB1\xB2\xB3\xB4\xB5\xB6\xB7\xB8\xB9\xBA\xBB\xBC\xBD\xBE\xBF\xC0"

local m = spi.new(1, "PB_7", "PB_8", "PB_9", "PB_10")
local s = spi.new_slave(0, "PA_29", "PA_30", "PA_31", "PB_0")

local ok, err = pcall(function()

    -- Case 1: slave write_intr preload → master read_intr; compare with SLAVE_TX
    s:write_intr(SLAVE_TX)
    local rx = m:read_intr(#SLAVE_TX)
    local fail = 0
    for i = 1, #SLAVE_TX do
        if rx:byte(i) ~= SLAVE_TX:byte(i) then
            fail = fail + 1
        end
    end
    assert(fail == 0,
        string.format("case1 mismatch: %d/%d bytes wrong", fail, #SLAVE_TX))
    print("[intr] 1. slave:write_intr + master:read_intr OK")

    m:close()
    s:close()
    print("[intr] 2. close OK")
end)

if s then pcall(function() s:close() end) end
if m then pcall(function() m:close() end) end

if ok then
    print("[test_spi_interrupt] PASS")
else
    print("[test_spi_interrupt] FAIL: " .. tostring(err))
end

-- SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
-- SPDX-License-Identifier: Apache-2.0

-- test_spi_polling.lua — SPI polling + ISR loopback test (single task).
-- Tests: write/read (polling), slave-preload write/read, write_intr/read_intr,
--        and close/reopen (resource release).
-- All directions that complete RX verify the received bytes against the transmitted bytes.
--
-- Hardware (RTL8721F, same-board loopback):
--   PB_7  (SPI1 SCLK) <-> PA_29 (SPI0 SCLK)
--   PB_8  (SPI1 MOSI) <-> PA_30 (SPI0 MOSI)
--   PB_9  (SPI1 MISO) <-> PA_31 (SPI0 MISO)
--   PB_10 (SPI1 CS)   <-> PB_0  (SPI0 CS)
--
-- Run via: dofile("vfs:test_spi_polling.lua")

local MASTER_TX = "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A\x0B\x0C\x0D\x0E\x0F\x10"
local SLAVE_TX  = "\xA1\xA2\xA3\xA4\xA5\xA6\xA7\xA8"

local m = spi.new(1, "PB_7", "PB_8", "PB_9", "PB_10")
local s = spi.new_slave(0, "PA_29", "PA_30", "PA_31", "PB_0")

local ok, err = pcall(function()

    -- Case 1: master write → slave read; compare slave_rx with MASTER_TX
    m:write(MASTER_TX)
    local rx1 = s:read(#MASTER_TX)
    assert(rx1 == MASTER_TX,
        string.format("case1 mismatch: got %d bytes, byte[1]=0x%02X expected 0x%02X",
                      #rx1, rx1:byte(1) or 0, MASTER_TX:byte(1)))
    print("[polling] 1. master:write + slave:read OK")

    -- Case 2: slave write preload → master read; compare master_rx with SLAVE_TX
    s:write(SLAVE_TX)
    local rx2 = m:read(#SLAVE_TX)
    assert(rx2 == SLAVE_TX,
        string.format("case2 mismatch: got %d bytes, byte[1]=0x%02X expected 0x%02X",
                      #rx2, rx2:byte(1) or 0, SLAVE_TX:byte(1)))
    print("[polling] 2. slave:write + master:read OK")

    -- Case 3: slave write_intr preload → master read_intr; compare with SLAVE_TX
    s:write_intr(SLAVE_TX)
    local rx3 = m:read_intr(#SLAVE_TX)
    assert(rx3 == SLAVE_TX,
        string.format("case3 mismatch: got %d bytes, byte[1]=0x%02X expected 0x%02X",
                      #rx3, rx3:byte(1) or 0, SLAVE_TX:byte(1)))
    print("[polling] 3. slave:write_intr + master:read_intr OK")

    -- Case 4: close + reopen (verifies hardware is fully released and reusable)
    m:close()
    s:close()
    local m2 = spi.new(1, "PB_7", "PB_8", "PB_9", "PB_10")
    m2:close()
    print("[polling] 4. close/reopen OK")
end)

if s then pcall(function() s:close() end) end
if m then pcall(function() m:close() end) end

if ok then
    print("[test_spi_polling] PASS")
else
    print("[test_spi_polling] FAIL: " .. tostring(err))
end

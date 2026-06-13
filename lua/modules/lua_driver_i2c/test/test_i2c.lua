-- I2C bus+device model test (standalone reference script).
-- Hardware: I2C0, SDA=PB_15, SCL=PB_16 (no external device required for basic test)
-- Not wired to an AT command; load and run this chunk directly in a Lua state.
-- For the on-target tests use AT+CLAW=i2c,<sh1106|rw|slave>.

local ok, err = pcall(function()
    -- Init bus at 100 kHz
    local bus = i2c.new(0, "PB_15", "PB_16", 100000)

    -- Scan: empty result is acceptable when no device is connected
    local found = bus:scan()
    local n = #found
    if n > 0 then
        local addrs = {}
        for i = 1, n do
            addrs[i] = string.format("0x%02X", found[i])
        end
        print("scan found: " .. table.concat(addrs, ", "))
    else
        print("scan: no devices found (bus functional)")
    end

    -- Create a device object and verify address() returns the bound address
    local dev = bus:device(0x50)
    assert(dev:address() == 0x50, "address() mismatch")

    -- Close device then bus
    dev:close()
    bus:close()

    -- Re-init to verify resources were released cleanly
    local bus2 = i2c.new(0, "PB_15", "PB_16", 100000)
    local dev2 = bus2:device(0x48)
    assert(dev2:address() == 0x48, "re-init address() mismatch")
    dev2:close()
    bus2:close()
end)

if ok then
    print("success")
else
    print("FAIL: " .. tostring(err))
end

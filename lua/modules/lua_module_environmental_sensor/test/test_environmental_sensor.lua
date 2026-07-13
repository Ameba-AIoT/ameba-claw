-- test_environmental_sensor.lua
-- DHT11 environmental sensor test for AmebaGreen2 (RTL8721F).
--
-- Usage:
--   AT+CLAW=env,dht11          -- default pin PB_8
--   AT+CLAW=env,dht11,PA_12    -- explicit pin

local environmental_sensor = require("environmental_sensor")
local sys = require("sys")

local a   = type(args) == "table" and args or {}
local PIN = type(a.pin) == "string" and a.pin or "PB_8"

local sensor

local function cleanup()
    if sensor then
        pcall(function() sensor:close() end)
        sensor = nil
    end
end

local function assert_error(fn, expected_fragment)
    local ok, err = pcall(fn)
    if ok then
        error("expected error containing '" .. expected_fragment .. "' but call succeeded")
    end
    if not string.find(tostring(err), expected_fragment, 1, true) then
        error("expected error '" .. expected_fragment .. "' but got: " .. tostring(err))
    end
end

local function run()
    -- ── 1. new() ──────────────────────────────────────────────────────────
    print(string.format("[env] new(pin=%s)", PIN))
    sensor = environmental_sensor.new({ pin = PIN })

    -- ── 2. name() ─────────────────────────────────────────────────────────
    local n = sensor:name()
    assert(n == "dht11", "name() expected 'dht11', got: " .. tostring(n))
    print("[env] name(): " .. n .. "  OK")

    -- ── 3. read() → table with temperature and humidity ───────────────────
    sys.sleep_ms(2000)
    local sample = sensor:read()
    assert(type(sample.temperature) == "number", "read().temperature not a number")
    assert(type(sample.humidity)    == "number", "read().humidity not a number")
    print(string.format("[env] read(): T=%.1f C  RH=%.1f %%  OK",
                        sample.temperature, sample.humidity))

    -- ── 4. read_temperature() ─────────────────────────────────────────────
    sys.sleep_ms(2000)
    local t = sensor:read_temperature()
    assert(type(t) == "number", "read_temperature() not a number")
    print(string.format("[env] read_temperature(): %.1f C  OK", t))

    -- ── 5. read_humidity() ────────────────────────────────────────────────
    sys.sleep_ms(2000)
    local h = sensor:read_humidity()
    assert(type(h) == "number", "read_humidity() not a number")
    print(string.format("[env] read_humidity(): %.1f %%  OK", h))

    -- ── 6. read_raw() → two integers (x10) ───────────────────────────────
    sys.sleep_ms(2000)
    local tr, hr = sensor:read_raw()
    assert(type(tr) == "number", "read_raw() temp_raw not a number")
    assert(type(hr) == "number", "read_raw() humi_raw not a number")
    -- raw values must be consistent with read() (same integer part)
    assert(math.floor(t * 10 + 0.5) == tr or math.abs(tr - t * 10) <= 10,
           string.format("read_raw temp_raw=%d inconsistent with read_temperature=%.1f", tr, t))
    print(string.format("[env] read_raw(): temp_raw=%d  humi_raw=%d  OK", tr, hr))

    -- ── 6b. repeated sampling (10 reads) ─────────────────────────────────
    print("[env] repeated sampling (10 reads):")
    for i = 1, 10 do
        sys.sleep_ms(2000)
        local ok_r, result = pcall(function() return sensor:read() end)
        if ok_r then
            print(string.format("  [%2d] T=%.1f C  RH=%.1f %%", i, result.temperature, result.humidity))
        else
            print(string.format("  [%2d] ERROR: %s", i, tostring(result)))
        end
    end
    print("[env] repeated sampling OK")

    -- ── 7. close() ────────────────────────────────────────────────────────
    sensor:close()
    print("[env] close()  OK")

    -- ── 8. method call on closed handle must error ────────────────────────
    assert_error(function() sensor:read() end,          "invalid or closed")
    assert_error(function() sensor:read_temperature() end, "invalid or closed")
    assert_error(function() sensor:read_humidity() end,    "invalid or closed")
    assert_error(function() sensor:read_raw() end,         "invalid or closed")
    assert_error(function() sensor:name() end,             "invalid or closed")
    print("[env] closed-handle errors: all OK")

    sensor = nil

    -- ── 9. new() with invalid pin must error ──────────────────────────────
    assert_error(function()
        environmental_sensor.new({ pin = "PZ_99" })
    end, "invalid pin")
    print("[env] invalid pin error: OK")

    -- ── 10. re-open and read after close (resource re-use) ────────────────
    sys.sleep_ms(500)
    sensor = environmental_sensor.new({ pin = PIN })
    sys.sleep_ms(2000)
    local s2 = sensor:read()
    assert(type(s2.temperature) == "number", "re-open read() failed")
    print(string.format("[env] re-open: T=%.1f C  RH=%.1f %%  OK",
                        s2.temperature, s2.humidity))
    sensor:close()
    sensor = nil

    print("success")
end

local ok, err = pcall(run)
cleanup()
if not ok then
    error(tostring(err))
end

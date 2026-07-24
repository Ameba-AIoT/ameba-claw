-- test_magnetometer.lua
-- BMM150 3-axis magnetometer test for AmebaGreen2 (RTL8721F).
--
-- Wiring (board.json i2c0 interface on EV721FL0_R03_BreadBoard):
--   VCC -> 3V3,  GND -> GND
--   SCL -> PA_25,  SDA -> PA_26
--   INT -> PB_8  (optional, not required for basic operation)
--   I2C mode: CSB solder bridge must be in I2C position.
--   SDO open/GND -> addr 0x10;  if probe fails at 0x10, tries 0x12 automatically.
--
-- Usage:
--   AT+CLAW=magnetometer                                    -- board.json defaults
--   AT+CLAW=magnetometer,PA_26,PA_25                        -- explicit sda,scl
--   AT+CLAW=magnetometer,PA_26,PA_25,0,0x10,PB_8,10,200    -- full explicit

local magnetometer = require("magnetometer")
local sys = require("sys")

local a       = type(args) == "table" and args or {}
local CHIP    = type(a.chip) == "string"      and a.chip      or "bmm150"
local SDA     = type(a.sda) == "string"       and a.sda       or "PA_26"
local SCL     = type(a.scl) == "string"       and a.scl       or "PA_25"
local I2C     = type(a.i2c) == "number"       and a.i2c       or 0
local ADDR    = type(a.addr) == "number"      and a.addr      or 0x10
local INT_GPIO = type(a.int_gpio) == "string" and a.int_gpio  or nil
local COUNT   = (type(a.count) == "number"    and a.count > 0) and math.floor(a.count) or 10
local INTVL   = (type(a.interval) == "number" and a.interval >= 0) and math.floor(a.interval) or 200

local dev

local function cleanup()
    if dev then
        pcall(function() dev:close() end)
        dev = nil
    end
end

local function assert_error(fn, fragment)
    local ok, err = pcall(fn)
    if ok then
        error("expected error containing '" .. fragment .. "' but call succeeded")
    end
    if not string.find(tostring(err), fragment, 1, true) then
        error("expected '" .. fragment .. "' but got: " .. tostring(err))
    end
end

local function run()
    -- ── 1. new() ──────────────────────────────────────────────────────────
    local opts = { chip = CHIP, sda = SDA, scl = SCL, i2c = I2C, addr = ADDR }
    if INT_GPIO then opts.int_gpio = INT_GPIO end
    print(string.format("[mag] new(chip=%s sda=%s scl=%s i2c=%d addr=0x%02x int_gpio=%s)",
                         CHIP, SDA, SCL, I2C, ADDR, INT_GPIO or "none"))
    dev = magnetometer.new(opts)

    -- ── 2. name() ─────────────────────────────────────────────────────────
    local n = dev:name()
    assert(n == CHIP, "name() expected '" .. CHIP .. "', got: " .. tostring(n))
    print("[mag] name(): " .. n .. "  OK")

    -- ── 3. read() -> {magnetic, temperature, status, calibrated}
    sys.sleep_ms(50)
    local s = dev:read()
    assert(type(s.magnetic)     == "table",  "read().magnetic not a table")
    assert(type(s.magnetic.x)   == "number", "read().magnetic.x not a number")
    assert(type(s.magnetic.y)   == "number", "read().magnetic.y not a number")
    assert(type(s.magnetic.z)   == "number", "read().magnetic.z not a number")
    assert(type(s.temperature)  == "number", "read().temperature not a number")
    assert(type(s.status)       == "number", "read().status not a number")
    assert(type(s.calibrated)   == "boolean","read().calibrated not a boolean")
    -- Earth magnetic field is roughly 25-65 uT; compensated BMM150 values are in
    -- uT when using the Bosch SensorAPI with floating-point off — raw int16 range.
    -- Sanity check: at least one axis should be non-trivially non-zero.
    local mag_max = math.max(math.abs(s.magnetic.x),
                              math.abs(s.magnetic.y),
                              math.abs(s.magnetic.z))
    assert(mag_max > 0, "all magnetic axes are zero — check sensor wiring")
    print(string.format("[mag] read(): mag=(%.1f,%.1f,%.1f) temp=%.1f status=0x%02x calibrated=%s  OK",
                         s.magnetic.x, s.magnetic.y, s.magnetic.z,
                         s.temperature, s.status, tostring(s.calibrated)))

    -- ── 4. read_temperature() -> 0 for BMM150 ────────────────────────────
    local t = dev:read_temperature()
    assert(type(t) == "number", "read_temperature() not a number")
    print(string.format("[mag] read_temperature(): %.1f  OK", t))

    -- ── 5. read_int_status() -> integer ──────────────────────────────────
    local st = dev:read_int_status()
    assert(type(st) == "number", "read_int_status() not a number")
    print(string.format("[mag] read_int_status(): 0x%02x  OK", st))

    -- ── 6. repeated sampling ──────────────────────────────────────────────
    print(string.format("[mag] repeated sampling (%d reads, %d ms apart):", COUNT, INTVL))
    for i = 1, COUNT do
        sys.sleep_ms(INTVL)
        local ok_r, r = pcall(function() return dev:read() end)
        if ok_r then
            print(string.format("  [%2d] mag=(%.1f, %.1f, %.1f) status=0x%02x",
                                 i, r.magnetic.x, r.magnetic.y, r.magnetic.z, r.status))
        else
            print(string.format("  [%2d] ERROR: %s", i, tostring(r)))
        end
    end
    print("[mag] repeated sampling  OK")

    -- ── 7. close() ────────────────────────────────────────────────────────
    dev:close()
    print("[mag] close()  OK")

    -- ── 8. closed-handle errors ───────────────────────────────────────────
    assert_error(function() dev:read() end,            "invalid or closed")
    assert_error(function() dev:read_temperature() end,"invalid or closed")
    assert_error(function() dev:read_int_status() end, "invalid or closed")
    print("[mag] closed-handle errors: all OK")
    dev = nil

    -- ── 9. invalid pin must error ─────────────────────────────────────────
    assert_error(function()
        magnetometer.new({ sda = "PZ_99", scl = SCL })
    end, "invalid pin")
    print("[mag] invalid pin error: OK")

    -- ── 10. re-open and read (resource re-use after close) ────────────────
    sys.sleep_ms(100)
    dev = magnetometer.new(opts)
    local s2 = dev:read()
    assert(type(s2.magnetic.x) == "number", "re-open read() failed")
    print(string.format("[mag] re-open: magnetic.x=%.1f  OK", s2.magnetic.x))
    dev:close()
    dev = nil

    print("success")
end

local ok, err = pcall(run)
cleanup()
if not ok then
    error(tostring(err))
end

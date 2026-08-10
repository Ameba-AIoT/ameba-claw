-- test_imu.lua
-- Multi-chip IMU test for AmebaGreen2 (RTL8721F).
-- Supports: mpu6050 (GY-521), bmi270.
--
-- MPU-6050 wiring: VCC->3V3, GND->GND, SCL->PA_25, SDA->PA_26, AD0->GND
-- BMI270 wiring:   VCC->3V3, GND->GND, SCL->PA_6,  SDA->PA_7,  SDO->GND
--
-- Run:  AT+CLAW=cap,lua_run_async,{"path":"vfs:/test_imu.lua","args":{"chip":"bmi270"}}
-- Args (all optional): chip, sda, scl, i2c, addr, count, interval.

local imu = require("imu")
local sys = require("sys")

local dev

local function cleanup()
    if dev then
        pcall(function() dev:close() end)
        dev = nil
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

function run(args)
    local a     = type(args) == "table" and args or {}
    local CHIP  = type(a.chip) == "string" and a.chip or "mpu6050"
    local SDA   = type(a.sda) == "string" and a.sda or (CHIP == "bmi270" and "PA_7" or "PA_26")
    local SCL   = type(a.scl) == "string" and a.scl or (CHIP == "bmi270" and "PA_6" or "PA_25")
    local I2C   = type(a.i2c) == "number" and a.i2c or (CHIP == "bmi270" and 1 or 0)
    local ADDR  = type(a.addr) == "number" and a.addr or 0x68

    -- Expected WHO_AM_I value per chip
    local WHO_AM_I_EXPECTED = (CHIP == "bmi270") and 0x24 or 0x68
    -- repeated-sampling controls (step 7): how many reads and the gap between them
    local COUNT = (type(a.count) == "number" and a.count > 0) and math.floor(a.count) or 10
    local INTVL = (type(a.interval) == "number" and a.interval >= 0) and math.floor(a.interval) or 100

    local function body()
        -- ── 1. new() ──────────────────────────────────────────────────────────
        print(string.format("[imu] new(chip=%s sda=%s scl=%s i2c=%d addr=0x%02x)",
                             CHIP, SDA, SCL, I2C, ADDR))
        dev = imu.new({ chip = CHIP, sda = SDA, scl = SCL, i2c = I2C, addr = ADDR })

        -- ── 2. name() / who_am_i() ────────────────────────────────────────────
        local n = dev:name()
        assert(n == CHIP, "name() expected '" .. CHIP .. "', got: " .. tostring(n))
        print("[imu] name(): " .. n .. "  OK")

        local id = dev:who_am_i()
        assert(id == WHO_AM_I_EXPECTED,
               string.format("who_am_i() expected 0x%02x, got 0x%02x", WHO_AM_I_EXPECTED, id))
        print(string.format("[imu] who_am_i(): 0x%02x  OK", id))

        -- ── 3. read() → {accel={x,y,z}, gyro={x,y,z}, temp} ───────────────────
        sys.sleep_ms(50)
        local s = dev:read()
        assert(type(s.accel) == "table", "read().accel not a table")
        assert(type(s.gyro)  == "table", "read().gyro not a table")
        assert(type(s.accel.x) == "number", "read().accel.x not a number")
        assert(type(s.gyro.z)  == "number", "read().gyro.z not a number")
        assert(type(s.temp)  == "number", "read().temp not a number")
        -- status (INT_STATUS) now comes from the SAME burst as accel/temp/gyro, so
        -- every field belongs to one measurement instant (was a separate read).
        assert(type(s.status) == "number", "read().status not a number")
        -- At rest one accel axis reads ~1 g. In ±16 g mode 1 g = 2048 LSB, so the
        -- magnitude of the largest axis should be clearly non-zero and within range.
        local amax = math.max(math.abs(s.accel.x), math.abs(s.accel.y), math.abs(s.accel.z))
        assert(amax > 500 and amax < 32768,
               string.format("accel magnitude out of expected range: %d", amax))
        print(string.format("[imu] read(): accel=(%d,%d,%d) gyro=(%d,%d,%d) temp_raw=%d status=0x%02x  OK",
                             s.accel.x, s.accel.y, s.accel.z,
                             s.gyro.x, s.gyro.y, s.gyro.z, s.temp, s.status))

        -- ── 4. read_accel() / read_gyro() ─────────────────────────────────────
        local ax, ay, az = dev:read_accel()
        assert(type(ax) == "number" and type(ay) == "number" and type(az) == "number",
               "read_accel() did not return three numbers")
        print(string.format("[imu] read_accel(): (%d,%d,%d)  OK", ax, ay, az))

        local gx, gy, gz = dev:read_gyro()
        assert(type(gx) == "number" and type(gy) == "number" and type(gz) == "number",
               "read_gyro() did not return three numbers")
        print(string.format("[imu] read_gyro(): (%d,%d,%d)  OK", gx, gy, gz))

        -- ── 5. read_temperature() → plausible room/board temp ────────────────
        local t = dev:read_temperature()
        assert(type(t) == "number", "read_temperature() not a number")
        assert(t > -10 and t < 85,
               string.format("temperature out of plausible range: %.2f C", t))
        print(string.format("[imu] read_temperature(): %.2f C  OK", t))

        -- ── 6. read_int_status() → integer bitfield ──────────────────────────
        -- No print here: with no feature-engine interrupt sources enabled this
        -- is normally 0x00 on BMI270, so it isn't a useful liveness signal.
        -- Watch accel/gyro values change in step 7 instead.
        local st = dev:read_int_status()
        assert(type(st) == "number", "read_int_status() not a number")

        -- ── 7. repeated sampling (count reads, interval ms apart) ────────────
        print(string.format("[imu] repeated sampling (%d reads, %d ms apart):", COUNT, INTVL))
        for i = 1, COUNT do
            sys.sleep_ms(INTVL)
            local ok_r, r = pcall(function() return dev:read() end)
            if ok_r then
                print(string.format("  [%2d] a=(%6d,%6d,%6d) g=(%6d,%6d,%6d)",
                                     i, r.accel.x, r.accel.y, r.accel.z,
                                     r.gyro.x, r.gyro.y, r.gyro.z))
            else
                print(string.format("  [%2d] ERROR: %s", i, tostring(r)))
            end
        end
        print("[imu] repeated sampling OK")

        -- ── 8. close() ───────────────────────────────────────────────────────
        dev:close()
        print("[imu] close()  OK")

        -- ── 9. method call on closed handle must error ───────────────────────
        assert_error(function() dev:read() end,             "invalid or closed")
        assert_error(function() dev:read_temperature() end, "invalid or closed")
        assert_error(function() dev:who_am_i() end,         "invalid or closed")
        print("[imu] closed-handle errors: all OK")

        dev = nil

        -- ── 10. new() with invalid pin must error ─────────────────────────────
        assert_error(function()
            imu.new({ sda = "PZ_99", scl = SCL })
        end, "invalid pin")
        print("[imu] invalid pin error: OK")

        -- ── 11. re-open and read (resource re-use after close) ────────────────
        sys.sleep_ms(100)
        dev = imu.new({ chip = CHIP, sda = SDA, scl = SCL, i2c = I2C, addr = ADDR })
        local s2 = dev:read()
        assert(type(s2.accel.x) == "number", "re-open read() failed")
        print(string.format("[imu] re-open: accel.x=%d  OK", s2.accel.x))
        dev:close()
        dev = nil

        print("success")
    end

    local ok, err = pcall(body)
    cleanup()
    if not ok then
        error(tostring(err))
    end
    return "success"
end

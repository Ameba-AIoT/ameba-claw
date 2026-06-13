-- test_thermal.lua: Thermal sensor read test.
-- No external wiring needed — reads on-chip temperature sensor.
-- Run via: AT+CLAW=thermal

local thermal = require("thermal")
local sys     = require("sys")

local function main()
    print("[thermal_test] init thermal sensor")
    local ok, thm = pcall(thermal.new)
    if not ok then
        print("[thermal_test] FAIL: thermal.new() error: " .. tostring(thm))
        return
    end

    local ok0, t0 = pcall(thm.power_on_temp, thm)
    if not ok0 then
        print("[thermal_test] FAIL: power_on_temp error: " .. tostring(t0))
        thm:close()
        return
    end
    print(string.format("[thermal_test] power-on temp: %.3f °C", t0))

    local all_ok = true
    for i = 1, 5 do
        sys.sleep_ms(200)
        local ok_r, t = pcall(thm.read, thm)
        if not ok_r then
            print("[thermal_test] FAIL: read error: " .. tostring(t))
            all_ok = false
            break
        end
        local ok_f, tf = pcall(thm.read_f, thm)
        local f_str = ok_f and string.format("  (%.3f °F)", tf) or ""
        print(string.format("[thermal_test] reading %d: %.3f °C%s", i, t, f_str))
    end

    if all_ok then
        local ok_max, tmax = pcall(thm.max_temp, thm)
        local ok_min, tmin = pcall(thm.min_temp, thm)
        if ok_max and ok_min then
            print(string.format("[thermal_test] max: %.3f  min: %.3f", tmax, tmin))
        end
    end

    thm:close()

    if all_ok then
        print("[thermal_test] success")
    else
        print("[thermal_test] FAIL")
    end
end

local ok, err = pcall(main)
if not ok then
    print("[thermal_test] ERROR: " .. tostring(err))
end

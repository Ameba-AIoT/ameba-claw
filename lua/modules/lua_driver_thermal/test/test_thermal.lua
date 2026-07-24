-- test_thermal.lua: Thermal sensor read test.
-- No external wiring needed -- reads on-chip temperature sensor.
-- Run via: AT+CLAW=thermal[,<count>[,<interval_ms>]]
--   count       : number of temperature readings (default 5)
--   interval_ms : delay between readings in ms   (default 200)

local thermal = require("thermal")
local sys     = require("sys")

args = args or {}
local count       = (args.count       and args.count       > 0) and args.count       or 5
local interval_ms = (args.interval_ms and args.interval_ms > 0) and args.interval_ms or 200

local function main()
    print("[thermal_test] init thermal sensor")
    local ok, thm = pcall(thermal.new)
    if not ok then
        print("[thermal_test] FAIL: thermal.new() error: " .. tostring(thm))
        return
    end

    -- power-on temperature
    local ok0, t0 = pcall(thm.power_on_temp, thm)
    if not ok0 then
        print("[thermal_test] FAIL: power_on_temp error: " .. tostring(t0))
        thm:close()
        return
    end
    print(string.format("[thermal_test] power-on temp: %.3f °C", t0))

    -- repeated read (Celsius + Fahrenheit)
    local all_ok = true
    for i = 1, count do
        sys.sleep_ms(interval_ms)
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

    -- max / min stat
    if all_ok then
        local ok_max, tmax = pcall(thm.max_temp, thm)
        local ok_min, tmin = pcall(thm.min_temp, thm)
        if ok_max and ok_min then
            print(string.format("[thermal_test] max: %.3f  min: %.3f", tmax, tmin))
        else
            print("[thermal_test] FAIL: max_temp/min_temp error")
            all_ok = false
        end
    end

    -- clear max and min
    if all_ok then
        local ok_cm   = pcall(thm.clear_max, thm)
        local ok_cmin = pcall(thm.clear_min, thm)
        if ok_cm and ok_cmin then
            print("[thermal_test] clear_max and clear_min ok")
        else
            print("[thermal_test] FAIL: clear_max/clear_min error")
            all_ok = false
        end
    end

    -- resource recycle: close then re-open
    thm:close()

    if all_ok then
        print("[thermal_test] resource recycle: re-opening thermal sensor")
        local ok2, thm2 = pcall(thermal.new)
        if not ok2 then
            print("[thermal_test] FAIL: re-open failed: " .. tostring(thm2))
            print("[thermal_test] FAIL")
            return
        end
        sys.sleep_ms(interval_ms)
        local ok3, t3 = pcall(thm2.read, thm2)
        if not ok3 then
            print("[thermal_test] FAIL: re-open read error: " .. tostring(t3))
            thm2:close()
            print("[thermal_test] FAIL")
            return
        end
        print(string.format("[thermal_test] recycle read: %.3f °C", t3))
        thm2:close()
        print("[thermal_test] success")
    else
        print("[thermal_test] FAIL")
    end
end

local ok, err = pcall(main)
if not ok then
    print("[thermal_test] ERROR: " .. tostring(err))
end

local pwm = require("pwm")
local sys = require("sys")

-- RTL8721F: TIM4 channel 0 -> PA_6
local PIN       = "PA_6"
local TIMER_IDX = 4
local CHANNEL   = 0

local handle     = nil
local pass_count = 0
local fail_count = 0

local function check(label, ok, result)
    if ok then
        print("[pwm] PASS: " .. label)
        pass_count = pass_count + 1
    else
        print("[pwm] FAIL: " .. label .. " => " .. tostring(result))
        fail_count = fail_count + 1
    end
end

local function cleanup()
    if handle then
        pcall(handle.stop,  handle)
        pcall(handle.close, handle)
        handle = nil
    end
end

local function test()
    -- Case 1: Create PWM handle (1 kHz, 50%)
    local ok, result = pcall(pwm.new, {
        pin          = PIN,
        timer_idx    = TIMER_IDX,
        channel      = CHANNEL,
        frequency_hz = 1000,
        duty_percent = 50,
    })
    check("pwm.new 1kHz 50%", ok, result)
    if not ok then return end
    handle = result

    -- Case 2: start
    ok, result = pcall(handle.start, handle)
    check("start", ok, result)
    sys.sleep_ms(50)

    -- Case 3: duty cycle boundary and step values
    for _, duty in ipairs({0, 10, 25, 50, 75, 90, 100}) do
        ok, result = pcall(handle.set_duty, handle, duty)
        check("set_duty(" .. duty .. "%)", ok, result)
        sys.sleep_ms(20)
    end

    -- Case 4: duty ramp sweep (mirrors raw_pwm continuous breathe loop)
    --   ramp up from 0 to 100 in steps of PWM_STEP=10, then back down
    local STEP = 10
    local sweep_ok = true
    local d = 0
    while d <= 100 do
        local ok_s = pcall(handle.set_duty, handle, d)
        if not ok_s then sweep_ok = false end
        sys.sleep_ms(5)
        d = d + STEP
    end
    d = 100
    while d >= 0 do
        local ok_s = pcall(handle.set_duty, handle, d)
        if not ok_s then sweep_ok = false end
        sys.sleep_ms(5)
        d = d - STEP
    end
    check("duty ramp sweep 0->100->0", sweep_ok, "error during sweep")

    -- restore 50% duty so the frequency tests produce a visible waveform on LA
    pcall(handle.set_duty, handle, 50)

    -- Case 5: set_frequency across a range
    for _, freq in ipairs({500, 1000, 2000, 5000, 10000}) do
        ok, result = pcall(handle.set_frequency, handle, freq)
        check("set_frequency(" .. freq .. " Hz)", ok, result)
        sys.sleep_ms(20)
    end

    -- Case 6: get_channel_count must return 1
    ok, result = pcall(handle.get_channel_count, handle)
    if ok then
        if result == 1 then
            check("get_channel_count == 1", true, result)
        else
            check("get_channel_count == 1", false,
                  "expected 1, got " .. tostring(result))
        end
    else
        check("get_channel_count", false, result)
    end

    -- Case 7: stop / restart cycle
    ok, result = pcall(handle.stop, handle)
    check("stop", ok, result)
    sys.sleep_ms(50)
    ok, result = pcall(handle.start, handle)
    check("restart after stop", ok, result)
    sys.sleep_ms(50)

    -- Case 8: set_enabled(false) / set_enabled(true)
    ok, result = pcall(handle.set_enabled, handle, false)
    check("set_enabled(false)", ok, result)
    sys.sleep_ms(30)
    ok, result = pcall(handle.set_enabled, handle, true)
    check("set_enabled(true)", ok, result)
    sys.sleep_ms(30)

    -- Case 9: close
    ok, result = pcall(handle.close, handle)
    check("close", ok, result)
    handle = nil

    -- Case 10: error validation - duty_percent out of range
    ok, result = pcall(pwm.new, {
        pin = PIN, timer_idx = TIMER_IDX, channel = CHANNEL,
        duty_percent = 150,
    })
    check("new rejects duty_percent=150", not ok, result)

    -- Case 11: error validation - invalid timer_idx
    ok, result = pcall(pwm.new, {
        pin = PIN, timer_idx = 99, channel = CHANNEL,
    })
    check("new rejects timer_idx=99", not ok, result)

    -- Case 12: error validation - missing pin
    ok, result = pcall(pwm.new, {
        timer_idx = TIMER_IDX, channel = CHANNEL,
    })
    check("new rejects missing pin", not ok, result)
end

local ok, err = xpcall(test, debug.traceback)
cleanup()

print(string.format("[pwm] Results: %d passed, %d failed", pass_count, fail_count))
if not ok then
    print("[pwm] EXCEPTION: " .. tostring(err))
elseif fail_count == 0 then
    print("[pwm] ALL TESTS PASSED")
else
    print("[pwm] SOME TESTS FAILED")
end

local pwm = require("pwm")
local sys = require("sys")

-- RTL8721F: TIM4 channel 3 -> PA_26
local PIN       = "PA_26"
local TIMER_IDX = 4
local CHANNEL   = 3

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
        pcall(handle.set_enabled, handle, false)
        pcall(handle.close, handle)
        handle = nil
    end
end

-- Convert servo angle (0-180) to duty percent for a standard servo.
-- SG90: 50 Hz, pulse 500-2500 us, period 20000 us.
local SERVO_MIN_US = 500
local SERVO_MAX_US = 2500
local SERVO_PERIOD_US = 20000
local function servo_angle_to_duty(angle)
    local clamped = angle
    if clamped < 0 then clamped = 0 end
    if clamped > 180 then clamped = 180 end
    local pulse_us = SERVO_MIN_US + (SERVO_MAX_US - SERVO_MIN_US) * clamped / 180
    return pulse_us / SERVO_PERIOD_US * 100
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

    -- Case 2: set_enabled(true) — start output
    ok, result = pcall(handle.set_enabled, handle, true)
    check("set_enabled(true) start", ok, result)
    sys.sleep_ms(50)

    -- Case 3: duty cycle boundary and step values
    for _, duty in ipairs({0, 10, 25, 50, 75, 90, 100}) do
        ok, result = pcall(handle.set_duty, handle, duty)
        check("set_duty(" .. duty .. "%)", ok, result)
        sys.sleep_ms(20)
    end

    -- Case 4: duty ramp sweep
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

    -- Case 7: disable / re-enable cycle
    ok, result = pcall(handle.set_enabled, handle, false)
    check("set_enabled(false) disable", ok, result)
    sys.sleep_ms(50)
    ok, result = pcall(handle.set_enabled, handle, true)
    check("set_enabled(true) re-enable", ok, result)
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

    -- Case 13: re-open after close (init->deinit->init lifecycle)
    ok, result = pcall(pwm.new, {
        pin          = PIN,
        timer_idx    = TIMER_IDX,
        channel      = CHANNEL,
        frequency_hz = 1000,
        duty_percent = 50,
    })
    check("re-open after close (init lifecycle)", ok, result)
    if ok then
        handle = result
        pcall(handle.close, handle)
        handle = nil
    end

    -- ----------------------------------------------------------------
    -- Cases 14-18: Servo sweep (Tower Pro SG90)
    --   50 Hz, pulse 500-2500 us -> duty 2.5%-12.5%
    --   Pin: PA_6, TIM4 ch0
    -- ----------------------------------------------------------------
    print("[pwm] --- servo (SG90) tests ---")

    ok, result = pcall(pwm.new, {
        pin          = PIN,
        timer_idx    = TIMER_IDX,
        channel      = CHANNEL,
        frequency_hz = 50,
        duty_percent = servo_angle_to_duty(0),
    })
    check("servo: new 50Hz", ok, result)
    if not ok then return end
    handle = result

    ok, result = pcall(handle.set_enabled, handle, true)
    check("servo: set_enabled(true)", ok, result)
    sys.sleep_ms(200)

    -- Case 15: sweep 0 -> 180 in 10-degree steps
    local servo_sweep_ok = true
    local angle = 0
    while angle <= 180 do
        local duty = servo_angle_to_duty(angle)
        local ok_s, err_s = pcall(handle.set_duty, handle, duty)
        if not ok_s then
            servo_sweep_ok = false
            print("[pwm] servo sweep error at " .. angle .. "deg: " .. tostring(err_s))
        end
        sys.sleep_ms(80)
        angle = angle + 10
    end
    check("servo: sweep 0->180 deg", servo_sweep_ok, "error during sweep")

    -- Case 16: sweep 180 -> 0
    servo_sweep_ok = true
    angle = 180
    while angle >= 0 do
        local duty = servo_angle_to_duty(angle)
        local ok_s, err_s = pcall(handle.set_duty, handle, duty)
        if not ok_s then
            servo_sweep_ok = false
            print("[pwm] servo sweep error at " .. angle .. "deg: " .. tostring(err_s))
        end
        sys.sleep_ms(80)
        angle = angle - 10
    end
    check("servo: sweep 180->0 deg", servo_sweep_ok, "error during sweep")

    -- Case 17: verify expected duty values at cardinal angles
    local angles_duties = {
        {0,   servo_angle_to_duty(0)},    -- 2.5%
        {90,  servo_angle_to_duty(90)},   -- 7.5%
        {180, servo_angle_to_duty(180)},  -- 12.5%
    }
    local angles_ok = true
    for _, ad in ipairs(angles_duties) do
        local ang, expected_duty = ad[1], ad[2]
        local ok_s = pcall(handle.set_duty, handle, expected_duty)
        if not ok_s then angles_ok = false end
        sys.sleep_ms(300)
    end
    check("servo: cardinal angles (0/90/180 deg hold)", angles_ok, "set_duty failed")

    -- Case 18: close and verify re-open possible
    ok, result = pcall(handle.close, handle)
    check("servo: close", ok, result)
    handle = nil

    ok, result = pcall(pwm.new, {
        pin = PIN, timer_idx = TIMER_IDX, channel = CHANNEL,
        frequency_hz = 50, duty_percent = servo_angle_to_duty(90),
    })
    check("servo: re-open after close", ok, result)
    if ok then
        handle = result
    end
end

local ok, err = xpcall(test, function(e) return tostring(e) end)
cleanup()

print(string.format("[pwm] Results: %d passed, %d failed", pass_count, fail_count))
if not ok then
    print("[pwm] EXCEPTION: " .. tostring(err))
elseif fail_count == 0 then
    print("[pwm] ALL TESTS PASSED")
    print("success")
else
    print("[pwm] SOME TESTS FAILED")
end

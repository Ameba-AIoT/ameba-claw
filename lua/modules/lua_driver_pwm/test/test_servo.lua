-- Servo sweep test for ameba_claw (RTL8721F).
-- Behaviour mirrors the reference servo sweep script; adapted for the pwm Lua driver.
--
-- Default hardware: PA_26, TIM4 channel 3, Tower Pro SG90.
-- All parameters are overridable via the global `args` table.
--
-- Usage:
--   AT+CLAW=servo                     run with defaults
--
-- Configurable via args (all optional):
--   args.pin           string  GPIO pin name          default "PA_26"
--   args.timer_idx     number  Timer index (4-7)      default 4
--   args.channel       number  Timer channel (0-3)    default 3
--   args.frequency_hz  number  PWM frequency Hz       default 50
--   args.min_pulse_us  number  Min pulse width (us)   default 500
--   args.max_pulse_us  number  Max pulse width (us)   default 2500
--   args.start_angle   number  Sweep start (0-180)    default 45
--   args.end_angle     number  Sweep end   (0-180)    default 135
--   args.step          number  Angle step (degrees)   default 10
--   args.step_delay_ms number  Delay per step (ms)    default 80
--   args.edge_hold_ms  number  Hold at edge (ms)      default 500

local pwm = require("pwm")
local sys = require("sys")

local DEFAULT_PIN          = "PA_26"
local DEFAULT_TIMER_IDX    = 4
local DEFAULT_CHANNEL      = 3
local DEFAULT_FREQUENCY_HZ = 50
local DEFAULT_MIN_PULSE_US = 500
local DEFAULT_MAX_PULSE_US = 2500
local DEFAULT_START_ANGLE  = 45
local DEFAULT_END_ANGLE    = 135
local DEFAULT_STEP         = 10
local DEFAULT_STEP_DELAY_MS = 80
local DEFAULT_EDGE_HOLD_MS = 500

local a = type(args) == "table" and args or {}

local PIN          = type(a.pin)          == "string" and a.pin                              or DEFAULT_PIN
local TIMER_IDX    = type(a.timer_idx)    == "number" and math.floor(a.timer_idx)            or DEFAULT_TIMER_IDX
local CHANNEL      = type(a.channel)      == "number" and math.floor(a.channel)              or DEFAULT_CHANNEL
local FREQ_HZ      = type(a.frequency_hz) == "number" and math.floor(a.frequency_hz)         or DEFAULT_FREQUENCY_HZ
local MIN_PULSE_US = type(a.min_pulse_us) == "number" and a.min_pulse_us                     or DEFAULT_MIN_PULSE_US
local MAX_PULSE_US = type(a.max_pulse_us) == "number" and a.max_pulse_us                     or DEFAULT_MAX_PULSE_US
local START_ANGLE  = type(a.start_angle)  == "number" and a.start_angle                      or DEFAULT_START_ANGLE
local END_ANGLE    = type(a.end_angle)    == "number" and a.end_angle                        or DEFAULT_END_ANGLE
local STEP         = type(a.step)         == "number" and math.floor(math.abs(a.step))        or DEFAULT_STEP
local STEP_DELAY_MS = type(a.step_delay_ms) == "number" and math.floor(a.step_delay_ms)      or DEFAULT_STEP_DELAY_MS
local EDGE_HOLD_MS = type(a.edge_hold_ms) == "number" and math.floor(a.edge_hold_ms)         or DEFAULT_EDGE_HOLD_MS

local handle

local function angle_to_duty(angle)
    local clamped = angle
    if clamped < 0 then clamped = 0 end
    if clamped > 180 then clamped = 180 end
    local pulse_us = MIN_PULSE_US + (MAX_PULSE_US - MIN_PULSE_US) * clamped / 180
    return pulse_us * FREQ_HZ / 10000
end

local function validate_config()
    if STEP <= 0 then
        error("[servo] step must be > 0")
    end
    if FREQ_HZ <= 0 then
        error("[servo] frequency_hz must be > 0")
    end
    if MIN_PULSE_US <= 0 then
        error("[servo] min_pulse_us must be > 0")
    end
    if MAX_PULSE_US <= MIN_PULSE_US then
        error("[servo] max_pulse_us must be > min_pulse_us")
    end
    if START_ANGLE < 0 or START_ANGLE > 180 or END_ANGLE < 0 or END_ANGLE > 180 then
        error("[servo] start_angle and end_angle must be in range 0-180")
    end
    if STEP_DELAY_MS < 0 then
        error("[servo] step_delay_ms must be >= 0")
    end
    if EDGE_HOLD_MS < 0 then
        error("[servo] edge_hold_ms must be >= 0")
    end
end

local function cleanup()
    if handle then
        pcall(handle.set_enabled, handle, false)
        pcall(handle.close, handle)
        handle = nil
    end
end

local function move_range(from_angle, to_angle, step)
    local angle = from_angle
    while (step > 0 and angle <= to_angle) or (step < 0 and angle >= to_angle) do
        handle:set_duty(angle_to_duty(angle))
        print("[servo] angle -> " .. tostring(angle))
        sys.sleep_ms(STEP_DELAY_MS)
        angle = angle + step
    end
end

local ok, err = xpcall(function()
    validate_config()

    print(string.format(
        "[servo] pin=%s tim=%d ch=%d freq=%dHz pulse=%d-%dus range=%d->%ddeg step=%ddeg delay=%dms",
        PIN, TIMER_IDX, CHANNEL, FREQ_HZ,
        MIN_PULSE_US, MAX_PULSE_US,
        START_ANGLE, END_ANGLE,
        STEP, STEP_DELAY_MS
    ))

    handle = pwm.new({
        pin          = PIN,
        timer_idx    = TIMER_IDX,
        channel      = CHANNEL,
        frequency_hz = FREQ_HZ,
        duty_percent = angle_to_duty(START_ANGLE),
    })

    handle:set_enabled(true)
    print("[servo] started")
    sys.sleep_ms(EDGE_HOLD_MS)

    if START_ANGLE <= END_ANGLE then
        move_range(START_ANGLE, END_ANGLE,  STEP)
        sys.sleep_ms(EDGE_HOLD_MS)
        move_range(END_ANGLE,   START_ANGLE, -STEP)
    else
        move_range(START_ANGLE, END_ANGLE,  -STEP)
        sys.sleep_ms(EDGE_HOLD_MS)
        move_range(END_ANGLE,   START_ANGLE,  STEP)
    end

    sys.sleep_ms(EDGE_HOLD_MS)
    print("[servo] done")
end, function(e) return tostring(e) end)

cleanup()
if not ok then
    error(err)
end

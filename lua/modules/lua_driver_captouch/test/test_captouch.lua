-- test_captouch.lua
--
-- CAPTOUCH_TEST_MODE controls which test case runs:
--   "interactive" -- wait for physical finger touch on PA_17 (default)
--   "ext"         -- raw data monitor; no touch required; tests init + baseline
--
-- Run via: AT+CLAW=captouch        (interactive)
--          AT+CLAW=captouch,ext    (ext raw monitor)
--
-- CAPTOUCH_TEST_MODE is injected as a Lua global by the C provision layer.

local TEST_MODE = CAPTOUCH_TEST_MODE or "interactive"

local touch = require("captouch")
local sys   = require("sys")

-- PA_16 = CH4, PA_17 = CH3
local PIN1 = "PA_16"
local PIN2 = "PA_17"

-- ── interactive test (physical touch required) ────────────────────────────────

local function run_interactive()
    local POLL_MS    = 100
    local TIMEOUT_MS = 30000

    print("[captouch_test] init CapTouch on " .. PIN1 .. " (CH4) + " .. PIN2 .. " (CH3)")

    -- CH4 (PA_16, key index 1): mbias=0x590, threshold=80
    -- CH3 (PA_17, key index 2): mbias=0x560, threshold=50
    local ok, dev = pcall(captouch.new, PIN1, PIN2,
                          {n_noise_thr = 40, p_noise_thr = 40,
                           interval_ms = 10,
                           ch_mbias     = {0x590, 0x560},
                           ch_threshold = {80,    50}})
    if not ok then
        print("[captouch_test] FAIL: captouch.new failed: " .. tostring(dev))
        return false
    end
    print("[captouch_test] opened: " .. dev:name())

    print("[captouch_test] waiting for baseline calibration (1 s)...")
    sys.sleep_ms(1000)

    -- Track press+release completion for each key (must both complete to pass)
    local pressed_done  = {false, false}  -- saw PRESS event
    local released_done = {false, false}  -- saw RELEASE event after PRESS
    local last_state    = {false, false}
    local PIN_NAME      = {PIN1, PIN2}
    local elapsed_ms    = 0

    print(string.format(
        "[captouch_test] please press and release BOTH %s and %s within %d s",
        PIN1, PIN2, TIMEOUT_MS // 1000))

    while elapsed_ms < TIMEOUT_MS do
        local ok_r, sample = pcall(dev.read, dev)
        if not ok_r then
            print("[captouch_test] FAIL: read error: " .. tostring(sample))
            dev:close()
            return false
        end

        for _, key in ipairs(sample.keys) do
            local i = key.index
            if key.pressed ~= last_state[i] then
                print(string.format(
                    "[captouch_test] %s  pin=%s ch=%d smooth=%d benchmark=%d delta=%d",
                    key.pressed and "PRESSED  " or "RELEASED ",
                    PIN_NAME[i], key.channel, key.smooth, key.benchmark, key.delta))
                if key.pressed then
                    pressed_done[i] = true
                elseif pressed_done[i] then
                    released_done[i] = true
                    print(string.format("[captouch_test] %s press+release OK", PIN_NAME[i]))
                end
                last_state[i] = key.pressed
            end
        end

        if released_done[1] and released_done[2] then
            break
        end

        sys.sleep_ms(POLL_MS)
        elapsed_ms = elapsed_ms + POLL_MS
    end

    dev:close()

    local pass = released_done[1] and released_done[2]
    if pass then
        print("[captouch_test] PASS: both channels press+release detected")
    else
        for i = 1, 2 do
            if not released_done[i] then
                print(string.format("[captouch_test] FAIL: %s did not complete press+release", PIN_NAME[i]))
            end
        end
    end
    return pass
end

-- ── ext test (no physical touch required) ────────────────────────────────────

local function run_ext()
    local SAMPLES      = 10
    local INTERVAL_MS  = 500
    local BASELINE_MIN = 50
    local BASELINE_MAX = 60000

    print("[captouch_test] === ext raw-monitor test on " .. PIN1 .. " + " .. PIN2 .. " ===")
    print(string.format(
        "[captouch_test] reading %d samples every %d ms (no touch needed)",
        SAMPLES, INTERVAL_MS))

    -- CH4 (PA_16, key index 1): mbias=0x590, threshold=80
    -- CH3 (PA_17, key index 2): mbias=0x560, threshold=50
    local ok, dev = pcall(captouch.new, PIN1, PIN2,
                          {n_noise_thr = 40, p_noise_thr = 40,
                           interval_ms = 10,
                           ch_mbias     = {0x590, 0x560},
                           ch_threshold = {80,    50}})
    if not ok then
        print("[captouch_test] FAIL: captouch.new failed: " .. tostring(dev))
        return false
    end
    print("[captouch_test] opened: " .. dev:name())

    print("[captouch_test] waiting for baseline calibration (1 s)...")
    sys.sleep_ms(1000)

    local all_ok = true
    for i = 1, SAMPLES do
        local ok_r, sample = pcall(dev.read, dev)
        if not ok_r then
            print("[captouch_test] FAIL: read error: " .. tostring(sample))
            dev:close()
            return false
        end

        for _, key in ipairs(sample.keys) do
            local pin_name = (key.index == 1) and PIN1 or PIN2
            local baseline_ok = (key.benchmark >= BASELINE_MIN and
                                 key.benchmark <= BASELINE_MAX)
            print(string.format(
                "[captouch_test] sample %2d key%d(%s ch%d): smooth=%5d benchmark=%5d delta=%5d  [%s]",
                i, key.index, pin_name, key.channel,
                key.smooth, key.benchmark, key.delta,
                baseline_ok and "PASS" or "FAIL"))
            if not baseline_ok then all_ok = false end

            local ok_cs, cst = pcall(dev.get_ch_status, dev, key.index)
            if ok_cs then
                print(string.format("[captouch_test]   key%d hw_status=0x%x", key.index, cst))
            end
        end

        if i < SAMPLES then
            sys.sleep_ms(INTERVAL_MS)
        end
    end

    dev:close()
    return all_ok
end

-- ── main ──────────────────────────────────────────────────────────────────────

local function main()
    local pass
    if TEST_MODE == "interactive" then
        print("[captouch_test] mode=interactive  (touch PA17 with finger)")
        pass = run_interactive()
    elseif TEST_MODE == "ext" then
        print("[captouch_test] mode=ext  (raw monitor, no touch needed)")
        pass = run_ext()
    else
        print("[captouch_test] ERROR: unknown TEST_MODE: " .. tostring(TEST_MODE))
        return
    end

    if pass then
        print("[captouch_test] success")
    else
        print("[captouch_test] FAIL")
    end
end

local ok, err = pcall(main)
if not ok then
    print("[captouch_test] ERROR: " .. tostring(err))
end

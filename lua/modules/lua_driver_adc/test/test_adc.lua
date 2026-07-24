-- test_adc.lua
--
-- TEST_MODE:
--   "loopback"   -- PA_13 <-> PA_25, PA_14 <-> PA_26 connected with jumpers
--   "ext_supply" -- PA_13, PA_14 driven by an external supply (0~3.3 V)
--
-- Trigger:
--   AT+CLAW=adc        (loopback)
--   AT+CLAW=adc,ext    (ext_supply)

local TEST_MODE = ADC_TEST_MODE or "loopback"

local adc = require("adc")
local sys = require("sys")

-- Test pins: PA_13 = ADC CH6, PA_14 = ADC CH5
local ADC_PINS  = {"PA_13", "PA_14"}
local CH_PA13   = 6
local CH_PA14   = 5

-- GPIO pins driving the ADC inputs in loopback mode
local GPIO_PA13 = "PA_25"
local GPIO_PA14 = "PA_26"

local MV_LOW_MAX  = 300
local MV_HIGH_MIN = 2800
local MV_MIN      = -20  -- allow small negative offset from ADC calibration noise floor
local MV_MAX      = 3600   -- ADC calibration may read slightly above 3300 mV; leave margin
local SAMPLES     = 10

-- ── Helpers ──────────────────────────────────────────────────────────────────

-- Check that both CH_PA13 and CH_PA14 in table r fall within the expected range
local function check_pair(r, lo, hi, label)
    local v13 = r[CH_PA13]
    local v14 = r[CH_PA14]
    local ok13 = v13 and v13 >= lo and v13 <= hi
    local ok14 = v14 and v14 >= lo and v14 <= hi
    print(string.format("[adc_test] %s: CH%d(PA_13)=%s mV  CH%d(PA_14)=%s mV  [%s]",
        label,
        CH_PA13, v13 and tostring(v13) or "nil",
        CH_PA14, v14 and tostring(v14) or "nil",
        (ok13 and ok14) and "PASS" or "FAIL"))
    return ok13 and ok14
end

-- ── Loopback test ─────────────────────────────────────────────────────────────

local function run_loopback()
    local gpio = require("gpio")
    local all_pass = true

    -- Two GPIO outputs drive the two ADC inputs
    gpio.set_direction(GPIO_PA13, "output")
    gpio.set_direction(GPIO_PA14, "output")

    -- ── Phase 1: SW trigger — single read (one new opens both channels) ─────
    print("[adc_test] === Phase 1: SW trigger read() ===")

    local ok, ch = pcall(adc.new, "PA_13", "PA_14")
    if not ok then
        print("[adc_test] FAIL: adc.new error: " .. tostring(ch))
        return false
    end

    -- Low level
    gpio.set_level(GPIO_PA13, 0)
    gpio.set_level(GPIO_PA14, 0)
    sys.sleep_ms(10)
    local ok_r, r_low = pcall(ch.read, ch)
    if not ok_r then
        print("[adc_test] FAIL: read() error: " .. tostring(r_low))
        ch:close(); return false
    end
    if not check_pair(r_low, MV_MIN, MV_LOW_MAX, "GPIO=LOW ") then
        all_pass = false
    end

    -- High level
    gpio.set_level(GPIO_PA13, 1)
    gpio.set_level(GPIO_PA14, 1)
    sys.sleep_ms(10)
    local ok_r2, r_high = pcall(ch.read, ch)
    if not ok_r2 then
        print("[adc_test] FAIL: read() error: " .. tostring(r_high))
        gpio.set_level(GPIO_PA13, 0); gpio.set_level(GPIO_PA14, 0)
        ch:close(); return false
    end
    if not check_pair(r_high, MV_HIGH_MIN, MV_MAX, "GPIO=HIGH") then
        all_pass = false
    end

    -- ── Phase 2: read() x SAMPLES rounds (low level) ─────────────────────
    print(string.format("[adc_test] === Phase 2: read() x%d ===", SAMPLES))

    gpio.set_level(GPIO_PA13, 0)
    gpio.set_level(GPIO_PA14, 0)
    sys.sleep_ms(5)

    local multi_pass = true
    for i = 1, SAMPLES do
        local ok_i, r = pcall(ch.read, ch)
        if not ok_i then
            print("[adc_test] FAIL: read() error: " .. tostring(r))
            multi_pass = false; break
        end
        if not check_pair(r, MV_MIN, MV_LOW_MAX, string.format("round[%2d]", i)) then
            multi_pass = false
        end
    end
    if not multi_pass then all_pass = false end

    -- ── Phase 3: SW trigger low-level API (trigger/readable/read_raw) ─────
    print("[adc_test] === Phase 3: trigger / readable / read_raw ===")

    gpio.set_level(GPIO_PA13, 0)
    gpio.set_level(GPIO_PA14, 0)
    sys.sleep_ms(5)

    local sw_pass = false
    local ok_t = pcall(ch.trigger, ch)
    if ok_t then
        local polls = 0
        local ready = false
        repeat
            local ok_rbl, r = pcall(ch.readable, ch)
            ready = ok_rbl and r
            polls = polls + 1
        until ready or polls >= 10000

        -- Stop conversion so the FIFO stops growing, then drain it
        pcall(ch.trigger, ch, false)

        if ready then
            -- Read out all FIFO entries (one per channel)
            local raw_list = {}
            repeat
                local ok_rr, ch_id, raw = pcall(ch.read_raw, ch)
                if ok_rr and ch_id ~= nil then
                    table.insert(raw_list, {ch = ch_id, raw = raw})
                else
                    break
                end
            until not ch:readable()

            if #raw_list > 0 then
                sw_pass = true
                for i, e in ipairs(raw_list) do
                    local mv_est = math.floor(e.raw * 3300 / 65535)
                    local ok_v = mv_est < 400
                    print(string.format(
                        "[adc_test] read_raw[%d]: ch=%d raw=%d (~%d mV, polls=%d)  [%s]",
                        i, e.ch, e.raw, mv_est, polls, ok_v and "PASS" or "FAIL"))
                    if not ok_v then sw_pass = false end
                end
            else
                print("[adc_test] read_raw returned no data")
            end
        else
            print("[adc_test] readable() never became true")
        end
    else
        print("[adc_test] trigger() failed")
    end
    if not sw_pass then all_pass = false end

    gpio.set_level(GPIO_PA13, 0)
    gpio.set_level(GPIO_PA14, 0)
    ch:close()

    -- ── Phase 4: AUTO mode (low level / high level) ─────────────────────────
    print("[adc_test] === Phase 4: AUTO mode read_auto ===")

    local ok_ch2, ch2 = pcall(adc.new, "PA_13", "PA_14", "auto")
    if not ok_ch2 then
        print("[adc_test] FAIL: adc.new error: " .. tostring(ch2))
        return false
    end

    -- Low level AUTO
    gpio.set_level(GPIO_PA13, 0)
    gpio.set_level(GPIO_PA14, 0)
    sys.sleep_ms(10)

    local ok_a1, res_low = pcall(ch2.read_auto, ch2, SAMPLES * 2)
    if not ok_a1 then
        print("[adc_test] FAIL: read_auto(low) error: " .. tostring(res_low))
        ch2:close(); return false
    end
    print(string.format("[adc_test] AUTO LOW: %d samples", #res_low))
    local auto_low_pass = true
    for i = 1, #res_low do
        local r = res_low[i]
        local ok_v = r.mv >= MV_MIN and r.mv <= MV_LOW_MAX
        print(string.format("[adc_test]   [%2d] ch=%d  %4d mV  [%s]",
            i, r.ch, r.mv, ok_v and "PASS" or "FAIL"))
        if not ok_v then auto_low_pass = false end
    end
    if not auto_low_pass then all_pass = false end

    -- High level AUTO
    gpio.set_level(GPIO_PA13, 1)
    gpio.set_level(GPIO_PA14, 1)
    sys.sleep_ms(10)

    local ok_a2, res_high = pcall(ch2.read_auto, ch2, SAMPLES * 2)
    if not ok_a2 then
        print("[adc_test] FAIL: read_auto(high) error: " .. tostring(res_high))
        gpio.set_level(GPIO_PA13, 0); gpio.set_level(GPIO_PA14, 0)
        ch2:close(); return false
    end
    print(string.format("[adc_test] AUTO HIGH: %d samples", #res_high))
    local auto_high_pass = true
    for i = 1, #res_high do
        local r = res_high[i]
        local ok_v = r.mv >= MV_HIGH_MIN and r.mv <= MV_MAX
        print(string.format("[adc_test]   [%2d] ch=%d  %4d mV  [%s]",
            i, r.ch, r.mv, ok_v and "PASS" or "FAIL"))
        if not ok_v then auto_high_pass = false end
    end
    if not auto_high_pass then all_pass = false end

    gpio.set_level(GPIO_PA13, 0)
    gpio.set_level(GPIO_PA14, 0)
    ch2:close()

    return all_pass
end

-- ── External supply test ──────────────────────────────────────────────────────

local function run_ext_supply()
    local all_pass = true

    -- ── Phase 1: read() x SAMPLES rounds (both channels) ─────────────────
    print(string.format("[adc_test] === Phase 1: read() x%d on PA_13 + PA_14 ===", SAMPLES))

    local ok, ch = pcall(adc.new, "PA_13", "PA_14")
    if not ok then
        print("[adc_test] FAIL: adc.new error: " .. tostring(ch))
        return false
    end

    local sum13, sum14 = 0, 0
    for i = 1, SAMPLES do
        local ok_i, r = pcall(ch.read, ch)
        if not ok_i then
            print("[adc_test] FAIL: read() error: " .. tostring(r))
            ch:close(); return false
        end
        local v13 = r[CH_PA13] or 0
        local v14 = r[CH_PA14] or 0
        local ok_v = (v13 >= MV_MIN and v13 <= MV_MAX) and
                     (v14 >= MV_MIN and v14 <= MV_MAX)
        print(string.format("[adc_test] round[%2d]: CH%d=%4d mV  CH%d=%4d mV  [%s]",
            i, CH_PA13, v13, CH_PA14, v14, ok_v and "PASS" or "FAIL"))
        if not ok_v then all_pass = false end
        sum13 = sum13 + v13
        sum14 = sum14 + v14
    end
    print(string.format("[adc_test] average: CH%d=%d mV  CH%d=%d mV",
        CH_PA13, sum13 // SAMPLES, CH_PA14, sum14 // SAMPLES))

    -- ── Phase 2: SW trigger low-level API ───────────────────────────────
    print("[adc_test] === Phase 2: trigger / readable / read_raw ===")

    local sw_pass = true
    for trial = 1, 3 do
        local ok_t = pcall(ch.trigger, ch)
        if not ok_t then
            print("[adc_test] FAIL: trigger() at trial " .. trial)
            sw_pass = false; break
        end

        local polls, ready = 0, false
        repeat
            local ok_rbl, r = pcall(ch.readable, ch)
            if not ok_rbl then sw_pass = false; break end
            ready = r; polls = polls + 1
        until ready or polls >= 10000

        -- Stop conversion so the FIFO stops growing, then drain it
        pcall(ch.trigger, ch, false)
        if not sw_pass then break end

        if not ready then
            print("[adc_test] FAIL: readable() never true at trial " .. trial)
            sw_pass = false; break
        end

        -- Read one entry per channel
        local got = 0
        repeat
            local ok_rr, ch_id, raw = pcall(ch.read_raw, ch)
            if ok_rr and ch_id ~= nil then
                local ok_v = raw >= 0 and raw <= 65535
                print(string.format("[adc_test] trial %d raw[%d]: ch=%d raw=%d  [%s]",
                    trial, got + 1, ch_id, raw, ok_v and "PASS" or "FAIL"))
                if not ok_v then sw_pass = false end
                got = got + 1
            else
                break
            end
        until not ch:readable()

        sys.sleep_ms(20)
    end
    if not sw_pass then all_pass = false end

    ch:close()

    -- ── Phase 3: AUTO mode ───────────────────────────────────────────────
    print(string.format("[adc_test] === Phase 3: AUTO mode read_auto(%d) ===", SAMPLES * 2))

    local ok_ch3, ch3 = pcall(adc.new, "PA_13", "PA_14", "auto")
    if not ok_ch3 then
        print("[adc_test] FAIL: adc.new error: " .. tostring(ch3))
        return false
    end

    local ok_a, results = pcall(ch3.read_auto, ch3, SAMPLES * 2)
    ch3:close()
    if not ok_a then
        print("[adc_test] FAIL: read_auto error: " .. tostring(results))
        return false
    end
    print(string.format("[adc_test] AUTO: %d samples", #results))
    for i = 1, #results do
        local r = results[i]
        local ok_v = r.mv >= MV_MIN and r.mv <= MV_MAX
        print(string.format("[adc_test]   [%2d] ch=%d  %4d mV  [%s]",
            i, r.ch, r.mv, ok_v and "PASS" or "FAIL"))
        if not ok_v then all_pass = false end
    end

    return all_pass
end

-- ── Exclusivity test (no wiring required) ─────────────────────────────────────
-- Pins down the single-instance ADC contract: while the first handle is still
-- open, a second adc.new must error; after close, a new open must succeed.
local function run_exclusive()
    print("[adc_test] === Exclusive: single-config contract ===")

    local ok1, ch = pcall(adc.new, "PA_13", "PA_14")
    if not ok1 then
        print("[adc_test] FAIL: first adc.new error: " .. tostring(ch))
        return false
    end

    -- First handle still open: the second new must fail (else it silently reads the old config)
    local ok2, second = pcall(adc.new, "PA_13")
    if ok2 then
        print("[adc_test] FAIL: second adc.new should have errored but succeeded")
        pcall(function() second:close() end)
        ch:close()
        return false
    end
    print("[adc_test] second open rejected as expected: " .. tostring(second))

    -- After closing the first, reopening with a different mode must succeed
    ch:close()
    local ok3, ch3 = pcall(adc.new, "PA_13", "auto")
    if not ok3 then
        print("[adc_test] FAIL: reopen after close error: " .. tostring(ch3))
        return false
    end
    ch3:close()

    print("[adc_test] exclusive: PASS")
    return true
end

-- ── Main entry ────────────────────────────────────────────────────────────────

local function main()
    local excl_pass = run_exclusive()

    local pass
    if TEST_MODE == "loopback" then
        print("[adc_test] mode=loopback  (PA_13<->PA_25, PA_14<->PA_26 wired)")
        pass = run_loopback()
    elseif TEST_MODE == "ext_supply" then
        print("[adc_test] mode=ext_supply  (external supply on PA_13 + PA_14)")
        pass = run_ext_supply()
    else
        print("[adc_test] ERROR: unknown TEST_MODE: " .. tostring(TEST_MODE))
        return
    end

    pass = pass and excl_pass
    print(pass and "[adc_test] success" or "[adc_test] FAIL")
end

local ok, err = pcall(main)
if not ok then
    print("[adc_test] ERROR: " .. tostring(err))
end

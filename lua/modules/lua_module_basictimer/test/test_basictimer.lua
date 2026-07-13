-- test_basictimer.lua
-- Systematic test: TIM0-TIM3 x {sdm32k, xtal/div=40} -- all 8 must pass.
--
-- Run via: AT+CLAW=basic,timer
--
-- Coverage:
--   TC1-TC4  : TIM0-TIM3, SDM32K 32kHz, 100 ms period, 500 ms window (>= 3 IRQs)
--   TC5-TC8  : TIM0-TIM3, XTAL div=40 / 1 MHz, 50 ms period, 500 ms window (>= 6 IRQs)
--   TC9      : get_count() returns non-negative integer
--   TC10     : slot release -- close + re-new TIM0 (slot reuse)
--   TC11     : clear_irq_count() resets counter to 0
--   TC12     : set_period() changes reload period (slower period -> fewer IRQs)

local bt  = require("basictimer")
local sys = require("sys")

local fail_count = 0

local function check(label, cond, got, expected)
    if cond then
        print("[basictimer] PASS: " .. label)
    else
        print("[basictimer] FAIL: " .. label ..
              " (got=" .. tostring(got) .. " expected=" .. tostring(expected) .. ")")
        fail_count = fail_count + 1
    end
end

-- Standard lifecycle: new -> start -> wait 500 ms -> check irq_count -> stop -> close.
local function run_lifecycle(label, tim_idx, period_us, clk_src, xtal_div, min_irq)
    local h
    if xtal_div then
        h = bt.new(tim_idx, period_us, clk_src, xtal_div)
    else
        h = bt.new(tim_idx, period_us, clk_src)
    end
    check(label .. " new", h ~= nil, h, "userdata")
    if not h then return end
    h:start()
    sys.sleep_ms(500)
    local cnt = h:get_irq_count()
    check(label .. " irq >= " .. min_irq, cnt >= min_irq, cnt, ">= " .. min_irq)
    h:stop()
    h:close()
end

-- ---- TC1-TC4: TIM0-TIM3, SDM32K, 100 ms -> ~5 IRQs in 500 ms (expect >= 3) --
for idx = 0, 3 do
    run_lifecycle("TC" .. (idx + 1) .. " TIM" .. idx .. " sdm32k",
                  idx, 100000, "sdm32k", nil, 3)
end

-- ---- TC5-TC8: TIM0-TIM3, XTAL div=40 (1 MHz), 50 ms -> ~10 IRQs (expect >= 6) -
for idx = 0, 3 do
    run_lifecycle("TC" .. (idx + 5) .. " TIM" .. idx .. " xtal/40",
                  idx, 50000, "xtal", 40, 6)
end

-- ---- TC9: get_count() returns a non-negative integer -------------------------
local hc = bt.new(0, 100000)
hc:start()
sys.sleep_ms(50)
local raw = hc:get_count()
check("TC9 get_count integer >= 0", type(raw) == "number" and raw >= 0, raw, ">= 0")
hc:stop()
hc:close()

-- ---- TC10: slot release -- close + re-new TIM0 -------------------------------
local h0 = bt.new(0, 100000)
h0:close()
local h0b = bt.new(0, 100000)
check("TC10 slot reuse after close", h0b ~= nil, h0b, "userdata")
if h0b then h0b:close() end

-- ---- TC11: clear_irq_count() resets the counter to 0 ------------------------
local hcc = bt.new(0, 100000)  -- TIM0, sdm32k, 100 ms
hcc:start()
sys.sleep_ms(500)
local cnt_before = hcc:get_irq_count()  -- expect >= 3
hcc:clear_irq_count()
local cnt_after = hcc:get_irq_count()   -- should be 0 (or 1 if IRQ fires mid-read)
check("TC11 clear_irq_count before >= 3", cnt_before >= 3, cnt_before, ">= 3")
check("TC11 clear_irq_count after <= 1",  cnt_after  <= 1, cnt_after,  "<= 1")
hcc:stop()
hcc:close()

-- ---- TC12: set_period() changes the reload period --------------------------
-- 50 ms period -> ~10 IRQ/s; then change to 200 ms -> ~5 IRQ/s.
-- After change, count over 600 ms window should be in [2, 5].
local hsp = bt.new(0, 50000, "xtal", 40)  -- TIM0, xtal/40, 50 ms
hsp:start()
sys.sleep_ms(500)
local cnt_fast = hsp:get_irq_count()  -- expect >= 6
hsp:set_period(200000)                -- change to 200 ms
hsp:clear_irq_count()
sys.sleep_ms(600)
local cnt_slow = hsp:get_irq_count()  -- expect 2-4 at 200 ms period
check("TC12 set_period fast >= 6",      cnt_fast >= 6,                     cnt_fast, ">= 6")
check("TC12 set_period slow >= 2",      cnt_slow >= 2,                     cnt_slow, ">= 2")
check("TC12 set_period slow < fast",    cnt_slow < cnt_fast,               cnt_slow, "< " .. cnt_fast)
hsp:stop()
hsp:close()

-- ---- Summary -----------------------------------------------------------------
if fail_count == 0 then
    print("success")
else
    print("FAIL: " .. fail_count .. " test(s) failed")
end

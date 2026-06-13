-- test_gpio.lua
-- GPIO test: PA30 (output) -> PA31 (input / interrupt, loopback)
-- Run: file.run("test_gpio.lua")  or  AT+CLAW=gpio
-- Covers all 9 APIs: set_direction, set_level, get_level, set_pull,
--                    set_irq, irq_enable, irq_disable, get_irq_count, clear_irq_count

local gpio = require("gpio")
local sys  = require("sys")

local OUT_PIN = "PA_30"
local INT_PIN = "PA_31"

local fail_count = 0

local function check_val(label, got, expected)
    if got == expected then
        print("[gpio] " .. label .. ": ok (val=" .. tostring(got) .. ")")
    else
        print("[gpio] " .. label .. ": FAIL got=" .. tostring(got) .. " expected=" .. tostring(expected))
        fail_count = fail_count + 1
    end
end

local function check_ge(label, got, minv)
    if got >= minv then
        print("[gpio] " .. label .. ": ok (count=" .. tostring(got) .. " >= " .. tostring(minv) .. ")")
    else
        print("[gpio] " .. label .. ": FAIL count=" .. tostring(got) .. " expected >= " .. tostring(minv))
        fail_count = fail_count + 1
    end
end

-- Setup output pin, start low
gpio.set_direction(OUT_PIN, "output")
gpio.set_level(OUT_PIN, 0)
sys.sleep_ms(10)

-- ── Test 0a: get_level on output ──────────────────────────────────────────
print("[gpio] test 0a: get_level on output")
gpio.set_level(OUT_PIN, 0)
sys.sleep_ms(5)
check_val("get_level low",  gpio.get_level(OUT_PIN), 0)
gpio.set_level(OUT_PIN, 1)
sys.sleep_ms(5)
check_val("get_level high", gpio.get_level(OUT_PIN), 1)
gpio.set_level(OUT_PIN, 0)
sys.sleep_ms(5)

-- ── Test 0b: set_direction input + get_level loopback ─────────────────────
print("[gpio] test 0b: set_direction input + get_level loopback")
gpio.set_direction(INT_PIN, "input")
gpio.set_level(OUT_PIN, 1)
sys.sleep_ms(5)
check_val("input reads high", gpio.get_level(INT_PIN), 1)
gpio.set_level(OUT_PIN, 0)
sys.sleep_ms(5)
check_val("input reads low",  gpio.get_level(INT_PIN), 0)

-- ── Test 0c: set_pull (drive both ends high-Z, the pull owns the wire) ─────
print("[gpio] test 0c: set_pull up/down/none")
gpio.set_direction(OUT_PIN, "input")   -- release the driver so the wire floats
gpio.set_direction(INT_PIN, "input")
gpio.set_pull(INT_PIN, "up")
sys.sleep_ms(10)
check_val("pull up reads high", gpio.get_level(INT_PIN), 1)
gpio.set_pull(INT_PIN, "down")
sys.sleep_ms(10)
check_val("pull down reads low", gpio.get_level(INT_PIN), 0)
gpio.set_pull(INT_PIN, "none")
-- restore driver
gpio.set_direction(OUT_PIN, "output")
gpio.set_level(OUT_PIN, 0)
sys.sleep_ms(5)

-- ── Test 1: rising edge x10 (count must reach ~10) ────────────────────────
print("[gpio_irq] test 1: rising edge x10")
gpio.set_level(OUT_PIN, 0)
sys.sleep_ms(5)
gpio.set_irq(INT_PIN, "rising", 1)
gpio.clear_irq_count(INT_PIN)
gpio.irq_enable(INT_PIN)
sys.sleep_ms(5)
for i = 1, 10 do
    gpio.set_level(OUT_PIN, 1)
    sys.sleep_ms(20)
    gpio.set_level(OUT_PIN, 0)
    sys.sleep_ms(10)
end
gpio.irq_disable(INT_PIN)
check_ge("rising edge x10", gpio.get_irq_count(INT_PIN), 5)

-- ── Test 2: falling edge x10 ──────────────────────────────────────────────
print("[gpio_irq] test 2: falling edge x10")
gpio.set_level(OUT_PIN, 1)
sys.sleep_ms(5)
gpio.set_irq(INT_PIN, "falling", 1)
gpio.clear_irq_count(INT_PIN)
gpio.irq_enable(INT_PIN)
sys.sleep_ms(5)
for i = 1, 10 do
    gpio.set_level(OUT_PIN, 0)
    sys.sleep_ms(20)
    gpio.set_level(OUT_PIN, 1)
    sys.sleep_ms(10)
end
gpio.irq_disable(INT_PIN)
check_ge("falling edge x10", gpio.get_irq_count(INT_PIN), 5)

-- ── Test 3: both edges x10 (≈20 edges) ────────────────────────────────────
print("[gpio_irq] test 3: both edges x10")
gpio.set_level(OUT_PIN, 0)
sys.sleep_ms(5)
gpio.set_irq(INT_PIN, "both", 1)
gpio.clear_irq_count(INT_PIN)
gpio.irq_enable(INT_PIN)
sys.sleep_ms(5)
for i = 1, 10 do
    gpio.set_level(OUT_PIN, 1)
    sys.sleep_ms(20)
    gpio.set_level(OUT_PIN, 0)
    sys.sleep_ms(20)
end
gpio.irq_disable(INT_PIN)
check_ge("both edges x10", gpio.get_irq_count(INT_PIN), 10)

-- ── Test 4: level high (auto re-arm; must fire at least once) ─────────────
print("[gpio_irq] test 4: level high x10")
gpio.set_level(OUT_PIN, 0)
sys.sleep_ms(5)
gpio.set_irq(INT_PIN, "level_high", 1)
gpio.clear_irq_count(INT_PIN)
gpio.irq_enable(INT_PIN)
for i = 1, 10 do
    gpio.set_level(OUT_PIN, 1)
    sys.sleep_ms(20)
    gpio.set_level(OUT_PIN, 0)
    sys.sleep_ms(20)
end
gpio.irq_disable(INT_PIN)
check_ge("level high x10", gpio.get_irq_count(INT_PIN), 1)

-- ── Test 5: level low ─────────────────────────────────────────────────────
print("[gpio_irq] test 5: level low x10")
gpio.set_level(OUT_PIN, 1)
sys.sleep_ms(5)
gpio.set_irq(INT_PIN, "level_low", 1)
gpio.clear_irq_count(INT_PIN)
gpio.irq_enable(INT_PIN)
for i = 1, 10 do
    gpio.set_level(OUT_PIN, 0)
    sys.sleep_ms(20)
    gpio.set_level(OUT_PIN, 1)
    sys.sleep_ms(20)
end
gpio.irq_disable(INT_PIN)
check_ge("level low x10", gpio.get_irq_count(INT_PIN), 1)

-- ── Test 6: resource recycle (acquire -> op -> release, repeatable) ───────
-- Re-arm the IRQ a second time, prove it still counts, then fully release:
-- irq_disable + clear_irq_count must bring the counter back to 0.
print("[gpio_irq] test 6: resource recycle")
gpio.set_level(OUT_PIN, 0)
sys.sleep_ms(5)
gpio.set_irq(INT_PIN, "rising", 1)   -- re-acquire
gpio.clear_irq_count(INT_PIN)
gpio.irq_enable(INT_PIN)
sys.sleep_ms(5)
for i = 1, 5 do
    gpio.set_level(OUT_PIN, 1)
    sys.sleep_ms(20)
    gpio.set_level(OUT_PIN, 0)
    sys.sleep_ms(10)
end
gpio.irq_disable(INT_PIN)            -- release
local recycled = gpio.get_irq_count(INT_PIN)
check_ge("recycle re-fire", recycled, 3)
gpio.clear_irq_count(INT_PIN)        -- reset counter
check_val("recycle counter cleared", gpio.get_irq_count(INT_PIN), 0)

-- ── Summary ───────────────────────────────────────────────────────────────
if fail_count == 0 then
    print("success")
else
    print("FAIL: " .. fail_count .. " test(s) failed")
end

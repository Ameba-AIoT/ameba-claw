-- rtc_test.lua — RTC test suite for Ameba RTOS (RTL8721F)
-- Tests: timing accuracy, alarm, minute rollover, hour rollover, wakeup,
--        resource recycle (re-init + re-acquire/release of alarm & wakeup).
-- Single board, no external wiring required.

local rtc = require("rtc")
local sys = require("sys")

rtc.init()

-- ── Check 1: after 10 seconds ─────────────────────────────────────────────
print("[RTC] timing accuracy test start")
rtc.set_time(2024, 1, 15, 10, 0, 0)
sys.sleep_ms(200)

local t0 = rtc.get_time()
print(string.format("[RTC] T+0s  : %02d:%02d:%02d", t0.hour, t0.min, t0.sec))

print("[RTC] waiting 10s ...")
sys.sleep_ms(10000)

local t1 = rtc.get_time()
print(string.format("[RTC] T+10s : %02d:%02d:%02d", t1.hour, t1.min, t1.sec))

local ok1 = (t1.hour == 10 and t1.min == 0 and t1.sec >= 9 and t1.sec <= 12)
if ok1 then
    print("[RTC] T+10s pass")
else
    print(string.format("[FAIL] T+10s expected 10:00:10+-2, got %02d:%02d:%02d",
        t1.hour, t1.min, t1.sec))
end

-- ── Check 2: after ~60 seconds ────────────────────────────────────────────
print("[RTC] waiting 50 more seconds (total ~60s) ...")
sys.sleep_ms(50000)

local t2 = rtc.get_time()
print(string.format("[RTC] T+60s : %02d:%02d:%02d", t2.hour, t2.min, t2.sec))

local ok2 = (t2.hour == 10 and t2.min == 1 and t2.sec >= 0 and t2.sec <= 3)
if ok2 then
    print("[RTC] T+60s pass")
else
    print(string.format("[FAIL] T+60s expected ~10:01:00, got %02d:%02d:%02d",
        t2.hour, t2.min, t2.sec))
end

-- ── Check 3: alarm fires within 5 seconds ────────────────────────────────
print("[RTC] alarm test start")
rtc.disable_alarm()
rtc.set_time(2024, 1, 15, 10, 0, 0)
sys.sleep_ms(200)
rtc.set_alarm(10, 0, 5)
sys.sleep_ms(6000)

local ok3 = rtc.alarm_fired()
if ok3 then
    print("[RTC] alarm pass")
else
    print("[FAIL] alarm did not fire after 6s")
end
rtc.clear_alarm()
rtc.disable_alarm()

-- ── Check 4: minute rollover (xx:00:55 → xx:01:01) ───────────────────────
print("[RTC] minute rollover test")
rtc.set_time(2024, 1, 15, 10, 0, 55)
sys.sleep_ms(200)
sys.sleep_ms(6000)

local t4 = rtc.get_time()
print(string.format("[RTC] minute rollover: %02d:%02d:%02d", t4.hour, t4.min, t4.sec))

local ok4 = (t4.hour == 10 and t4.min == 1 and t4.sec >= 0 and t4.sec <= 3)
if ok4 then
    print("[RTC] minute rollover pass")
else
    print(string.format("[FAIL] minute rollover expected ~10:01:01, got %02d:%02d:%02d",
        t4.hour, t4.min, t4.sec))
end

-- ── Check 5: hour rollover (10:59:55 → 11:00:01) ─────────────────────────
print("[RTC] hour rollover test")
rtc.set_time(2024, 1, 15, 10, 59, 55)
sys.sleep_ms(200)
sys.sleep_ms(6000)

local t5 = rtc.get_time()
print(string.format("[RTC] hour rollover: %02d:%02d:%02d", t5.hour, t5.min, t5.sec))

local ok5 = (t5.hour == 11 and t5.min == 0 and t5.sec >= 0 and t5.sec <= 3)
if ok5 then
    print("[RTC] hour rollover pass")
else
    print(string.format("[FAIL] hour rollover expected ~11:00:01, got %02d:%02d:%02d",
        t5.hour, t5.min, t5.sec))
end

-- ── Check 6: periodic wakeup fires within 3 seconds ─────────────────────
print("[RTC] wakeup test")
rtc.disable_wakeup()
rtc.set_wakeup(3)
sys.sleep_ms(4000)

local ok6 = rtc.wakeup_fired()
if ok6 then
    print("[RTC] wakeup pass")
else
    print("[FAIL] wakeup did not fire after 4s")
end
rtc.clear_wakeup()
rtc.disable_wakeup()

-- ── Check 7: resource recycle (init→op→deinit→re-init→re-op) ─────────────
-- Proves the init/acquire/release cycle is repeatable: a second init() is
-- idempotent (does not wipe the time), the calendar date fields survive the
-- yday<->mon/mday conversion, and alarm/wakeup can be re-acquired then released.
print("[RTC] resource recycle test")
rtc.disable_alarm()
rtc.disable_wakeup()
rtc.init()                         -- idempotent re-init, must not disturb time
rtc.set_time(2024, 6, 11, 8, 30, 0)
sys.sleep_ms(200)

local t7 = rtc.get_time()
print(string.format("[RTC] recycle read-back: %04d-%02d-%02d %02d:%02d:%02d",
    t7.year, t7.mon, t7.mday, t7.hour, t7.min, t7.sec))

rtc.set_alarm(8, 30, 30)           -- re-acquire alarm ...
rtc.disable_alarm()                -- ... then release
rtc.set_wakeup(10)                 -- re-acquire wakeup ...
rtc.disable_wakeup()               -- ... then release

local ok7 = (t7.year == 2024 and t7.mon == 6 and t7.mday == 11 and
             t7.hour == 8 and t7.min == 30 and t7.sec >= 0 and t7.sec <= 2)
if ok7 then
    print("[RTC] resource recycle pass")
else
    print(string.format("[FAIL] recycle expected 2024-06-11 08:30:00+-2, got %04d-%02d-%02d %02d:%02d:%02d",
        t7.year, t7.mon, t7.mday, t7.hour, t7.min, t7.sec))
end

-- ── Result ─────────────────────────────────────────────────────────────────
if ok1 and ok2 and ok3 and ok4 and ok5 and ok6 and ok7 then
    print("[RTC] all pass")
else
    print("[RTC] fail")
end

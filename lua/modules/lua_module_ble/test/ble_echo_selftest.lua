-- ble_echo_selftest.lua — BLE peripheral bring-up / regression script.
--
-- This is OUR harness bring-up script (not an LLM-facing skill). It exercises
-- the full transparent link end to end and needs a phone to drive the peer side
-- (nRF Connect / LightBlue), so it cannot self-assert — it prints every event
-- and auto-echoes inbound writes so a human can watch the loop over serial.
--
-- How to run (bring-up, NOT via the LLM):
--   1. Put this file on the device VFS, e.g. vfs:/ble_echo_selftest.lua
--      (push with the file tools, or embed via a *_test_provision.c when
--       CONFIG_CLAW_ENABLE_TESTS is on).
--   2. AT+CLAW=lua_execute_async,vfs:/ble_echo_selftest.lua
--      (async because it runs an event loop; use lua_execute_sync only for a
--       short bounded run).
--
-- Phone steps: scan -> connect "claw-ble-selftest" -> expand service 0xFFF0 ->
--   char 0xFFF1 -> enable notifications (subscribe) -> write text -> observe the
--   "echo:<text>" notification come back.

local ble = require("ble")

local ok, err = ble.init("claw-ble-selftest")
if not ok then
    print("[selftest] init failed:", err)
    return
end
print("[selftest] address:", ble.address())

ble.on_event(function(ev)
    print("[selftest] EV", ev.type, ev.conn, ev.payload, ev.notify, ev.mtu)
    if ev.type == "data" and ev.conn then
        local nok, nerr = ble.notify(ev.conn, "echo:" .. tostring(ev.payload))
        print("[selftest] notify ->", nok, nerr)
    end
end)

print("[selftest] adv_start:", ble.adv_start())

-- Pump the event loop. ~30 min then exit so a stray async job cannot run forever.
for _ = 1, 3600 do
    ble.process_events(500)
end
print("[selftest] done")

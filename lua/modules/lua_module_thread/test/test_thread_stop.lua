-- SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
-- SPDX-License-Identifier: Apache-2.0
--
-- test_thread_stop — batch B cancellation test. Starts a child blocked on a
-- thread.sync.queue_recv() with a 20s timeout, then thread.stop()s it almost
-- immediately. If the cancel path inside thread.sync (the cooperative
-- __cancel_ptr check, armed for async jobs before run() starts — unlike
-- batch A's single-script top-level-code window) works, the child observes
-- "stopped" and exits within ~1-2s, not the full 20s.
--
-- Run with: AT+CLAW=skill,test_thread_stop.lua

local thread = require("thread")
local sys = require("sys")

function run(args)
    local id, err = thread.start("vfs:/skills/thread_stop_child_blocker.lua", {}, {name = "stop_blocker"})
    if not id then
        print("[stop_test] start failed: " .. tostring(err))
        return false
    end
    print("[stop_test] started job_id=" .. id)

    sys.sleep_ms(300)   -- let it actually reach queue_recv before stopping

    local t0 = sys.millis and sys.millis() or 0
    local stopres, stoperr = thread.stop(id)
    if not stopres then
        print("[stop_test] thread.stop failed: " .. tostring(stoperr))
        return false
    end
    print("[stop_test] thread.stop() returned stopped=" .. tostring(stopres.stopped) ..
          " status=" .. tostring(stopres.status))

    local info, geterr = thread.get(id)
    if not info then
        print("[stop_test] thread.get failed: " .. tostring(geterr))
        return false
    end
    print("[stop_test] final status=" .. tostring(info.status) .. " log=" .. tostring(info.log))

    local fast_enough = true
    if t0 ~= 0 and sys.millis then
        local elapsed = sys.millis() - t0
        fast_enough = elapsed < 5000
        print("[stop_test] elapsed=" .. elapsed .. "ms")
    end

    local saw_stopped_in_log = info.log and string.find(info.log, "err=stopped") ~= nil
    local ok = stopres.stopped == true and fast_enough and saw_stopped_in_log

    if ok then
        print("[stop_test] PASS")
    else
        print("[stop_test] FAIL stopped=" .. tostring(stopres.stopped) ..
              " fast_enough=" .. tostring(fast_enough) ..
              " saw_stopped_in_log=" .. tostring(saw_stopped_in_log))
    end
    return ok
end

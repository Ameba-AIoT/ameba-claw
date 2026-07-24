-- SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
-- SPDX-License-Identifier: Apache-2.0
--
-- test_thread_job_named_replace — batch C. Starts a long-blocked job under
-- opts.name="job_named_test", tries to start a second job with the SAME
-- name (expect a "conflict" error, since the first is still active), then
-- retries with opts.replace=true (expect it to stop the first job and take
-- its place). Reuses thread_stop_child_blocker.lua as the long-blocked
-- script (20s queue_recv, exits early on cancel).
--
-- Run with: AT+CLAW=skill,test_thread_job_named_replace.lua

local thread = require("thread")
local sys = require("sys")

local CHILD = "vfs:/skills/thread_stop_child_blocker.lua"

function run(args)
    local id1, err1 = thread.start(CHILD, {}, {name = "job_named_test"})
    if not id1 then
        print("[named] start 1 failed: " .. tostring(err1))
        return false
    end
    sys.sleep_ms(200)   -- let it reach queue_recv

    local id2, err2 = thread.start(CHILD, {}, {name = "job_named_test"})
    local conflict_detected = (id2 == nil) and err2 ~= nil and string.find(err2, "conflict") ~= nil
    print("[named] duplicate name, no replace -> id2=" .. tostring(id2) .. " err2=" .. tostring(err2))

    local id3, err3 = thread.start(CHILD, {}, {name = "job_named_test", replace = true})
    print("[named] duplicate name, replace=true -> id3=" .. tostring(id3) .. " err3=" .. tostring(err3))
    local replaced_ok = id3 ~= nil

    sys.sleep_ms(200)
    local info1 = thread.get(id1)
    local old_terminal = info1 and (info1.status == "STOPPED" or info1.status == "DONE")

    if id3 then thread.stop(id3) end   -- cleanup: don't leave a job running

    local ok = conflict_detected and replaced_ok and old_terminal
    if ok then
        print("[named] PASS")
    else
        print("[named] FAIL conflict_detected=" .. tostring(conflict_detected) ..
              " replaced_ok=" .. tostring(replaced_ok) ..
              " old_terminal=" .. tostring(old_terminal) ..
              " old_status=" .. tostring(info1 and info1.status))
    end
    return ok
end

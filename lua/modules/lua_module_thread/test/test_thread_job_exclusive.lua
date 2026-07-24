-- SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
-- SPDX-License-Identifier: Apache-2.0
--
-- test_thread_job_exclusive — batch C. Same shape as
-- test_thread_job_named_replace.lua but arbitrating on opts.exclusive (an
-- exclusive-group string) instead of opts.name — two DIFFERENTLY-named jobs
-- in the same exclusive group should still conflict.
--
-- Run with: AT+CLAW=skill,test_thread_job_exclusive.lua

local thread = require("thread")
local sys = require("sys")

local CHILD = "vfs:/skills/thread_stop_child_blocker.lua"

function run(args)
    local id1, err1 = thread.start(CHILD, {}, {name = "excl_a", exclusive = "job_excl_test"})
    if not id1 then
        print("[excl] start 1 failed: " .. tostring(err1))
        return false
    end
    sys.sleep_ms(200)

    local id2, err2 = thread.start(CHILD, {}, {name = "excl_b", exclusive = "job_excl_test"})
    local conflict_detected = (id2 == nil) and err2 ~= nil and string.find(err2, "conflict") ~= nil
    print("[excl] same exclusive group, no replace -> id2=" .. tostring(id2) .. " err2=" .. tostring(err2))

    local id3, err3 = thread.start(CHILD, {}, {name = "excl_c", exclusive = "job_excl_test", replace = true})
    print("[excl] same exclusive group, replace=true -> id3=" .. tostring(id3) .. " err3=" .. tostring(err3))
    local replaced_ok = id3 ~= nil

    sys.sleep_ms(200)
    local info1 = thread.get(id1)
    local old_terminal = info1 and (info1.status == "STOPPED" or info1.status == "DONE")

    if id3 then thread.stop(id3) end   -- cleanup

    local ok = conflict_detected and replaced_ok and old_terminal
    if ok then
        print("[excl] PASS")
    else
        print("[excl] FAIL conflict_detected=" .. tostring(conflict_detected) ..
              " replaced_ok=" .. tostring(replaced_ok) ..
              " old_terminal=" .. tostring(old_terminal) ..
              " old_status=" .. tostring(info1 and info1.status))
    end
    return ok
end

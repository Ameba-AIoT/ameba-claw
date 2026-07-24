-- SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
-- SPDX-License-Identifier: Apache-2.0
--
-- test_thread_sync.lua — single-script self-test of thread.sync semantics
-- (create/timeout/busy/type-conflict), run entirely within one job. The
-- cross-job producer/consumer case lives in thread_full_flow_*.lua instead
-- (see design_spec/lua/lua_module_thread_architecture.md).
--
-- Run with: AT+CLAW=skill,test_thread_sync.lua

local thread = require("thread")

local fail_count = 0

local function check(label, cond, detail)
    if cond then
        print("[thread] " .. label .. ": ok")
    else
        print("[thread] " .. label .. ": FAIL " .. tostring(detail))
        fail_count = fail_count + 1
    end
end

-- ---- queue ---------------------------------------------------------------

check("queue_create", thread.sync.queue_create("q1", {depth = 2, item_size = 16}))

local ok2, err2 = thread.sync.queue_create("q1")
check("queue_create duplicate -> exists", ok2 == nil and err2 == "exists", err2)

check("queue_send roundtrip", thread.sync.queue_send("q1", "hello", 100))
local v, e = thread.sync.queue_recv("q1", 100)
check("queue_recv roundtrip", v == "hello", e)

local rv, re = thread.sync.queue_recv("q1", 100)
check("queue_recv empty -> timeout", rv == nil and re == "timeout", re)

check("queue_send fill depth 1/2", thread.sync.queue_send("q1", "a", 0))
check("queue_send fill depth 2/2", thread.sync.queue_send("q1", "b", 0))
local sok, serr = thread.sync.queue_send("q1", "c", 100)
check("queue_send over depth -> timeout", sok == false and serr == "timeout", serr)

local big_ok = pcall(thread.sync.queue_send, "q1", string.rep("x", 17), 0)
check("queue_send over item_size -> error", big_ok == false)

local dok, derr = thread.sync.queue_delete("q1")
check("queue_delete busy (unread messages)", dok == nil and derr == "busy", derr)

thread.sync.queue_recv("q1", 0)
thread.sync.queue_recv("q1", 0)
check("queue_delete after drain", thread.sync.queue_delete("q1"))

local nfv, nfe = thread.sync.queue_recv("nope", 0)
check("queue_recv not_found", nfv == nil and nfe == "not_found", nfe)

-- ---- semaphore -------------------------------------------------------------

check("sem_create", thread.sync.sem_create("s1", {max = 2, initial = 0}))
check("sem_take empty -> timeout", (function()
    local ok, err = thread.sync.sem_take("s1", 50)
    return ok == false and err == "timeout"
end)())
check("sem_give 1/2", thread.sync.sem_give("s1"))
check("sem_give 2/2", thread.sync.sem_give("s1"))
local fok, ferr = thread.sync.sem_give("s1")
check("sem_give over max -> full", fok == false and ferr == "full", ferr)
check("sem_take 1/2", thread.sync.sem_take("s1", 0))
check("sem_take 2/2", thread.sync.sem_take("s1", 0))
check("sem_delete", thread.sync.sem_delete("s1"))

-- ---- lock -------------------------------------------------------------

check("lock_create", thread.sync.lock_create("l1"))
check("lock acquire", thread.sync.lock("l1", 100))
check("unlock owner", thread.sync.unlock("l1"))
local uok, uerr = thread.sync.unlock("l1")
check("unlock non-owner -> not_owner", uok == false and uerr == "not_owner", uerr)
check("lock re-acquire after unlock", thread.sync.lock("l1", 100))
check("unlock again", thread.sync.unlock("l1"))
check("lock_delete", thread.sync.lock_delete("l1"))

-- ---- type conflict -------------------------------------------------------------

thread.sync.queue_create("mixed", {depth = 1, item_size = 8})
local type_ok = pcall(thread.sync.sem_take, "mixed", 0)
check("sem_take on a queue name -> type error", type_ok == false)
thread.sync.queue_delete("mixed")

function run(args)
    if fail_count == 0 then
        print("[thread] PASS")
    else
        print("[thread] FAIL: " .. fail_count .. " check(s) failed")
    end
    return fail_count == 0
end

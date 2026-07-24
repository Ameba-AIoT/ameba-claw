-- SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
-- SPDX-License-Identifier: Apache-2.0
--
-- test_thread_full_flow — batch B cross-job test. This script (running as
-- ONE job) creates a shared thread.sync queue, starts two child jobs via
-- thread.start() (producer + consumer, both running CONCURRENTLY as
-- separate jobs), and polls thread.get() until both terminate. Proves job
-- orchestration (start/get) and thread.sync (queue) work together across
-- job boundaries — the H-4 scenario this whole module exists to fix.
--
-- Run with: AT+CLAW=skill,thread_full_flow_parent.lua

local thread = require("thread")
local sys = require("sys")

local function wait_terminal(id, label)
    for _ = 1, 100 do
        local info, err = thread.get(id)
        if not info then
            print("[flow] " .. label .. " get failed: " .. tostring(err))
            return nil
        end
        local s = info.status
        if s == "DONE" or s == "FAILED" or s == "TIMEOUT" or s == "STOPPED" then
            return info
        end
        sys.sleep_ms(100)
    end
    print("[flow] " .. label .. " did not terminate in time")
    return nil
end

function run(args)
    local ok, err = thread.sync.queue_create("ff_queue", {depth = 4, item_size = 64})
    if not ok then
        print("[flow] queue_create failed: " .. tostring(err))
        return false
    end

    local base = "vfs:/skills/"
    local producer_id, perr = thread.start(base .. "thread_full_flow_child_producer.lua",
                                            {n = 5}, {name = "ff_producer"})
    if not producer_id then
        print("[flow] start producer failed: " .. tostring(perr))
        thread.sync.queue_delete("ff_queue")
        return false
    end
    local consumer_id, cerr = thread.start(base .. "thread_full_flow_child_consumer.lua",
                                            {n = 5}, {name = "ff_consumer"})
    if not consumer_id then
        print("[flow] start consumer failed: " .. tostring(cerr))
        thread.sync.queue_delete("ff_queue")
        return false
    end
    print("[flow] started producer job_id=" .. producer_id .. " consumer job_id=" .. consumer_id)

    local jobs = thread.list()
    print("[flow] thread.list() sees " .. #jobs .. " job(s)")

    local pinfo = wait_terminal(producer_id, "producer")
    local cinfo = wait_terminal(consumer_id, "consumer")

    thread.sync.queue_delete("ff_queue")

    local ok_all = pinfo and pinfo.status == "DONE" and cinfo and cinfo.status == "DONE"
    if ok_all then
        print("[flow] producer log: " .. tostring(pinfo.log))
        print("[flow] consumer log: " .. tostring(cinfo.log))
        print("[flow] PASS")
    else
        print("[flow] FAIL producer=" .. (pinfo and pinfo.status or "?") ..
              " consumer=" .. (cinfo and cinfo.status or "?"))
    end
    return ok_all
end

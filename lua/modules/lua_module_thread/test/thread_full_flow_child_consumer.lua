-- SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
-- SPDX-License-Identifier: Apache-2.0
--
-- Started by thread_full_flow_parent.lua via thread.start(). Receives
-- `args.n` items from the "ff_queue" thread.sync queue (created by the
-- parent) and checks they arrive in order.

local thread = require("thread")

function run(args)
    local n = (args and args.n) or 5
    for i = 1, n do
        local v, err = thread.sync.queue_recv("ff_queue", 3000)
        if not v then
            print("[consumer] recv " .. i .. " failed: " .. tostring(err))
            return false
        end
        local expected = "item" .. i
        if v ~= expected then
            print("[consumer] out of order at " .. i .. ": got=" .. v .. " expected=" .. expected)
            return false
        end
    end
    print("[consumer] received " .. n .. " items in order")
    return true
end

-- SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
-- SPDX-License-Identifier: Apache-2.0
--
-- Started by thread_full_flow_parent.lua via thread.start(). Sends `args.n`
-- items into the "ff_queue" thread.sync queue (created by the parent).

local thread = require("thread")

function run(args)
    local n = (args and args.n) or 5
    for i = 1, n do
        local ok, err = thread.sync.queue_send("ff_queue", "item" .. i, 2000)
        if not ok then
            print("[producer] send " .. i .. " failed: " .. tostring(err))
            return false
        end
    end
    print("[producer] sent " .. n .. " items")
    return true
end

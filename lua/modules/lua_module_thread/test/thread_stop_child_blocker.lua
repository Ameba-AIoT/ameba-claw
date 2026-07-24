-- SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
-- SPDX-License-Identifier: Apache-2.0
--
-- Started by test_thread_stop.lua via thread.start(). Blocks on a
-- thread.sync queue that nobody will ever send to, so it only terminates
-- via thread.stop() — exercises the cancel path INSIDE a thread.sync wait
-- (the ASYNC job registry key, __cancel_ptr, is armed before run() starts,
-- unlike the top-level-of-script window batch A's single-script test ran in).

local thread = require("thread")

function run(args)
    thread.sync.queue_create("stop_test_queue", {depth = 1, item_size = 16})
    local v, err = thread.sync.queue_recv("stop_test_queue", 20000)
    thread.sync.queue_delete("stop_test_queue")
    print("[blocker] queue_recv returned: v=" .. tostring(v) .. " err=" .. tostring(err))
    return err == "stopped"
end

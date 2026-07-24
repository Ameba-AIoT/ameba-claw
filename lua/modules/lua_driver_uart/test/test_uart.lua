-- test_uart.lua
-- UART driver tests.
-- MODE global injected by C before script runs:
--
-- Single-board (no external wiring needed):
--   "loopback"      → 5-case loopback self-test (UART0, PA_25/PA_26)
--   "loopback_baud" → baud-rate sweep 9600–3M in loopback (UART0, PA_25/PA_26)
--   "loopback_opts" → data_bits/parity/stop_bits combos in loopback
--   "loopback_port" → UART1/2/3 loopback with PA_25/PA_26 and PA_7/PA_6
--
-- Two-board (requires wiring below):
--   "tx"  / "rx"   → UART0 basic echo,          board-A = tx, board-B = rx
--   "tx2" / "rx2"  → UART0 basic echo (swapped), board-B = tx, board-A = rx
--   "txe" / "rxe"  → extended: baud+opts+ports,  board-A = tx, board-B = rx
--   "txe2"/ "rxe2" → extended (swapped),          board-B = tx, board-A = rx
--
-- Two-board wiring (fixed, all modes share the same physical wires):
--   Wire A: board-A PA_25 ────► board-B PA_6
--   Wire B: board-B PA_7  ────► board-A PA_26
--
-- Two-board usage: start echo side (rx/rx2/rxe/rxe2) first,
-- then start sender (tx/tx2/txe/txe2) within 90 seconds.

local uart = require("uart")
local sys  = require("sys")

local MODE = MODE or "loopback"

local fail_count = 0

local function check_val(label, got, expected)
    if got == expected then
        print("[uart] " .. label .. ": ok")
    else
        print("[uart] " .. label .. ": FAIL got=" .. tostring(got)
              .. " expected=" .. tostring(expected))
        fail_count = fail_count + 1
    end
end

local function check_true(label, cond, detail)
    if cond then
        print("[uart] " .. label .. ": ok")
    else
        print("[uart] " .. label .. ": FAIL"
              .. (detail and (" (" .. detail .. ")") or ""))
        fail_count = fail_count + 1
    end
end

-- Shared sub-test list for txe/rxe and txe2/rxe2.
-- Both sender and echo sides iterate this list in the same order so they stay
-- in lock-step without an explicit handshake.
local EXT_SUBTESTS = {
    -- baud-rate sub-tests (UART0, default 8N1)
    {label="baud 9600",   port=0, baud=9600,   opts=nil},
    {label="baud 38400",  port=0, baud=38400,  opts=nil},
    {label="baud 921600", port=0, baud=921600, opts=nil},
    -- opts sub-tests (UART0, 115200 baud)
    {label="opts 7N1", port=0, baud=115200, opts={data_bits=7, parity="none",  stop_bits=1}},
    {label="opts 8N2", port=0, baud=115200, opts={data_bits=8, parity="none",  stop_bits=2}},
    {label="opts 8O1", port=0, baud=115200, opts={data_bits=8, parity="odd",   stop_bits=1}},
    {label="opts 8E1", port=0, baud=115200, opts={data_bits=8, parity="even",  stop_bits=1}},
    -- UART port sub-tests (115200, 8N1) — same physical wires, different controller.
    {label="port 1", port=1, baud=115200, opts=nil},
    {label="port 2", port=2, baud=115200, opts=nil},
    {label="port 3", port=3, baud=115200, opts=nil},
}
-- All chars are < 0x80 so the string is valid in 7-bit data mode too.
-- 22 chars @ 9600 baud ≈ 22.9 ms — safe within the 80 ms TX sleep.
local EXT_MSG = "UART extended test OK!"

-- ── LOOPBACK SELF-TESTS ─────────────────────────────────────────────────────
if MODE == "loopback" then
    local PORT   = 0
    local TX_PIN = "PA_25"
    local RX_PIN = "PA_26"
    local BAUD   = 115200

    local ok, u = pcall(uart.new, PORT, TX_PIN, RX_PIN, BAUD)
    if not ok then
        print("[uart] FATAL: uart.new failed: " .. tostring(u))
        print("FAIL: 1 test(s) failed")
        return
    end
    print("[uart] opened UART" .. PORT)
    u:set_loopback(true)

    -- Test 1: write + read
    print("[uart] test 1: write + read")
    u:flush_input()
    local TEST_STR = "Ameba UART loopback test!"
    local sent = u:write(TEST_STR)
    print("[uart] loopback send: " .. TEST_STR)
    check_val("write byte count", sent, #TEST_STR)
    sys.sleep_ms(5)
    local rx1 = u:read(#TEST_STR, 200)
    print("[uart] loopback recv: " .. tostring(rx1))
    check_val("read loopback", rx1, TEST_STR)

    -- Test 2: available() before and after write
    print("[uart] test 2: available()")
    u:flush_input()
    sys.sleep_ms(5)
    check_val("available before write", u:available(), 0)
    local ch2 = "X"
    u:write(ch2)
    print("[uart] loopback send: " .. ch2)
    sys.sleep_ms(5)
    check_val("available after write", u:available(), 1)
    u:flush_input()

    -- Test 3: flush_input clears buffered bytes
    print("[uart] test 3: flush_input")
    local str3 = "ABCDEFGH"
    u:write(str3)
    print("[uart] loopback send: " .. str3)
    sys.sleep_ms(5)
    u:flush_input()
    sys.sleep_ms(2)
    check_val("available after flush", u:available(), 0)

    -- Test 4: read_line stops at newline
    print("[uart] test 4: read_line")
    u:flush_input()
    local str4 = "line data test string\nmore data after newline"
    u:write(str4)
    print("[uart] loopback send: line data test string\\nmore data after newline")
    sys.sleep_ms(10)
    local line = u:read_line(64, 200)
    print("[uart] loopback recv line: " .. (line and line:gsub("\n", "\\n") or "nil"))
    check_val("read_line ends at newline", line, "line data test string\n")

    -- Test 5: resource recycle (close + reopen same port)
    print("[uart] test 5: resource recycle")
    u:close()
    local ok2, u2 = pcall(uart.new, PORT, TX_PIN, RX_PIN, BAUD)
    check_true("reopen after close", ok2, tostring(u2))
    if ok2 then
        u2:set_loopback(true)
        u2:flush_input()
        local ch5 = "Reopen verification byte"
        u2:write(ch5)
        print("[uart] loopback send: " .. ch5)
        sys.sleep_ms(5)
        local rx5 = u2:read(#ch5, 200)
        print("[uart] loopback recv: " .. tostring(rx5))
        check_val("write+read after reopen", rx5, ch5)
        u2:close()
    end

-- ── LOOPBACK BAUD-RATE SWEEP ─────────────────────────────────────────────────
elseif MODE == "loopback_baud" then
    local TX_PIN = "PA_25"
    local RX_PIN = "PA_26"
    local MSG    = "Baud rate loopback test!"
    local BAUD_LIST = {9600, 38400, 115200, 921600, 3000000}

    for _, baud in ipairs(BAUD_LIST) do
        print("[uart] baud " .. baud .. ": open")
        local ok, u = pcall(uart.new, 0, TX_PIN, RX_PIN, baud)
        if not ok then
            check_true("baud " .. baud .. " open", false, tostring(u))
        else
            u:set_loopback(true)
            u:flush_input()
            u:write(MSG)
            print("[uart] loopback send: [" .. MSG .. "] @ " .. baud)
            -- Slowest: 9600 baud, 24 chars ≈ 25 ms — 80 ms covers all rates.
            sys.sleep_ms(80)
            local rx = u:read(#MSG, 500)
            print("[uart] loopback recv: [" .. tostring(rx) .. "] @ " .. baud)
            check_val("baud " .. baud, rx, MSG)
            u:close()
        end
    end

-- ── LOOPBACK OPTS COMBOS ─────────────────────────────────────────────────────
elseif MODE == "loopback_opts" then
    local TX_PIN = "PA_25"
    local RX_PIN = "PA_26"
    local BAUD   = 115200
    -- All chars < 0x80 so the string is valid in 7-bit data mode too.
    local MSG    = "Options combo test data"
    local OPTS_LIST = {
        {label="7N1", opts={data_bits=7, parity="none",  stop_bits=1}},
        {label="8N2", opts={data_bits=8, parity="none",  stop_bits=2}},
        {label="8O1", opts={data_bits=8, parity="odd",   stop_bits=1}},
        {label="8E1", opts={data_bits=8, parity="even",  stop_bits=1}},
    }

    for _, cfg in ipairs(OPTS_LIST) do
        print("[uart] opts " .. cfg.label .. ": open")
        local ok, u = pcall(uart.new, 0, TX_PIN, RX_PIN, BAUD, cfg.opts)
        if not ok then
            check_true("opts " .. cfg.label .. " open", false, tostring(u))
        else
            u:set_loopback(true)
            u:flush_input()
            u:write(MSG)
            print("[uart] loopback send: [" .. MSG .. "] (" .. cfg.label .. ")")
            sys.sleep_ms(10)
            local rx = u:read(#MSG, 200)
            print("[uart] loopback recv: [" .. tostring(rx) .. "] (" .. cfg.label .. ")")
            check_val("opts " .. cfg.label, rx, MSG)
            u:close()
        end
    end

-- ── LOOPBACK PORT 1/2/3 ──────────────────────────────────────────────────────
elseif MODE == "loopback_port" then
    local BAUD = 115200
    local MSG  = "Port loopback test data!"
    -- Ports tested sequentially (each closes before the next opens) so the same
    -- pins can be safely reused across different UART controllers.
    local PORT_LIST = {
        {port=1, tx="PA_25", rx="PA_26"},
        {port=2, tx="PA_25", rx="PA_26"},
        {port=3, tx="PA_7",  rx="PA_6"},
    }

    for _, cfg in ipairs(PORT_LIST) do
        print("[uart] port " .. cfg.port .. ": open")
        local ok, u = pcall(uart.new, cfg.port, cfg.tx, cfg.rx, BAUD)
        if not ok then
            check_true("port " .. cfg.port .. " open", false, tostring(u))
        else
            u:set_loopback(true)
            u:flush_input()
            local pmsg = MSG .. " UART" .. cfg.port
            u:write(pmsg)
            print("[uart] loopback send: [" .. pmsg .. "]")
            sys.sleep_ms(10)
            local rx = u:read(#pmsg, 200)
            print("[uart] loopback recv: [" .. tostring(rx) .. "]")
            check_val("port " .. cfg.port .. " loopback", rx, pmsg)
            u:close()
        end
    end

-- ── TWO-BOARD SENDER (basic) ─────────────────────────────────────────────────
-- tx  → PA_25(TX)/PA_26(RX)   pair with rx  on the other board
-- tx2 → PA_7(TX)/PA_6(RX)     pair with rx2 on the other board
elseif MODE == "tx" or MODE == "tx2" then
    local TX_PIN = (MODE == "tx") and "PA_25" or "PA_7"
    local RX_PIN = (MODE == "tx") and "PA_26" or "PA_6"
    local PORT, BAUD = 0, 115200

    local ok, u = pcall(uart.new, PORT, TX_PIN, RX_PIN, BAUD)
    if not ok then
        print("[uart] FATAL: uart.new failed: " .. tostring(u))
        print("FAIL: 1 test(s) failed")
        return
    end
    print("[uart] [TX] opened UART" .. PORT)
    u:flush_input()

    -- Test 1: write + read(len) — basic echo
    print("[uart] [TX] test 1: write+read")
    local STR1 = "Ameba UART board-to-board test!"
    u:write(STR1)
    print("[uart] [TX] send: " .. STR1)
    sys.sleep_ms(50)
    local echo1 = u:read(#STR1, 3000)
    print("[uart] [TX] recv: " .. tostring(echo1))
    check_val("[TX] echo match", echo1, STR1)

    -- Test 2: available()
    print("[uart] [TX] test 2: available()")
    u:flush_input()
    local STR2 = "Echo availability check payload!"
    u:write(STR2)
    print("[uart] [TX] send: " .. STR2)
    sys.sleep_ms(50)
    local avail = u:available()
    print("[uart] [TX] available: " .. tostring(avail))
    check_val("[TX] available==1", avail, 1)
    local echo2 = u:read(#STR2, 1000)
    print("[uart] [TX] recv: " .. tostring(echo2))
    check_val("[TX] available echo", echo2, STR2)

    -- Test 3: read_line
    print("[uart] [TX] test 3: read_line")
    u:flush_input()
    local STR3 = "Newline termination test data\n"
    u:write(STR3)
    print("[uart] [TX] send: Newline termination test data\\n")
    sys.sleep_ms(50)
    local echo3 = u:read_line(64, 3000)
    print("[uart] [TX] recv line: "
          .. (echo3 and echo3:gsub("\n", "\\n") or "nil"))
    check_val("[TX] read_line echo", echo3, STR3)

    u:close()

-- ── TWO-BOARD ECHO (basic) ───────────────────────────────────────────────────
-- rx  → PA_7(TX)/PA_6(RX)     echoes tx
-- rx2 → PA_25(TX)/PA_26(RX)   echoes tx2
elseif MODE == "rx" or MODE == "rx2" then
    local TX_PIN = (MODE == "rx") and "PA_7" or "PA_25"
    local RX_PIN = (MODE == "rx") and "PA_6" or "PA_26"
    local PORT, BAUD = 0, 115200

    local ok, u = pcall(uart.new, PORT, TX_PIN, RX_PIN, BAUD)
    if not ok then
        print("[uart] FATAL: uart.new failed: " .. tostring(u))
        print("FAIL: 1 test(s) failed")
        return
    end
    print("[uart] [RX] opened UART" .. PORT)
    u:flush_input()

    -- Test 1: echo back 31-byte string
    -- 30 s gives ample time for the operator to start the TX test after RX.
    print("[uart] [RX] test 1: waiting for 31 bytes...")
    local d1 = u:read(31, 30000)
    print("[uart] [RX] recv: " .. tostring(d1))
    check_true("[RX] got 31 bytes", d1 and #d1 == 31,
               "got " .. (d1 and #d1 or 0))
    if d1 and #d1 > 0 then
        u:write(d1)
        print("[uart] [RX] send: (echoing " .. #d1 .. " bytes)")
    end

    -- Test 2: echo 32-byte string (for available() test on sender)
    print("[uart] [RX] test 2: waiting for 32 bytes...")
    local d2 = u:read(32, 30000)
    print("[uart] [RX] recv: " .. tostring(d2))
    check_true("[RX] got 32 bytes", d2 and #d2 == 32,
               "got " .. (d2 and #d2 or 0))
    if d2 and #d2 > 0 then
        u:write(d2)
        print("[uart] [RX] send: (echoing " .. #d2 .. " bytes)")
    end

    -- Test 3: echo one line (for read_line test on sender)
    print("[uart] [RX] test 3: waiting for line...")
    local line = u:read_line(64, 30000)
    print("[uart] [RX] recv line: "
          .. (line and line:gsub("\n", "\\n") or "nil"))
    check_true("[RX] got line", line and #line > 0,
               "len=" .. (line and #line or 0))
    if line and #line > 0 then
        u:write(line)
        print("[uart] [RX] send: (echoing line, " .. #line .. " bytes)")
    end

    u:close()

-- ── EXTENDED TWO-BOARD SENDER ────────────────────────────────────────────────
-- txe  → PA_25(TX)/PA_26(RX)  pair with rxe  (baud + opts + UART1/2/3)
-- txe2 → PA_7(TX)/PA_6(RX)    pair with rxe2 (roles swapped)
elseif MODE == "txe" or MODE == "txe2" then
    local TX_PIN = (MODE == "txe") and "PA_25" or "PA_7"
    local RX_PIN = (MODE == "txe") and "PA_26" or "PA_6"

    for _, st in ipairs(EXT_SUBTESTS) do
        print("[uart] [TXE] " .. st.label .. ": open port=" .. st.port
              .. " baud=" .. st.baud)
        local ok, u = pcall(uart.new, st.port, TX_PIN, RX_PIN, st.baud, st.opts)
        if not ok then
            check_true("[TXE] " .. st.label .. " open", false, tostring(u))
        else
            u:flush_input()
            -- Allow 200 ms for the echo side to open its port and enter read().
            sys.sleep_ms(200)
            u:write(EXT_MSG)
            print("[uart] [TXE] send: [" .. EXT_MSG .. "] (" .. st.label .. ")")
            sys.sleep_ms(80)
            local echo = u:read(#EXT_MSG, 3000)
            print("[uart] [TXE] recv: [" .. tostring(echo) .. "] (" .. st.label .. ")")
            check_val("[TXE] " .. st.label, echo, EXT_MSG)
            u:close()
            -- Allow 200 ms for both sides to finish close() before next open().
            sys.sleep_ms(200)
        end
    end

-- ── EXTENDED TWO-BOARD ECHO ──────────────────────────────────────────────────
-- rxe  → PA_7(TX)/PA_6(RX)    echoes txe
-- rxe2 → PA_25(TX)/PA_26(RX)  echoes txe2
elseif MODE == "rxe" or MODE == "rxe2" then
    local TX_PIN = (MODE == "rxe") and "PA_7" or "PA_25"
    local RX_PIN = (MODE == "rxe") and "PA_6" or "PA_26"

    for _, st in ipairs(EXT_SUBTESTS) do
        print("[uart] [RXE] " .. st.label .. ": open port=" .. st.port
              .. " baud=" .. st.baud)
        local ok, u = pcall(uart.new, st.port, TX_PIN, RX_PIN, st.baud, st.opts)
        if not ok then
            check_true("[RXE] " .. st.label .. " open", false, tostring(u))
        else
            u:flush_input()
            -- 30 s timeout for the first sub-test (operator may take time to
            -- start the sender); subsequent sub-tests are driven by the sender
            -- so 30 s is also a safe upper bound.
            local data = u:read(#EXT_MSG, 30000)
            print("[uart] [RXE] recv: [" .. tostring(data) .. "] (" .. st.label .. ")")
            check_true("[RXE] " .. st.label .. " got " .. #EXT_MSG .. " bytes",
                       data and #data == #EXT_MSG,
                       "got " .. (data and #data or 0))
            if data and #data > 0 then
                u:write(data)
                print("[uart] [RXE] send: (echoing " .. #data .. " bytes, "
                      .. st.label .. ")")
            end
            u:close()
            sys.sleep_ms(200)
        end
    end

else
    print("[uart] unknown MODE: " .. tostring(MODE))
    fail_count = fail_count + 1
end

if fail_count == 0 then
    print("success")
else
    print("FAIL: " .. fail_count .. " test(s) failed")
end

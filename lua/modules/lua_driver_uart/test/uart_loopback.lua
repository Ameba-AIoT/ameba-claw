local uart = require("uart")
local sys  = require("sys")

local PORT     = 0
local TX_PIN   = "PA_18"
local RX_PIN   = "PA_19"
local BAUD     = 115200
local TEST_STR = "Hello UART loopback!"

local function close_uart(u)
    if not u then return end
    local ok, err = pcall(u.close, u)
    if not ok then
        print("[uart_test] WARN: close failed: " .. tostring(err))
    end
end

local function test()
    print(string.format(
        "[uart_test] open UART%d tx=%s rx=%s baud=%d",
        PORT, TX_PIN, RX_PIN, BAUD))

    local ok, u = pcall(uart.new, PORT, TX_PIN, RX_PIN, BAUD)
    if not ok then
        print("[uart_test] ERROR: uart.new failed: " .. tostring(u))
        return false
    end

    u:set_loopback(true)
    u:flush_input()

    local ok_w, sent = pcall(u.write, u, TEST_STR)
    if not ok_w then
        print("[uart_test] ERROR: write failed: " .. tostring(sent))
        close_uart(u)
        return false
    end
    print(string.format("[uart_test] sent %d bytes", sent))

    sys.sleep_ms(10)

    local ok_r, rx = pcall(u.read, u, #TEST_STR, 200)
    if not ok_r then
        print("[uart_test] ERROR: read failed: " .. tostring(rx))
        close_uart(u)
        return false
    end

    u:set_loopback(false)
    close_uart(u)

    if rx == TEST_STR then
        print("[uart_test] success")
        return true
    else
        print("[uart_test] FAIL: expected=" .. TEST_STR
              .. " got=" .. tostring(rx))
        return false
    end
end

local ok, result = pcall(test)
if not ok then
    print("[uart_test] ERROR: " .. tostring(result))
end

local uart = require("uart")
local sys  = require("sys")

local PORT   = 0
local TX_PIN = "PA_18"
local RX_PIN = "PA_19"
local BAUD   = 115200

local function close_uart(u)
    if not u then return end
    local ok, err = pcall(u.close, u)
    if not ok then
        print("[uart_at] WARN: close failed: " .. tostring(err))
    end
end

local function main()
    print(string.format(
        "[uart_at] open UART%d tx=%s rx=%s baud=%d",
        PORT, TX_PIN, RX_PIN, BAUD))

    local ok, u = pcall(uart.new, PORT, TX_PIN, RX_PIN, BAUD)
    if not ok then
        print("[uart_at] ERROR: uart.new failed: " .. tostring(u))
        return
    end

    local success, err = pcall(u.flush_input, u)
    if not success then
        print("[uart_at] ERROR: flush_input failed: " .. tostring(err))
        close_uart(u)
        return
    end

    local request = "AT\r\n"
    success, err = pcall(u.write, u, request)
    if not success then
        print("[uart_at] ERROR: write failed: " .. tostring(err))
        close_uart(u)
        return
    end
    print("[uart_at] sent request: " .. request:gsub("[\r\n]+", "\\r\\n"))

    sys.sleep_ms(50)

    local ok_line, reply = pcall(u.read_line, u, 128, 5000)
    if not ok_line then
        print("[uart_at] ERROR: read_line failed: " .. tostring(reply))
        close_uart(u)
        return
    end

    if #reply > 0 then
        local trimmed = reply:gsub("[\r\n]+$", "")
        print("[uart_at] received line: " .. trimmed)
    else
        print("[uart_at] no line received within 5000 ms")
    end

    print("[uart_at] polling RX buffer for 20 seconds...")
    for _ = 1, 1000 do
        local ok_avail, available = pcall(u.available, u)
        if not ok_avail then
            print("[uart_at] ERROR: available failed: " .. tostring(available))
            close_uart(u)
            return
        end

        if available > 0 then
            local read_len = available
            if read_len > 64 then read_len = 64 end

            local ok_chunk, chunk = pcall(u.read, u, read_len, 0)
            if not ok_chunk then
                print("[uart_at] ERROR: read failed: " .. tostring(chunk))
                close_uart(u)
                return
            end

            if #chunk > 0 then
                print(string.format(
                    "[uart_at] rx chunk (%d bytes): %q",
                    #chunk, chunk))
            end
        end

        sys.sleep_ms(20)
    end

    close_uart(u)
    print("[uart_at] done")
end

main()

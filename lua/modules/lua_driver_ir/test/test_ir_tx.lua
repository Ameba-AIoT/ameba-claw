-- IR TX cross-test: sends NEC frame via PA_25 -> Dplus RX PA_27 (PINMUX_S1).
-- Wiring: RTL8721F TX PA_25 -> Dplus RX PA_27 (Dplus PINMUX_S1) @example: raw_ir_rx_nec_interrupt
-- Trigger: AT+CLAW=ir,tx
local ir = require("ir")
local TX_PIN = "PA_25"
local ADDR   = 0x12
local CMD    = 0x34

-- NEC protocol encoder (pure Lua, timing in microseconds)
local function nec_encode(addr, cmd)
    local syms = {}
    local function push(lv, dur)
        syms[#syms + 1] = {level = lv, duration_us = dur}
    end
    local function encode_byte(b)
        for bit = 0, 7 do
            push(1, 560)
            if ((b >> bit) & 1) == 1 then push(0, 1690) else push(0, 560) end
        end
    end
    push(1, 9000) push(0, 4500)
    encode_byte(addr & 0xFF)
    encode_byte((~addr) & 0xFF)
    encode_byte(cmd & 0xFF)
    encode_byte((~cmd) & 0xFF)
    push(1, 560)
    return syms
end

print("[ir_tx] test start")
local dev
local ok, result = pcall(ir.new, TX_PIN, nil)
if not ok then
    print("[ir_tx] ERROR: ir.new failed: " .. tostring(result))
    return
end
dev = result

local info = dev:info()
print(string.format("[ir_tx] tx=%s carrier=%dHz", TX_PIN, info.carrier_hz))

local symbols = nec_encode(ADDR, CMD)
local ok_raw, err_raw = pcall(dev.send_raw, dev, symbols)
if ok_raw then
    print(string.format("[ir_tx] NEC send addr=0x%02X cmd=0x%02X done (%d symbols)",
        ADDR, CMD, #symbols))
else
    print("[ir_tx] ERROR: send_raw: " .. tostring(err_raw))
    dev:close()
    return
end

dev:close()
print("[ir_tx] success")

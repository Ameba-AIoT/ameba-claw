-- IR RX cross-test: RTL8721F PA_26 receives NEC frame from Dplus PA_26 TX (PINMUX_S1).
-- Wiring: Dplus TX PA_26 @example:raw_ir_tx_nec_polling -> RTL8721F RX PA_26 (direct wire)
-- Trigger: AT+CLAW=ir,rx
local ir = require("ir")
local RX_PIN     = "PA_26"
local TIMEOUT_MS = 15000

-- NEC timing constants (µs)
local LEADER_MARK  = 9000
local LEADER_SPACE = 4500
local BIT_MARK     = 560
local ZERO_SPACE   = 560
local ONE_SPACE    = 1690
-- Leader tolerances are loose: carrier accumulation can under-count the leader
-- burst by ~700 µs due to FIFO batching at 1 MHz sample clock.
local LEADER_MARK_TOL  = 2000
local LEADER_SPACE_TOL = 700
local DATA_TOL         = 400

local function within(v, target, tol)
    return math.abs(v - target) <= tol
end

-- Decode NEC from demodulated symbols {level, duration_us}.
-- Returns addr, cmd on success; nil, errmsg on failure.
local function nec_decode(syms)
    if #syms < 67 then
        return nil, string.format("too few symbols: %d (need 67)", #syms)
    end
    if not within(syms[1].duration_us, LEADER_MARK, LEADER_MARK_TOL) then
        return nil, string.format("bad leader mark: %d us", syms[1].duration_us)
    end
    if not within(syms[2].duration_us, LEADER_SPACE, LEADER_SPACE_TOL) then
        return nil, string.format("bad leader space: %d us", syms[2].duration_us)
    end
    local bits = 0
    local idx = 3
    for bit = 0, 31 do
        local mk, sp = syms[idx], syms[idx + 1]
        if not mk or not sp then
            return nil, string.format("missing symbol at bit %d", bit)
        end
        if not within(mk.duration_us, BIT_MARK, DATA_TOL) then
            return nil, string.format("bad mark at bit %d: %d us", bit, mk.duration_us)
        end
        local bval
        if within(sp.duration_us, ZERO_SPACE, DATA_TOL) then
            bval = 0
        elseif within(sp.duration_us, ONE_SPACE, DATA_TOL) then
            bval = 1
        else
            return nil, string.format("bad space at bit %d: %d us", bit, sp.duration_us)
        end
        bits = bits | (bval << bit)
        idx = idx + 2
    end
    local addr     = bits & 0xFF
    local addr_inv = (bits >> 8) & 0xFF
    local cmd      = (bits >> 16) & 0xFF
    local cmd_inv  = (bits >> 24) & 0xFF
    local ca = addr ~ addr_inv
    local cc = cmd  ~ cmd_inv
    if ca ~= 0xFF then
        return nil, string.format("addr checksum: 0x%02X^0x%02X=0x%02X", addr, addr_inv, ca)
    end
    if cc ~= 0xFF then
        return nil, string.format("cmd checksum: 0x%02X^0x%02X=0x%02X", cmd, cmd_inv, cc)
    end
    return addr, cmd
end

print("[ir_rx] test start - waiting for NEC frame (" .. TIMEOUT_MS / 1000 .. "s timeout)")

local dev
local ok, result = pcall(ir.new, nil, RX_PIN)
if not ok then
    print("[ir_rx] ERROR: ir.new failed: " .. tostring(result))
    return
end
dev = result

local symbols, err = dev:receive(TIMEOUT_MS)
if not symbols then
    print("[ir_rx] receive: " .. tostring(err))
    dev:close()
    return
end

print(string.format("[ir_rx] received %d symbols", #symbols))

local addr, cmd = nec_decode(symbols)
if not addr then
    print("[ir_rx] decode failed: " .. tostring(cmd))
    dev:close()
    return
end

print(string.format("[ir_rx] NEC decoded: addr=0x%02X cmd=0x%02X", addr, cmd))
dev:close()
if addr == 0x12 and cmd == 0x34 then
    print("[ir_rx] success")
else
    print(string.format("[ir_rx] unexpected data: addr=0x%02X cmd=0x%02X (expected 0x12 0x34)",
        addr, cmd))
end

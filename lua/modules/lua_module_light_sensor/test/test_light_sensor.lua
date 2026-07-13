-- test_light_sensor.lua
-- LM393 LDR light sensor test for AmebaGreen2 (RTL8721F).
--
-- Usage:
--   AT+CLAW=light_sensor              -- pin from board.json "light_sensor" device
--   AT+CLAW=light_sensor,PA_26        -- explicit pin override
--   AT+CLAW=light_sensor,PA_26,30     -- 30 samples x 200ms

local light_sensor = require("light_sensor")
local sys   = require("sys")
local file  = require("file")
local cjson = require("cjson")

-- Read the DO pin for the light_sensor device from board.json.
-- Returns the pin string, or nil if the device entry is absent.
local function board_pin()
    local ok, s = pcall(file.read, "board.json")
    if not (ok and s) then return nil end
    local ok2, cfg = pcall(cjson.decode, s)
    if not (ok2 and cfg and cfg.devices) then return nil end
    for _, d in ipairs(cfg.devices) do
        if d.id == "light_sensor" and d.params and d.params.pin then
            return d.params.pin
        end
    end
    return nil
end

local a     = type(args) == "table" and args or {}
-- Priority: caller-supplied do_pin > board.json device entry
local PIN   = (type(a.do_pin) == "string" and a.do_pin) or board_pin()
if not PIN then
    error("light_sensor: no do_pin in args and no 'light_sensor' device in board.json")
end
local COUNT = type(a.count) == "number" and a.count or 10

local sensor

local function cleanup()
    if sensor then
        pcall(function() sensor:close() end)
        sensor = nil
    end
end

local function assert_error(fn, expected_fragment)
    local ok, err = pcall(fn)
    if ok then
        error("expected error containing '" .. expected_fragment .. "' but call succeeded")
    end
    if not string.find(tostring(err), expected_fragment, 1, true) then
        error("expected error '" .. expected_fragment .. "' but got: " .. tostring(err))
    end
end

local function run()
    -- ── 1. new() ──────────────────────────────────────────────────────────
    print(string.format("[light] new(do_pin=%s)", PIN))
    sensor = light_sensor.new({ do_pin = PIN })

    -- ── 2. name() ─────────────────────────────────────────────────────────
    local n = sensor:name()
    assert(n == "lm393_ldr", "name() expected 'lm393_ldr', got: " .. tostring(n))
    print("[light] name(): " .. n .. "  OK")

    -- ── 3. read() → 0 or 1 ────────────────────────────────────────────────
    local level = sensor:read()
    assert(level == 0 or level == 1, "read() must return 0 or 1, got: " .. tostring(level))
    local state_str = (level == 0) and "bright" or "dark"
    print(string.format("[light] read(): %d (%s)  OK", level, state_str))

    -- ── 4. is_bright() and is_dark() ──────────────────────────────────────
    local bright = sensor:is_bright()
    local dark   = sensor:is_dark()
    assert(type(bright) == "boolean", "is_bright() must return boolean")
    assert(type(dark)   == "boolean", "is_dark() must return boolean")
    -- bright and dark must be logically consistent with read()
    assert(bright == (level == 0), "is_bright() inconsistent with read()")
    assert(dark   == (level == 1), "is_dark() inconsistent with read()")
    print(string.format("[light] is_bright()=%s  is_dark()=%s  OK",
                        tostring(bright), tostring(dark)))

    -- ── 5. repeated sampling ──────────────────────────────────────────────
    print(string.format("[light] repeated sampling (%d reads):", COUNT))
    for i = 1, COUNT do
        sys.sleep_ms(200)
        local ok_r, v = pcall(function() return sensor:read() end)
        if ok_r then
            print(string.format("  [%2d] level=%d (%s)", i, v,
                                v == 0 and "bright" or "dark"))
        else
            print(string.format("  [%2d] ERROR: %s", i, tostring(v)))
        end
    end
    print("[light] repeated sampling OK")

    -- ── 6. close() ────────────────────────────────────────────────────────
    sensor:close()
    print("[light] close()  OK")

    -- ── 7. method call on closed handle must error ────────────────────────
    assert_error(function() sensor:read()      end, "invalid or closed")
    assert_error(function() sensor:is_bright() end, "invalid or closed")
    assert_error(function() sensor:is_dark()   end, "invalid or closed")
    assert_error(function() sensor:name()      end, "invalid or closed")
    print("[light] closed-handle errors: all OK")

    sensor = nil

    -- ── 8. invalid pin must error ─────────────────────────────────────────
    assert_error(function()
        light_sensor.new({ do_pin = "PZ_99" })
    end, "invalid pin")
    print("[light] invalid pin error: OK")

    -- ── 9. re-open and read after close ───────────────────────────────────
    sensor = light_sensor.new({ do_pin = PIN })
    local v2 = sensor:read()
    assert(v2 == 0 or v2 == 1, "re-open read() failed")
    print(string.format("[light] re-open: level=%d  OK", v2))
    sensor:close()
    sensor = nil

    print("success")
end

local ok, err = pcall(run)
cleanup()
if not ok then
    error(tostring(err))
end

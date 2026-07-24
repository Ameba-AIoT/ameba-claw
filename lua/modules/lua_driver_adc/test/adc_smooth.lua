-- adc_smooth.lua
-- Continuous EMA-smoothed sampling on PA_13 / PA_14.
--
-- Optional globals:
--   ADC_ALPHA    = 0.2   -- smoothing factor (smaller = smoother; 0 < alpha <= 1)
--   ADC_INTERVAL = 100   -- sample interval in ms
--   ADC_COUNT    = 0     -- number of samples (0 = infinite loop)
--
-- Trigger: AT+CLAW=adc_smooth

local adc = require("adc")
local sys = require("sys")

local PINS     = {"PA_13", "PA_14"}
local ALPHA    = ADC_ALPHA    or 0.2
local INTERVAL = ADC_INTERVAL or 100
local COUNT    = ADC_COUNT    or 0   -- 0 = infinite

local ch = adc.new(table.unpack(PINS))

-- EMA state table: key = hw channel id
local smooth = {}

local i = 0
while COUNT == 0 or i < COUNT do
    local r = ch:read()  -- {[ch_id]=mv, ...}

    for ch_id, mv in pairs(r) do
        local prev = smooth[ch_id]
        if prev == nil then
            smooth[ch_id] = mv
        else
            smooth[ch_id] = ALPHA * mv + (1 - ALPHA) * prev
        end
    end

    -- Sort by channel id for aligned output
    local ids = {}
    for ch_id in pairs(smooth) do ids[#ids + 1] = ch_id end
    table.sort(ids)

    local parts = {}
    for _, ch_id in ipairs(ids) do
        parts[#parts + 1] = string.format("ch%d=%4d mV", ch_id, math.floor(smooth[ch_id] + 0.5))
    end
    print(string.format("[adc_smooth] %s", table.concat(parts, "  ")))

    i = i + 1
    if COUNT == 0 or i < COUNT then
        sys.sleep_ms(INTERVAL)
    end
end

ch:close()
print("[adc_smooth] done")

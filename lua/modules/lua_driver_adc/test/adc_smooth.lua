-- adc_smooth.lua
-- 循环采样 PA_13 / PA_14，打印每通道 EMA 平滑值。
--
-- 用法：
--   ADC_ALPHA    = 0.2   -- 平滑系数（越小越平滑，0<alpha<=1）
--   ADC_INTERVAL = 100   -- 采样间隔 ms
--   ADC_COUNT    = 0     -- 采样次数（0 = 无限循环）
--
-- 触发：AT+CLAW=adc_smooth

local adc = require("adc")
local sys = require("sys")

local PINS     = {"PA_13", "PA_14"}
local ALPHA    = ADC_ALPHA    or 0.2
local INTERVAL = ADC_INTERVAL or 100
local COUNT    = ADC_COUNT    or 0   -- 0 = infinite

local ch = adc.new(table.unpack(PINS))

-- EMA 状态表：key = hw channel id
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

    -- 按 channel id 排序输出，格式对齐
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

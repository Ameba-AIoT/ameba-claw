-- SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
-- SPDX-License-Identifier: Apache-2.0

-- test_led_strip.lua — WS2812 LED strip test.
--
-- Reads board.json (device id "led_strip") for spi index, mosi pin, and
-- pixel count.  Globals NUM_LEDS and MODE may be injected by the C AT runner:
--   NUM_LEDS  — override pixel count
--   MODE      — "demo" (default) or "loop"
--
-- Trigger:
--   AT+CLAW=led[,<n>]       one-shot demo with <n> pixels
--   AT+CLAW=led,loop[,<n>]  continuous rainbow animation
--   AT+CLAW=led,off         stop loop + turn strip off

local mode = MODE or "demo"

-- Read board.json to get led_strip device config.
local function board_cfg()
    local ok, s = pcall(file.read, "board.json")
    if ok and s then
        local ok2, cfg = pcall(cjson.decode, s)
        if ok2 and cfg and cfg.devices then
            for _, d in ipairs(cfg.devices) do
                if d.id == "led_strip" and d.params then
                    return d.params.spi or 1,
                           d.params.mosi or "PB_8",
                           d.params.count or 15
                end
            end
        end
    end
    return 1, "PB_8", 15
end

local spi_idx, mosi, n = board_cfg()
if NUM_LEDS then n = NUM_LEDS end

print(string.format("[led] spi=%d mosi=%s pixels=%d mode=%s",
                    spi_idx, mosi, n, mode))

local ok, err = pcall(function()
    local strip = led_strip.new({spi = spi_idx, mosi = mosi, count = n})

    -- Rainbow: shift hue evenly across all pixels, rotated by `off` degrees.
    local function rainbow(off)
        for i = 1, n do
            strip:set_pixel_hsv(i, (i * 360 // n + off) % 360, 200, 40)
        end
        strip:show()
    end

    if mode == "loop" then
        print("[led] loop running — AT+CLAW=led,off to stop")
        local t = 0
        while not led_strip.stop_requested() do
            rainbow(t)
            t = (t + 4) % 360
            sys.sleep_ms(50)
        end
        strip:clear()
        strip:show()
        strip:close()
        print("[led] loop stopped, strip off")
    else
        -- one-shot demo: solid colours then rainbow sweep
        local solids = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {50, 50, 50}}
        for _, c in ipairs(solids) do
            strip:fill(c[1], c[2], c[3])
            strip:show()
            sys.sleep_ms(400)
        end
        for t = 0, 355, 5 do
            rainbow(t)
            sys.sleep_ms(40)
        end
        strip:clear()
        strip:show()
        strip:close()
        print("[led] demo done")
    end
end)

if ok then
    print("[led_strip] PASS")
else
    print("[led_strip] FAIL: " .. tostring(err))
end

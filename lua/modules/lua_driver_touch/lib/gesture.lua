-- gesture.lua
-- Pure-Lua gesture recogniser layered on top of the `touch` driver's raw
-- down / move / up event stream (see docs/touch.md).
--
-- The C driver only reports *where* the finger is; interpreting a stroke as a
-- tap, long-press or swipe is policy, so it lives here in Lua where it is cheap
-- to tune and does not cost a reflash. The recogniser is a tiny state machine
-- fed one frame at a time:
--
--     local gesture = dofile("rolfs:/lib/gesture.lua")
--     local touch   = require("touch")
--     touch.init("touch_gt911")
--     local g = gesture.new()
--     while true do
--         local ev = touch.get_event()      -- may be nil when nothing happened
--         local gz = g:feed(ev)             -- MUST be called every frame, even on nil
--         if gz then
--             if gz.kind == "tap"        then ... end
--             if gz.kind == "long_press" then ... end
--             if gz.kind == "swipe"      then ... gz.dir ... end
--         end
--     end
--
-- IMPORTANT: feed() must be called on *every* loop iteration, including frames
-- where get_event() returned nil. Long-press is detected by elapsed wall-clock
-- time while the finger is held still, so the recogniser needs a heartbeat to
-- notice the deadline has passed even when no new touch event arrives.
--
-- Recognised gestures (no double-tap — see docs/touch.md for the rationale):
--   { kind = "tap",        x, y }              short contact, little movement
--   { kind = "long_press", x, y }              held > long_press_ms without moving
--   { kind = "swipe", dir, x, y, dx, dy }      released after moving >= swipe_min
--                                              dir is "up" / "down" / "left" / "right"
-- At most one gesture is returned per feed() call.

local ok, sys = pcall(require, "sys")

local gesture = {}
gesture.__index = gesture

local DEFAULT_LONG_PRESS_MS = 1000  -- hold this long (still) -> long_press
local DEFAULT_SWIPE_MIN     = 30    -- travel this many px    -> swipe (else tap)

-- Classify a displacement vector into one of the four cardinal directions by
-- its dominant axis. Screen y grows downward, so +dy is "down".
local function dir_of(dx, dy)
    if math.abs(dx) >= math.abs(dy) then
        return (dx > 0) and "right" or "left"
    else
        return (dy > 0) and "down" or "up"
    end
end

-- Chebyshev distance (max of the two axes) — matches the driver's move
-- threshold model and avoids a sqrt on every check.
local function reach(dx, dy)
    local ax, ay = math.abs(dx), math.abs(dy)
    return (ax > ay) and ax or ay
end

-- opts (all optional):
--   long_press_ms : hold duration for a long_press           (default 1000)
--   swipe_min     : min travel in px to count as a swipe      (default 30)
--   now           : function returning a monotonic ms clock   (default sys.millis)
function gesture.new(opts)
    opts = opts or {}
    local self = setmetatable({}, gesture)
    self.long_ms   = opts.long_press_ms or DEFAULT_LONG_PRESS_MS
    self.swipe_min = opts.swipe_min     or DEFAULT_SWIPE_MIN
    self.now       = opts.now or (ok and sys and sys.millis)
    if not self.now then
        error("gesture.new: no time source (sys.millis unavailable); pass opts.now")
    end
    self.state = "idle"   -- "idle" | "pressed"
    return self
end

-- Drop any in-progress stroke. Call after touch.deinit()/init() so a finger
-- that was down across the reset does not produce a phantom gesture.
function gesture:reset()
    self.state = "idle"
    self.long_fired = false
end

-- Feed one frame. `ev` is a touch event table {type,x,y,dx,dy} or nil.
-- Returns a gesture table (see file header) or nil.
function gesture:feed(ev)
    local now = self.now()

    if ev then
        local ty = ev.type
        if ty == "down" then
            self.state      = "pressed"
            self.sx, self.sy = ev.x, ev.y   -- stroke start
            self.lx, self.ly = ev.x, ev.y   -- last seen position
            self.t0         = now
            self.long_fired = false
            return nil
        elseif ty == "up" then
            if self.state ~= "pressed" then
                self.state = "idle"
                return nil
            end
            self.state = "idle"
            local dx, dy = ev.x - self.sx, ev.y - self.sy
            if reach(dx, dy) >= self.swipe_min then
                return { kind = "swipe", dir = dir_of(dx, dy),
                         x = ev.x, y = ev.y, dx = dx, dy = dy }
            elseif self.long_fired then
                return nil                  -- long_press already emitted while held
            else
                return { kind = "tap", x = ev.x, y = ev.y }
            end
        elseif ty == "move" then
            if self.state == "pressed" then
                self.lx, self.ly = ev.x, ev.y
            end
            -- fall through to the long-press deadline check below
        end
    end

    -- Held-still long-press: fires once, mid-stroke, without waiting for release.
    if self.state == "pressed" and not self.long_fired then
        if (now - self.t0) >= self.long_ms then
            if reach(self.lx - self.sx, self.ly - self.sy) < self.swipe_min then
                self.long_fired = true
                return { kind = "long_press", x = self.lx, y = self.ly }
            end
        end
    end

    return nil
end

return gesture

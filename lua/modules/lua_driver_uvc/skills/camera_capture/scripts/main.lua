local uvc   = require("usb_uvc")
local sys   = require("sys")
local cjson = require("cjson")
local file  = require("file")

function run(args)
    if type(args) ~= "table" then args = {} end
    local filename  = type(args.filename)   == "string" and args.filename  or "capture.jpg"
    local timeout   = tonumber(args.timeout_ms) or 10000
    local width     = tonumber(args.width)  or 640
    local height    = tonumber(args.height) or 480
    local fps       = tonumber(args.fps)    or 15
    local buf_size  = tonumber(args.buf_size) or 153600

    sys.sleep_ms(500)  -- let WiFi finish any pending IPC before USB host init
    local ok, err = uvc.init()
    if not ok then
        return cjson.encode({error = "init failed: " .. tostring(err)})
    end

    local ready, reason = uvc.wait_ready(timeout)
    if not ready then
        uvc.deinit()
        return cjson.encode({error = "camera not ready: " .. tostring(reason)})
    end

    local ok2, err2 = uvc.set_param({
        width = width, height = height, fps = fps,
        format = "mjpeg", buf_size = buf_size
    })
    if not ok2 then
        uvc.deinit()
        return cjson.encode({error = "set_param failed: " .. tostring(err2)})
    end

    local ok3, err3 = uvc.stream_on()
    if not ok3 then
        uvc.deinit()
        return cjson.encode({error = "stream_on failed: " .. tostring(err3)})
    end

    local frame, ferr = uvc.get_frame(3000)
    uvc.stream_off()
    uvc.deinit()

    if not frame then
        return cjson.encode({error = "get_frame failed: " .. tostring(ferr)})
    end

    local ok4, werr = file.write(filename, frame)
    if not ok4 then
        return cjson.encode({error = "save failed: " .. tostring(werr)})
    end

    return cjson.encode({
        ok       = true,
        filename = filename,
        bytes    = #frame,
        format   = "jpeg",
        width    = width,
        height   = height,
    })
end

local msc   = require("usb_msc")
local sys   = require("sys")
local cjson = require("cjson")

local function ensure_mounted()
    local ok, err = msc.init()
    if not ok then return nil, "init failed: " .. tostring(err) end
    local rdy, rerr = msc.wait_ready(8000)
    if not rdy then return nil, "drive not ready: " .. tostring(rerr) end
    local drv, merr = msc.mount()
    if not drv then return nil, "mount failed: " .. tostring(merr) end
    return drv
end

function run(args)
    if type(args) ~= "table" then args = {} end
    local action = tostring(args.action or "")

    sys.sleep_ms(200)

    if action == "list" then
        local drv, err = ensure_mounted()
        if not drv then return cjson.encode({error = err}) end
        local dir = (type(args.dir) == "string" and args.dir ~= "") and args.dir or drv
        local entries, lerr = msc.list_dir(dir)
        if not entries then
            return cjson.encode({error = "list failed: " .. tostring(lerr)})
        end
        local result = {}
        for _, e in ipairs(entries) do
            table.insert(result, {name = e.name, size = e.size, is_dir = e.is_dir})
        end
        return cjson.encode({ok = true, dir = dir, count = #result, entries = result})

    elseif action == "write" then
        local path = type(args.path) == "string" and args.path or ""
        if path == "" then return cjson.encode({error = "path required"}) end
        local data = type(args.data) == "string" and args.data or ""
        if data == "" then return cjson.encode({error = "data required"}) end
        local drv, err = ensure_mounted()
        if not drv then return cjson.encode({error = err}) end
        local wok, werr = msc.write_file(path, data)
        if not wok then
            return cjson.encode({error = "write failed: " .. tostring(werr)})
        end
        return cjson.encode({ok = true, path = path, bytes = #data})

    elseif action == "read" then
        local path = type(args.path) == "string" and args.path or ""
        if path == "" then return cjson.encode({error = "path required"}) end
        local drv, err = ensure_mounted()
        if not drv then return cjson.encode({error = err}) end
        local content, rerr = msc.read_file(path)
        if not content then
            return cjson.encode({error = "read failed: " .. tostring(rerr)})
        end
        return cjson.encode({ok = true, path = path, bytes = #content, data = content})

    elseif action == "delete" then
        local path = type(args.path) == "string" and args.path or ""
        if path == "" then return cjson.encode({error = "path required"}) end
        local drv, err = ensure_mounted()
        if not drv then return cjson.encode({error = err}) end
        local dok, derr = msc.remove(path)
        if not dok then
            return cjson.encode({error = "delete failed: " .. tostring(derr)})
        end
        return cjson.encode({ok = true, path = path})

    else
        return cjson.encode({
            error = "unknown action: " .. action,
            usage = "action must be one of: list, write, read, delete"
        })
    end
end

local cap = require("cap")
local cjson = require("cjson")
local file = require("file")

local SKILLS_DIR = "vfs:/skills"
local TIMEOUT_S = 10

local function http_get(url)
    local ok, result = cap.call("http_request", cjson.encode({
        method = "GET",
        url = url,
        timeout = TIMEOUT_S,
    }))
    if not ok then
        local msg = tostring(result)
        if msg:find("allowlist", 1, true) then
            error(msg .. "  ->  add the server IP/domain to the HTTP Request allowlist (or set to *)")
        end
        error("http_request failed: " .. msg)
    end
    local resp = cjson.decode(result)
    if resp.error then error(tostring(resp.error)) end
    return resp.status_code, resp.body
end

local function body_to_str(body)
    if type(body) == "table" then return cjson.encode(body) end
    return tostring(body or "")
end

local function ensure_dir(path)
    if not file.exists(path) then
        local ok, err = file.mkdir(path)
        if not ok then error("mkdir failed: " .. path .. "  " .. tostring(err)) end
    end
end

local function fetch_and_write(url, dest_path)
    local status, body = http_get(url)
    if status ~= 200 then
        error(string.format("download failed (HTTP %d): %s", status, url))
    end
    local ok, err = file.write(dest_path, body_to_str(body))
    if not ok then error("file.write failed: " .. dest_path .. "  " .. tostring(err)) end
end

local function validate_filename(name, group)
    if type(name) ~= "string" or name == "" then
        error("invalid filename in extra_files." .. group)
    end
    if name:find("/", 1, true) or name:find("\\", 1, true) then
        error("extra_files." .. group .. " must be filenames only: " .. name)
    end
end

local function validate_skill_name(name)
    if not name or name == "" then error("args.skill_name is required") end
    if not name:match("^[A-Za-z0-9_%-]+$") then
        error("skill_name must match ^[A-Za-z0-9_-]+$")
    end
end

function run(args)
    local a = type(args) == "table" and args or {}

    local function str_arg(key, default)
        local v = a[key]
        return (type(v) == "string" and v ~= "") and v or default
    end

    local base_url = str_arg("base_url", nil)
    if not base_url then error("args.base_url is required (e.g. http://192.168.50.100:8080)") end
    base_url = base_url:gsub("/$", "")

    local action = str_arg("action", "fetch_metadata")
    local skill_name = str_arg("skill_name", nil)
    validate_skill_name(skill_name)

    if action == "fetch_metadata" then
        local url = base_url .. "/" .. skill_name .. "/_metadata.json"
        local status, body = http_get(url)
        if status == 404 then error("skill not found: " .. skill_name) end
        if status ~= 200 then error(string.format("HTTP %d for %s", status, url)) end
        print(body_to_str(body))

    elseif action == "install" then
        local meta_name = str_arg("skill_name_from_metadata", nil)
        if meta_name and meta_name ~= skill_name then
            error(string.format("name mismatch: requested=%s metadata=%s", skill_name, meta_name))
        end
        local skill_dir = SKILLS_DIR .. "/" .. skill_name
        ensure_dir(skill_dir)
        fetch_and_write(base_url .. "/" .. skill_name .. "/SKILL.md", skill_dir .. "/SKILL.md")
        local extra = a.extra_files
        if type(extra) == "table" then
            for group, files in pairs(extra) do
                if type(files) ~= "table" then
                    error("extra_files." .. tostring(group) .. " must be an array")
                end
                if #files > 0 then
                    local group_dir = skill_dir .. "/" .. group
                    ensure_dir(group_dir)
                    for _, fname in ipairs(files) do
                        validate_filename(fname, group)
                        fetch_and_write(
                            base_url .. "/" .. skill_name .. "/" .. group .. "/" .. fname,
                            group_dir .. "/" .. fname
                        )
                    end
                end
            end
        end
        print("Installed: " .. skill_name)
        print("Path: " .. skill_dir)
    else
        error("unsupported action: " .. tostring(action))
    end
end

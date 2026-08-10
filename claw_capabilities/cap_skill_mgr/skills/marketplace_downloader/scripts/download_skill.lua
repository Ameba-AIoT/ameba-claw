local cap = require("cap")
local cjson = require("cjson")
local file = require("file")

local MARKET_BASE = "https://raw.githubusercontent.com/Ameba-AIoT/ameba-claw-skills-marketplace/main"
local SKILLS_DIR = "vfs:/skills"
local TIMEOUT_S = 20

local function http_get(url)
    local ok, result = cap.call("http_request", cjson.encode({
        method = "GET",
        url = url,
        timeout = TIMEOUT_S,
    }))
    if not ok then
        local msg = tostring(result)
        if msg:find("allowlist", 1, true) then
            error(msg .. "  ->  add github.com to the HTTP Request allowlist")
        end
        error("http_request failed: " .. msg)
    end
    local resp = cjson.decode(result)
    if resp.error then
        local msg = tostring(resp.error)
        if msg:find("allowlist", 1, true) then
            error(msg .. "  ->  add github.com to the HTTP Request allowlist")
        end
        error(msg)
    end
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
    local ok, result = cap.call("http_request", cjson.encode({
        method = "GET",
        url = url,
        timeout = TIMEOUT_S,
        save_path = dest_path,
        max_file_bytes = 10 * 1024 * 1024,
    }))
    if not ok then error("http_request failed: " .. tostring(result)) end
    local resp = cjson.decode(result)
    if resp.error then error(tostring(resp.error)) end
    if resp.status_code ~= 200 then
        error(string.format("download failed (HTTP %d): %s", resp.status_code, url))
    end
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

    local action = str_arg("action", "fetch_metadata")
    local skill_name = str_arg("skill_name", nil)
    validate_skill_name(skill_name)

    if action == "fetch_metadata" then
        local url = MARKET_BASE .. "/" .. skill_name .. "/_metadata.json"
        local status, body = http_get(url)
        if status == 404 then
            error("skill not found: " .. skill_name .. "  (use marketplace_search to find available skills)")
        end
        if status ~= 200 then
            error(string.format("HTTP %d fetching metadata for %s", status, skill_name))
        end
        print(body_to_str(body))

    elseif action == "install" then
        local meta_name = str_arg("skill_name_from_metadata", nil)
        if meta_name and meta_name ~= skill_name then
            error(string.format("name mismatch: requested=%s metadata=%s", skill_name, meta_name))
        end
        local skill_dir = SKILLS_DIR .. "/" .. skill_name
        ensure_dir(skill_dir)
        fetch_and_write(MARKET_BASE .. "/" .. skill_name .. "/SKILL.md", skill_dir .. "/SKILL.md")
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
                            MARKET_BASE .. "/" .. skill_name .. "/" .. group .. "/" .. fname,
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

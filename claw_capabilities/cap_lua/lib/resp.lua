-- lib/resp — blessed response helper (improvement #12 Inc 4).
--
-- A tiny, trusted building block that scripts pull in with
--     local resp = require("lib/resp")
-- to produce consistent JSON result strings, instead of hand-rolling cjson
-- shapes in every script. Loaded read-only from rolfs:/lua/lib/ by the restricted
-- require searcher in cap_lua. Self-written; not derived from any third-party implementation.

local cjson = require("cjson")

local M = {}

-- ok(payload) -> JSON string {"ok":true, ...payload}
-- payload may be a table (merged) or nil. Always sets ok=true.
function M.ok(payload)
    local t = {}
    if type(payload) == "table" then
        for k, v in pairs(payload) do
            t[k] = v
        end
    elseif payload ~= nil then
        t.result = payload
    end
    t.ok = true
    return cjson.encode(t)
end

-- err(msg) -> JSON string {"ok":false,"error":msg}
function M.err(msg)
    return cjson.encode({ ok = false, error = tostring(msg or "error") })
end

return M

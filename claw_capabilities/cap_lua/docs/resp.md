# lib/resp  —  require("lib/resp")  (blessed Lua library)

- ok(payload) -> JSON string {"ok":true, ...payload}   (payload table merged in)
- err(msg)    -> JSON string {"ok":false,"error":msg}

Loaded read-only from rolfs:/lua/lib/. Use to build consistent result strings:
return resp.ok({pin="PA_25", verified=true})

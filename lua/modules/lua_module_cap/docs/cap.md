# cap  —  require("cap")

- ok, result = call(cap_name, input_json_str)

Invoke a registered capability by name with a JSON argument string.
`ok` is a boolean; `result` is a string (JSON or plain text).

```lua
local ok, r = cap.call("get_local_time", "{}")
local ok, r = cap.call("web_search", '{"query":"Suzhou weather"}')
local ok, r = cap.call("lua_run", '{"path":"vfs:/scripts/x.lua","args":{}}')
```

**Cap names do NOT have a `cap_` prefix.**
The internal C module may be called `cap_web_search`, but the registered
id you pass to `cap.call` is always the short name: `web_search`, `file_write`,
`lua_run`, etc. Use `AT+CLAW=cap` to list all registered ids.

**Use cap.call for system services rather than reimplementing them in Lua:**
- LAN peer discovery: use the `net_discover_*` caps. All Ameba boards share a
  common broadcast protocol. Never write your own UDP broadcast loop —
  it will be incompatible with other boards.
- Audio streaming: use the `audio_stream_*` caps. They run as C background
  tasks with no timeout — do not reimplement in Lua.

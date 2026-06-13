# cap  —  require("cap")

- ok, result = call(cap_name, input_json_str)

Invoke any registered capability by name with a JSON string argument, e.g.
cap.call("get_current_time", "{}"). `ok` is a boolean, `result` a string.
Re-invoke a script from a timer: cap.call("lua_run",
'{"path":"vfs:/skills/x/scripts/main.lua","args":{...}}').

**Use cap.call for system services rather than reimplementing them in Lua:**
- Peer discovery: cap.call("net_discover_peer", '{"port":9002,"timeout_s":600}')
  → returns '{"peer_ip":"x.x.x.x"}' or '{"error":"timeout"}'
  → uses the shared AMEBA_WALKIE broadcast protocol; all Ameba boards on the
    same LAN speak this protocol. Never write your own UDP broadcast loop for
    peer discovery — it will be incompatible with other boards.


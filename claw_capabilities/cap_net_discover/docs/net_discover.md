# net_discover  —  LAN peer discovery

Discovers another Ameba board on the same WiFi subnet via UDP broadcast.
Never implement peer discovery yourself in Lua —
custom UDP loops are incompatible with the built-in protocol.

Two caps are provided. Choose based on whether you need the connection to
survive peer reboots and late arrivals.

---

## net_discover_start  —  persistent background service

Starts a background task that continuously broadcasts and listens.
When a peer appears, `on_found_cap` is called with `peer_ip` injected
into `on_found_args`. When the peer disappears (no broadcast for
`keepalive_s` seconds), `on_lost_cap` is called.

**Returns immediately** — the service runs until `net_discover_stop`.
Idempotent: calling again on the same port returns `already_running=true`.

Use this when:
- The connection must survive if the peer reboots or connects late.
- You want the system to react automatically to peer appearance/disappearance.
- The script should return quickly rather than block.

```json
{
  "port": <udp_port>,
  "keepalive_s": 8,
  "on_found_cap": "<cap to call when peer appears>",
  "on_found_args": { ... base args, peer_ip is injected at call time ... },
  "on_lost_cap":  "<cap to call when peer disappears>"
}
```

**`port` must be different from any port used by `on_found_args`.**
Discovery and audio each bind their own UDP socket; sharing a port causes
packets to be routed to only one socket, breaking both services.
Example: use port 9002 for discovery and port 9000 for audio.

Minimal Lua wrapper (if custom logic is needed before starting):
```lua
function run(args)
    local cap   = require("cap")
    local cjson = require("cjson")
    cap.call("net_discover_start", cjson.encode({
        port        = <udp_port>,
        keepalive_s = 8,
        on_found_cap  = "<on_found_cap>",
        on_found_args = { ... },
        on_lost_cap   = "<on_lost_cap>",
    }))
    -- Return immediately — C layer handles the rest.
    -- Do NOT add a monitoring loop here.
end
```

---

## net_discover_peer  —  one-shot blocking query

Blocks until one peer is found or timeout. Returns `{"peer_ip":"x.x.x.x"}`
on success or `{"error":"timeout"}`.

**Stops broadcasting as soon as it returns.** If the remote device boots
later or reboots, this device is no longer broadcasting and they will never
find each other until the script is restarted.

Use this only for one-time queries (e.g. "what is my peer right now?") when
you know both sides will be online and broadcasting at the same time.

```json
{"port": <udp_port>, "timeout_s": 60}
```

cap.call returns two values — capture both:
```lua
local ok, result = cap.call("net_discover_peer", '{"port":<udp_port>,"timeout_s":60}')
if ok and result then
    local t = cjson.decode(result)
    local peer_ip = t.peer_ip   -- may be nil on timeout
end
```

---

## Auto-run on boot

Register a scheduler job triggered by `wifi_connected` — **not** `delay_sec`.
The event fires as soon as WiFi gets an IP, regardless of how long WiFi takes.

```json
{
  "id": "<job_id>",
  "cap_id": "net_discover_start",
  "cap_args": {
    "port": <udp_port>,
    "keepalive_s": 8,
    "on_found_cap": "<cap>",
    "on_found_args": { ... },
    "on_lost_cap": "<cap>"
  },
  "event_type": "wifi_connected",
  "interval_sec": 0
}
```

Note: `cap_args` accepts a JSON object directly — no need to escape quotes.

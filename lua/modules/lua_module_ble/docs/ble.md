# ble  —  require("ble")

BLE **peripheral** (device is advertised & connected TO). One built-in
transparent channel: service `0xFFF0`, characteristic `0xFFF1`
(read + write + notify). A phone app (nRF Connect / LightBlue) can connect,
write bytes to `fff1` (→ your `data` event), and subscribe to `fff1` notify
(you push bytes back with `ble.notify`). No central/scan; peripheral only.

## Lifecycle
- `ble.init([name])`  → true | nil,err   Enable the stack (idempotent). `name` = advertised device name.
- `ble.deinit()`      → true              Stop the stack.
- `ble.address()`     → "xx:xx:.." | nil,err
- `ble.set_name(name)`→ true | nil,err
- `ble.adv_start()`   → true | nil,err    Start connectable advertising (a connection auto-stops it).
- `ble.adv_stop()`    → true | nil,err
- `ble.disconnect(conn)` → true | nil,err
- `ble.notify(conn, data)` → true | nil,err   Send bytes to a peer over `fff1`. Fails unless that peer has subscribed, and `#data` must be ≤ (negotiated MTU − 3), else error.
- `ble.stats()`       → { enabled, advertising, conn_count, max_conn }

## Event model (this is how you receive data)
Events are queued by the stack; you drain them yourself — nothing arrives until
you poll.
1. `ble.on_event(fn)` — register one handler `fn(ev)`.
2. Loop calling `ble.process_events(timeout_ms)` — dispatches queued events to
   `fn`, returns the count dispatched. Block up to `timeout_ms` for the first.

`ev.type` and its fields:
| type            | fields |
|-----------------|--------|
| `connected`     | `ev.conn` (0..max_conn-1) |
| `disconnected`  | `ev.conn`, `ev.reason` |
| `adv_started`   | — |
| `adv_stopped`   | `ev.reason` |
| `subscribe`     | `ev.conn`, `ev.notify` (bool: peer enabled/disabled fff1 notify) |
| `data`          | `ev.conn`, `ev.payload` (bytes the peer wrote to fff1) |
| `mtu_changed`   | `ev.conn`, `ev.mtu` |

Send to a peer only after you saw its `subscribe` event with `ev.notify==true`.

## Minimal echo peripheral
```lua
local ble = require("ble")
ble.init("my-device")
ble.on_event(function(ev)
  if ev.type == "data" and ev.conn then
    ble.notify(ev.conn, "echo:" .. ev.payload)   -- peer must be subscribed
  elseif ev.type == "disconnected" then
    ble.adv_start()   -- a connection stopped adv; restart to stay discoverable
  end
end)
ble.adv_start()
while true do
  ble.process_events(500)   -- pump the event loop
end
```

## Notes
- Multiple peers connect at once (up to `max_conn`); always act on `ev.conn`.
- **Staying discoverable:** a connection automatically stops advertising, and it
  is NOT resumed when the peer disconnects. A long-running peripheral must call
  `ble.adv_start()` again on the `disconnected` event, otherwise no new phone can
  find it (see the example above).
- `ev.payload` is the raw bytes the peer wrote — nothing is trimmed or framed for
  you. If you treat it as a text command, strip trailing whitespace / newlines
  first (`payload:gsub("%s+$","")`): BLE apps often append a space or `\n`.
- `ble.notify` over-long payloads (> MTU−3) return an error — keep messages short
  or split them yourself; fragmentation is not automatic.

# net_discover_peer cap  —  LAN peer discovery

Discovers another Ameba board on the same WiFi subnet via UDP
broadcast (AMEBA_WALKIE protocol). Never implement peer discovery
yourself in Lua — custom UDP loops are incompatible with this
protocol and with other boards.

## Usage

```json
{"port": 9002, "timeout_s": 60}
```

- `port` (optional): broadcast port, default 9002.
- `timeout_s` (optional): how long to wait; default 600 s, max 3600 s.
  When called as a direct LLM tool call, capped at 60 s.
  Inside `lua_run_async` via `cap.call()`, up to 3600 s is allowed.

## Return value

Success: `{"peer_ip": "192.168.x.x", "msg": "peer discovered via UDP broadcast"}`

Timeout: `{"error": "timeout", "msg": "no peer found after N broadcasts"}`

## Typical walkie-talkie flow

Both boards must run `net_discover_peer` **at the same time** for
discovery to succeed — one board broadcasts and the other receives.

```
Board A:  net_discover_peer {"port":9002,"timeout_s":60}
Board B:  net_discover_peer {"port":9002,"timeout_s":60}
          → both get each other's IP
Board A:  audio_stream_rx_start {"port":9000}
Board A:  audio_stream_tx_start {"peer_ip":"<B_ip>","port":9000,"gpio_pin":"PA_15"}
Board B:  audio_stream_rx_start {"port":9000}
Board B:  audio_stream_tx_start {"peer_ip":"<A_ip>","port":9000,"gpio_pin":"PA_15"}
```

If `net_discover_peer` returns `{"error":"timeout"}`, **do not retry
automatically** — inform the user that the peer board must also run
the discovery command at the same time.

## Auto-run on boot: use event_type, NOT delay_sec

To run a walkie-talkie script automatically after each reboot, register
a scheduler job triggered by the `wifi_connected` system event — **not**
a fixed `delay_sec`. The event fires as soon as WiFi gets an IP address,
so the script starts at the right moment regardless of how long WiFi takes.

```json
{
  "id": "walkie_talkie",
  "cap_id": "lua_run_async",
  "cap_args": {"path": "vfs:/scripts/walkie_talkie.lua", "timeout_ms": 3600000},
  "event_type": "wifi_connected"
}
```

**Never use `delay_sec` to wait for WiFi** — a fixed delay is always wrong:
too short on a slow AP, wasteful on a fast one, and broken after reconnects.

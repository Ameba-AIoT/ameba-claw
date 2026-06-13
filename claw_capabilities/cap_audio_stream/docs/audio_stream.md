# audio_stream caps  —  C-layer audio streaming

These caps run as **C background tasks**. Never reimplement audio
streaming in Lua — the `audio` Lua module exists only for simple
fixed-duration recordings (e.g. record a WAV file), not for
real-time peer-to-peer streaming.

## Correct call order

```
1. audio_stream_rx_start  {"port": 9000}
2. audio_stream_tx_start  {"peer_ip": "<from net_discover_peer>",
                           "port": 9000, "gpio_pin": "PA_15"}
```

RX **must** start before TX (SPORT0 full-duplex constraint).

## audio_stream_rx_start

Listen for raw PCM audio from a peer on a UDP port and play it on
the speaker.

```json
{"port": 9000}
```

- `port` (required): local UDP port to receive on (e.g. 9000).
- `sample_rate` (optional): Hz, default 16000.

## audio_stream_tx_start

Capture DMIC audio and stream it to a peer over UDP while a PTT
button is held. The C task polls the GPIO pin automatically — do NOT
read the pin in Lua.

```json
{"peer_ip": "192.168.0.x", "port": 9000, "gpio_pin": "PA_15"}
```

- `peer_ip` (required): destination IP (use `net_discover_peer`).
- `port` (required): destination UDP port (must match peer's RX port).
- `gpio_pin` (required): PTT button pin, e.g. `"PA_15"` (active-low).
- `gpio_active_low` (optional): default `true`.
- `sample_rate` (optional): Hz, default 16000.

## audio_stream_stop

Stop both TX and RX tasks.

```json
{}
```

## audio_stream_status

Returns `{"tx":"running"|"idle", "rx":"running"|"idle", ...}`.

```json
{}
```

## Auto-run on boot

Register the walkie-talkie script with `event_type: "wifi_connected"` —
the scheduler fires it the instant WiFi gets an IP, with no artificial delay.
See `rolfs:/docs/net_discover.md` for the full scheduler_add_job example.

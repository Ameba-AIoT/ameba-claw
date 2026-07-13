# audio_stream caps  —  C-layer audio streaming

These caps run as **C background tasks**. Never reimplement audio
streaming in Lua — the `audio` Lua module exists only for simple
fixed-duration recordings, not for real-time UDP audio streaming.

## audio_stream_start  —  combined RX + TX

Starts both RX (UDP→speaker) and TX (DMIC→GPIO-gated UDP) in a single call.
Returns immediately; both tasks run in the background.

Compatible with `net_discover_start` as an `on_found_cap`: `peer_ip` is
automatically injected into the args by the discovery service.

```json
{"peer_ip": "x.x.x.x", "port": <udp_port>, "gpio_pin": "<pin>"}
```

- `peer_ip` (required): destination IP for TX; also the expected sender for RX.
- `port` (required): UDP port — TX sends to this port on peer; RX listens on this port locally.
- `gpio_pin` (required): GPIO pin name that gates TX (active-low by default), e.g. `"PA_0"`, `"PB_3"`.
- `gpio_active_low` (optional): default `true`.
- `sample_rate` (optional): Hz, default 16000.

Internally starts RX first, then TX — satisfying the SPORT0 full-duplex
constraint without the caller needing to manage the order.

If called when RX is already running (e.g. after `audio_stream_pause`),
it skips RX init and only (re)starts TX.

---

## audio_stream_rx_start

Listen for raw PCM audio from a peer on a UDP port and play it on
the speaker.

```json
{"port": <udp_port>}
```

- `port` (required): local UDP port to receive on.
- `sample_rate` (optional): Hz, default 16000.

## audio_stream_tx_start

Capture DMIC audio and stream it to a peer over UDP while a GPIO pin
is held. The C task polls the GPIO pin automatically — do NOT read the
pin in Lua.

```json
{"peer_ip": "x.x.x.x", "port": <udp_port>, "gpio_pin": "<pin>"}
```

- `peer_ip` (required): destination IP address.
- `port` (required): destination UDP port.
- `gpio_pin` (required): GPIO pin name that gates TX, active-low by default (e.g. `"PA_0"`, `"PB_3"`).
- `gpio_active_low` (optional): default `true`.
- `sample_rate` (optional): Hz, default 16000.

Note: RX must be started before TX (SPORT0 full-duplex constraint).
Use `audio_stream_start` to start both in the correct order automatically.

## audio_stream_pause

Stop TX (DMIC→UDP) only. RX (UDP→speaker) keeps running, continuously writing
silence frames to the DMA. This keeps the I2S clock running so the amplifier
stays active — no pop when `audio_stream_start` resumes.
`audio_stream_start` handles "RX already running" gracefully and only restarts TX.

```json
{}
```

## audio_stream_stop

Stop both TX and RX tasks completely. Speaker hardware is powered down.

```json
{}
```

## audio_stream_status

Returns current TX/RX state and packet counters.

```json
{}
```

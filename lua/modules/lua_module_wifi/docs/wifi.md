# wifi  —  require("wifi")

Wi-Fi STA helper for RTL8721F. Connects to an access point and runs DHCP;
provides a connectivity check. Only STA mode is supported. The module wraps
`wifi_connect` + `lwip_dhcp` — both calls are **blocking** (may take several
seconds); do not call from a timer callback or a short-lived task stack.

## API

```lua
-- Connect to an access point (blocking, ~2–10 s)
local ok, err = wifi.connect(ssid, password)
    -- ssid    : string, max 32 bytes
    -- password: string (WPA/WPA2-PSK); pass "" for open networks
    -- Returns: true               on success (associated + IP assigned)
    --          false, err_string  on failure (association or DHCP error)

-- Check current connectivity
local connected = wifi.status()
    -- Returns: true  if STA is associated and has a valid IP
    --          false otherwise
```

### Return-value summary

| Function    | Success         | Failure                     |
|-------------|-----------------|-----------------------------|
| `connect`   | `true`          | `false, "<reason string>"`  |
| `status`    | `true`          | `false`                     |

## Notes

- `connect` is **not idempotent**: calling it while already connected issues a
  new association + DHCP cycle. Check `status()` first if you want to avoid
  re-connecting unnecessarily.
- On DHCP failure the STA may still be associated; `status()` will return
  `false` because there is no valid IP yet.
- Reconnection after disconnection must be triggered manually — there is no
  built-in auto-reconnect in this module.

## Examples

Connect once and check result:
```lua
local wifi = require("wifi")

local ok, err = wifi.connect("MySSID", "MyPassword")
if not ok then
    print("Connect failed:", err)
    return
end
print("Connected")
```

Guard against redundant reconnects:
```lua
local wifi = require("wifi")

if not wifi.status() then
    local ok, err = wifi.connect("MySSID", "MyPassword")
    if not ok then error(err) end
end
print("Wi-Fi ready")
```

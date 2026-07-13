---
name: camera_capture
description: "Capture a JPEG image from a USB UVC camera and save it to the VFS filesystem. Requires a UVC camera connected via USB host."
compatibility: RTL8721F
metadata:
  cap_groups: usb_uvc
  manage_mode: readonly
---
# camera_capture

Capture a JPEG image from a USB UVC camera and save it to the VFS filesystem.

USB cameras are hot-pluggable and are **not tracked by the board hardware system** —
`board_list_devices()` and `board_query_peripheral()` will never show a UVC camera
regardless of whether one is physically connected.

**Do not use `board_hardware_info` to check for a USB camera.** Just activate this
skill and call `lua_run` directly. If no camera is connected, the script returns an
error (`{"error":"camera not ready: timeout"}`); show that message to the user.

## Workflow
1. Activate `camera_capture`
2. Call `lua_run` with the args below

## How to invoke

Default capture:
```json
{"path":"rolfs:/skills/camera_capture/scripts/main.lua","args":{}}
```

Custom filename and resolution:
```json
{"path":"rolfs:/skills/camera_capture/scripts/main.lua","args":{"filename":"vfs:photo.jpg","width":1280,"height":720,"fps":15,"timeout_ms":10000}}
```

## Parameters (JSON)
- `filename`: output path (default `"vfs:capture.jpg"`)
- `timeout_ms`: wait for camera ready, ms (default 10000)
- `width` / `height`: capture resolution (default 640×480)
- `fps`: frame rate (default 15)

## Return value
- Success: `{"ok":true,"filename":"vfs:capture.jpg","bytes":12345,"width":640,"height":480}`
- Error:   `{"error":"<reason>"}`

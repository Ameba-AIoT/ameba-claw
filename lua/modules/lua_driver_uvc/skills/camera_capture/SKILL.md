---
name: camera_capture
description: "Capture a JPEG image from a USB UVC camera and save it to the VFS filesystem. Requires a UVC camera connected via USB host."
compatibility: RTL8721F
metadata:
  cap_groups: audio_stream
  manage_mode: readonly
  prerequisites: board_hardware_info
  peripherals: usb_uvc_camera
---
# camera_capture

Capture a JPEG image from a USB UVC camera and save it to the VFS filesystem.

**Activate `board_hardware_info` first** to confirm the board has a UVC camera
(`usb_uvc_camera` in the peripheral list) before calling this skill.

Activating this skill makes the `audio_stream` cap group visible, so media
streaming tools (live preview, audio capture) become available alongside still
capture in the same session.

## Workflow
1. Activate `board_hardware_info` → confirm `usb_uvc_camera` is present
2. Activate `camera_capture`
3. Call `lua_run` with the args below

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

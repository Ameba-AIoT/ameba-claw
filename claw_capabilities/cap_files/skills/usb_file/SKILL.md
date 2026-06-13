---
name: usb_file
description: "Read, write, list, and delete files on a FAT32 USB flash drive (U-disk) connected via USB host."
compatibility: RTL8721F
metadata:
  cap_groups: lua files
  manage_mode: readonly
  category: storage
  peripherals: usb_msc
---
# usb_file

Read, write, list, and delete files on a FAT32 USB flash drive (U-disk) connected via USB host.

## Parameters (JSON)

- action: "list" | "write" | "read" | "delete"  (required)
- path:   full file path on U-disk, e.g. "0:/note.txt"  (required for write/read/delete)
- data:   text content to write (required for action="write")
- dir:    directory path to list (optional, default: U-disk root "0:/", used for action="list")

## Return value

On success:
- list:   `{"ok":true, "dir":"0:/", "count":3, "entries":[{"name":"note.txt","size":42,"is_dir":false}, ...]}`
- write:  `{"ok":true, "path":"0:/note.txt", "bytes":42}`
- read:   `{"ok":true, "path":"0:/note.txt", "bytes":42, "data":"file content here"}`
- delete: `{"ok":true, "path":"0:/note.txt"}`

On error: `{"error":"description"}`

## How to invoke

Run `{CUR_SKILL_DIR}/scripts/main.lua` via `lua_run` with the args object:

List root directory:
```json
{"path":"{CUR_SKILL_DIR}/scripts/main.lua","args":{"action":"list"}}
```

Write a file:
```json
{"path":"{CUR_SKILL_DIR}/scripts/main.lua","args":{"action":"write","path":"0:/hello.txt","data":"Hello from Ameba!"}}
```

Read a file:
```json
{"path":"{CUR_SKILL_DIR}/scripts/main.lua","args":{"action":"read","path":"0:/hello.txt"}}
```

Delete a file:
```json
{"path":"{CUR_SKILL_DIR}/scripts/main.lua","args":{"action":"delete","path":"0:/hello.txt"}}
```

## Notes

- The U-disk must be inserted and formatted as FAT32 before calling this skill.
- File paths must start with the drive prefix, e.g. "0:/filename.txt".
- Write creates or overwrites the file.
- The first call may take ~5s for USB enumeration; subsequent calls are fast.

# storage — Writable Storage Module

Provides unified access to writable storage on Ameba RTOS.  
The active root switches automatically between SD card and internal flash depending on whether an SD card is inserted.

## Root Selection

| Condition | Root prefix | Backend |
|-----------|------------|---------|
| SD card mounted | `"sdcard:"` | FatFS on SDIOH (PA6–PA11, 4-bit HS) |
| No SD card | `"vfs:"` | LittleFS on internal flash (VFS1 partition) |

Always call `storage.get_root_dir()` instead of hardcoding a prefix.

## API

### `storage.get_root_dir() → string`

Returns the current active root prefix: `"sdcard:"` if SD is mounted, `"vfs:"` otherwise.

```lua
local root = storage.get_root_dir()
-- "sdcard:" or "vfs:"
```

### `storage.join_path(part, ...) → string`

Joins path parts with `/` as separator. Handles trailing `:` (VFS prefix) correctly.

```lua
storage.join_path("sdcard:", "data", "log.txt")  -- "sdcard:data/log.txt"
storage.join_path("vfs:", "cfg")                  -- "vfs:cfg"
```

### `storage.exists(path) → boolean`

Returns `true` if the path exists (file or directory).

```lua
if storage.exists("vfs:config.json") then ... end
```

### `storage.stat(path) → table | nil, err`

Returns `{type, size}` for the given path, or `nil, err` on failure.

- `type`: `"file"` or `"dir"`
- `size`: file size in bytes (0 for directories)

```lua
local info, err = storage.stat("vfs:data.bin")
if info then print(info.type, info.size) end
```

### `storage.mkdir(path) → true | error`

Creates a directory. Raises an error if it fails (already exists is an error on FatFS).

```lua
storage.mkdir("sdcard:logs")
```

### `storage.write_file(path, data) → true | error`

Writes binary data to a file (overwrites if exists).

```lua
storage.write_file("vfs:hello.txt", "Hello, World!")
```

### `storage.read_file(path) → string | error`

Reads the entire file content as a string.

```lua
local content = storage.read_file("vfs:hello.txt")
```

### `storage.listdir(path) → array | error`

Returns an array of `{name, type, size}` tables for all entries in the directory.

```lua
local entries = storage.listdir("sdcard:")
for _, e in ipairs(entries) do
    print(e.name, e.type, e.size)
end
```

### `storage.remove(path) → true | error`

Removes a file.

```lua
storage.remove("vfs:old.txt")
```

### `storage.rename(old, new) → true | error`

Renames/moves a file.

```lua
storage.rename("vfs:tmp.txt", "vfs:final.txt")
```

### `storage.get_free_space() → {total, free, used} | nil, err`

Returns capacity in **KB** (kilobytes). Only available when SD card is mounted (FatFS).  
Returns `nil, "SD card not mounted"` when on internal flash.

```lua
local sp, err = storage.get_free_space()
if sp then
    -- sp.total, sp.free, sp.used are all in KB
    print("total=" .. sp.total .. " KB, free=" .. sp.free .. " KB")
end
```

## Typical Usage Pattern

```lua
local root = storage.get_root_dir()
local path = storage.join_path(root, "myapp", "data.json")

if not storage.exists(storage.join_path(root, "myapp")) then
    storage.mkdir(storage.join_path(root, "myapp"))
end

storage.write_file(path, '{"key":"value"}')
local data = storage.read_file(path)
```

## Notes

- SD card hotplug is handled automatically; the root switches on insert/remove.
- `get_free_space()` is FatFS-only; for LittleFS (vfs:) it returns nil.
- Path prefixes are mandatory: `"vfs:file.txt"` not `"file.txt"`.

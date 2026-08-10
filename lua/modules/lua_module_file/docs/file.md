# file  —  require("file")

- write(path, data) -> true | nil, err   — **TRUNCATES**: replaces the whole file
- read(path) -> string | nil
- exists(path) -> bool
- remove(path)
- list(path) -> entries

Paths are auto-prefixed with "vfs:". Use a VFS file to persist state between
lua_run calls (each call gets a brand-new Lua state). rolfs:/ is read-only.

**No append mode, no `io.*`.** `write` always overwrites the file from scratch,
and Lua's `io` library is not available. To APPEND (e.g. a log line), read the
old content, concatenate, and write it back:

```lua
local file = require("file")
local old = file.read(path) or ""      -- nil when the file doesn't exist yet
file.write(path, old .. line .. "\n")  -- write returns true, or nil+err on failure
```

**Path persistence:**
- `vfs:/scripts/`, `vfs:/skills/`, `vfs:/scheduler/` — survive reboot ✓
- `vfs:/tmp/` — **wiped on every reboot**. Use only for throwaway temp files.
  Do NOT store user config or app state here.

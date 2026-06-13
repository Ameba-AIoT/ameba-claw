# file  —  require("file")

- write(path, data)
- read(path) -> string|nil
- exists(path) -> bool
- remove(path)
- list(path) -> entries

Paths are auto-prefixed with "vfs:". Use a VFS file to persist state between
lua_run calls (each call gets a brand-new Lua state). rolfs:/ is read-only.

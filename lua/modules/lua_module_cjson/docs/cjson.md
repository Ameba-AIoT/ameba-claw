# cjson — require("cjson")

Minimal JSON codec (backed by cJSON). Two functions:

```lua
local cjson = require("cjson")

cjson.decode(str)   -- JSON text -> Lua value
cjson.encode(value) -- Lua value -> compact JSON string (no whitespace)
```

## decode(str)

Parses `str` and returns the Lua value: object -> table, array -> table,
string -> string, number -> integer or float, `true`/`false` -> boolean,
`null` -> `nil`.

On a parse error it returns **two values**: `nil, "cjson.decode: parse error"`.
Always check the first return before using it:

```lua
local t, err = cjson.decode(str)
if not t then return cjson.encode({ok=false, error=err}) end
```

## encode(value)

Returns a compact (unformatted) JSON string. Supports nil, boolean, number,
string, and table. Unsupported types (function/userdata) become the string
`"(unsupported)"`. Nesting deeper than 16 levels is cut off as
`"...(too deep)"`, so cyclic tables do not crash but truncate.

### Array vs object — the main gotcha

A Lua table is encoded as a JSON **array** only when its keys are exactly the
contiguous integers `1..N` (no gaps, all positive). Any other shape encodes as
a JSON **object**:

```lua
cjson.encode({10, 20, 30})            --> [10,20,30]      (1..3 contiguous)
cjson.encode({a=1, b=2})              --> {"a":1,"b":2}
cjson.encode({[1]="a", [100]="b"})    --> {"1":"a","100":"b"}  (sparse -> object)
cjson.encode({})                      --> []   <-- empty table is an ARRAY, not {}
```

- **Empty table `{}` encodes as `[]`, not `{}`.** There is no way to emit an
  empty JSON object from a bare `{}`; put at least one string key in it if you
  need `{...}`.
- `nil` cannot be stored as a Lua table value, so a JSON `null` field is lost on
  decode (`{"a":null}` -> `{}`), and you cannot round-trip an explicit `null`.

### Numbers

Integral numbers stay integers; non-integral ones are floats. Note Lua division
`/` always yields a float (`10/2` -> `5.0`), which affects both encoding and a
later `string.format("%d", ...)` — use `//` or `math.floor()` for integers.

## Returning results from a skill

`run(args)` receives an **already-decoded** table — do NOT `cjson.decode(args)`.
`run()` should return a JSON **string**. The recommended result envelope is a
table with an `ok` flag, encoded on the way out:

```lua
function run(args)
    -- ... do work ...
    return cjson.encode({ok=true, pin="PA_25", value=1})   -- success
end
-- on failure:
return cjson.encode({ok=false, error="pin not found"})
```

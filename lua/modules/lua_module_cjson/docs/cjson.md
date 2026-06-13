# cjson  —  require("cjson")

- decode(str) -> table
- encode(value) -> string

run(args) already receives a decoded table — do NOT cjson.decode(args).
Return a JSON string from run(): return cjson.encode({ok=true, ...}).

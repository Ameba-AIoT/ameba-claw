---
name: skill_authoring
description: "Guide for writing Lua skills or application scripts. Activate when the user wants to create, run, or automate something new on the device."
compatibility: RTL8721F
metadata:
  manage_mode: readonly
  category: authoring
---
# skill_authoring

## Skill or application script?

| | Skill | Application script |
|---|---|---|
| Who uses it | LLM invokes it to complete a task | User runs it directly |
| Lifetime | Short, returns a result to LLM | Long-running daemon / loop |
| Needs SKILL.md | Yes | No |

**→ Application script** (long-running, user-facing): use `write_file` + `lua_run` / `lua_run_async`.
- `vfs:/tmp/<name>.lua` — throwaway, wiped on reboot
- `vfs:/scripts/<name>.lua` — persistent across reboots ✓
- Auto-run on boot: `scheduler_add_job` with `cap_id=lua_run_async`, `delay_sec=20`, `interval_sec=86400`

Do NOT put application scripts under `vfs:/skills/` — that path is reserved
for the skill catalog. Use `vfs:/scripts/` for user apps that survive reboots.

**→ Skill** (LLM-invokable capability): follow the workflow below.

## Skill workflow

1. skill_list — check if a skill already covers it
2. skill_activate(name) — load closest skill; its doc shows script path
3. lua_run(path, args) — run the script
4. If nothing matches: skill_save(name, code, doc) → skill_activate → lua_run

## Running scripts

Scripts run **by path**. `args` is a decoded Lua table — do NOT call `cjson.decode(args)`.
Allowed roots: `rolfs:/skills/`, `rolfs:/docs/`, `rolfs:/lib/`, `vfs:/skills/`, `vfs:/scripts/`, `vfs:/tmp/`.

```lua
function run(args)
    -- args is already a table
    local pin = args.pin or "PA_11"
    return require("cjson").encode({ok=true, pin=pin})
end
```

## Lua modules

Activate **builtin_lua_modules** for the full index, then `read_file("rolfs:/docs/<module>.md")`.
For board pins/peripherals, activate **board_hardware_info** first.

## ⚠️ CRITICAL rules

**Lua state isolation:** Every `lua_run` creates a fresh state — globals reset, hardware handles gone.
Store persistent state in VFS: `file.write("vfs:/tmp/state.json", cjson.encode(state))`.

**No infinite loops:** Skills time out after 30 s. For repeating operations use `timer.start(ms, code, true)`.
For indefinitely-running apps use `lua_run_async` or `scheduler_add_job`.

**Timer callbacks:**
1. `require()` is NOT available — all modules are pre-loaded globals (`cap`, `gpio`, `i2c`, `rtc`, `cjson`, `sys`).
2. Never call `sys.sleep_ms()` inside a callback — blocks all timers.
3. Avoid `string.format([[...]])` when body contains Lua patterns (`%d`, `%s`) — use header-concat instead.
4. Functions defined in `run()` are NOT visible in callbacks (different Lua state).

Re-invoke a script from a timer:
```lua
timer.start(1000, [[
    cap.call("lua_run", '{"path":"vfs:/scripts/myscript.lua","args":{"phase":"tick"}}')
]], true)
```

**Use cap.call for system services** instead of reimplementing in Lua:
- Peer discovery: `cap.call("net_discover_peer", '{"port":9002,"timeout_s":30}')` → `{"peer_ip":"x.x.x.x"}`
  Uses the shared `AMEBA_WALKIE` broadcast protocol — all Ameba boards speak it. Never write your own UDP discovery loop.

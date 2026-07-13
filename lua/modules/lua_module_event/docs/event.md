# event  —  require("event")

## Hardware event queue (blocking wait)

```lua
event.wait([timeout_ms])  ->  true | nil
```

Block until a hardware event is ready (GPIO callback registered via `gpio.on`),
dispatch it (calls the registered Lua callback), and return `true`.
Returns `nil` on timeout.  Pass 0 or omit for a non-blocking check.

Waits in 50 ms chunks and checks the cooperative cancel hook each chunk, so the
script responds to timeouts and `lua_run` cancellation correctly.

**Available in skill scripts AND the REPL.**

Typical pattern with `gpio.on`:
```lua
function run(args)
    gpio.on("PA_22", "both", function(ev)
        print(ev.pin, ev.edge, sys.millis())
    end)
    gpio.irq_enable("PA_22")
    while true do
        if not event.wait(30000) then print("idle") end
    end
end
```

## Outbound event publishing

```lua
event.publish_message(source_cap, channel, chat_id, text [, sender_id]) -> bool
event.publish_trigger(source_cap, event_type, event_key [, payload_json]) -> bool
```

Push an event into the claw_event_dispatcher for routing to IM channels or
triggering agent workflows. Usually only needed in advanced multi-cap scripts.

## Proactively notifying the user (outbound push)

```lua
event.notify(text)                 -> bool   -- push to WHOEVER launched this script (recommended)
event.send(channel, chat_id, text) -> bool   -- push to a specific channel/chat
event.origin()                     -> channel, chat_id   -- this script's launcher ids
```

Send a text message **straight to the user**, **without** starting an
agent/LLM turn. This is the "tell the user something happened" primitive for
**detached background jobs** — e.g. a `lua_run_async` monitor loop that watches
something and must report a change on its own schedule.

**Prefer `event.notify(text)`.** The harness captured the launching user's
channel + chat_id when the script started, so `notify` routes back to that same
user with **no ids to pass** — the script (and the LLM that wrote it) cannot
target the wrong person. Returns `false` (sends nothing) when there is no
reachable origin (launched from a trigger/scheduler with no channel, or that
channel has no outbound handler) — check it.

```lua
-- inside a background monitor loop, after detecting a change:
if changed then
    event.notify("画面变化：新增 cup；pen 移动了")   -- goes to whoever started this job
end
```

Use `event.send(channel, chat_id, text)` only to target a **different** channel
than the launcher (returns `false` if no send handler is registered for
`channel`). `event.origin()` returns the launcher's raw `channel, chat_id`
(both `""` if the job had no channel) for advanced routing.

Contrast with `publish_message()`, which injects an **inbound** message that
re-triggers the LLM; `notify`/`send` are **one-way outbound** pushes.

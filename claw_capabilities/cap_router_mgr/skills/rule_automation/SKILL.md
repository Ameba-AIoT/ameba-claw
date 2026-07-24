---
name: rule_automation
description: "Set up declarative fast-path / auto-reaction rules so recurring messages or events (fixed commands, threshold alerts, canned auto-replies, muting spam) are handled instantly and deterministically WITHOUT waking the full LLM agent every time. Activate this when the user wants certain repetitive inputs or events to be answered automatically, quickly and consistently, or wants to stop reasoning from scratch on the same request over and over. Unmatched inputs still fall through to you (the agent) as normal."
compatibility: RTL8721F
metadata:
  cap_groups: router_mgr
  manage_mode: readonly
  category: automation
---
# rule_automation

Event-routing rules are a **fast-path + guardrail** table: encode the "high-frequency /
low-latency / deterministic reactions that shouldn't wake the full LLM every time" as a single
rule that handles the input directly. **Deliberately leave gaps for what rules can't express** —
any input that matches no rule automatically falls through to you (the agent). Don't cram complex
logic into a rule just to force a match.

Once this skill is active, these rule-management tools become callable (hidden until activated):
`router_list_rules` `router_get_rule` `router_add_rule` `router_update_rule`
`router_remove_rule` `router_reload_rules`. Submit a rule as a JSON object via `router_add_rule`;
it takes effect immediately and is persisted. The tools' `input_schema` inlines the field table,
the `kind` enum, and a copyable example.

## Rule structure

```json
{
  "id": "unique_id",            // required; also the key for update/remove
  "enabled": true,               // default true
  "consume_on_match": false,     // true = stop evaluating later rules once this one matches
  "ack": "Got it: @{ev.text}",   // optional; sent to the source channel the instant it matches (before actions run)
  "cooldown_ms": 5000,           // optional; min interval between two fires (debounce / anti-spam)
  "vars": { "city": "Beijing" }, // optional; rule-level constants, referenced as @{vars.city}
  "match": { ... },              // required; match conditions (all ANDed; omit a field = wildcard)
  "actions": [ ... ]             // required; run in order once matched
}
```

## match — conditions (ANDed; omitted = wildcard)

| field | meaning |
|---|---|
| `event_type` | exact match on event type (e.g. `"message"`, `"sensor"`) |
| `source_cap` / `channel` / `chat_id` | exact match on source cap / source channel / chat id |
| `event_key` | exact match on a trigger event's correlation key |
| `text_contains` | **substring** match on the text |
| `text` + `text_match_rule` | `"exact"` (whole string equal) or `"prefix"` (prefix) match |

**PREFIX trick**: `text_match_rule:"prefix"` + `text:"/weather"` matching "/weather Beijing" puts
the part after the prefix (leading spaces trimmed) into `@{match.remainder}` (= "Beijing") — this
is how you do "command word + argument".

`text_contains` is a **loose substring** test — `"hi"` also matches "this", "chip", etc. For
command-like triggers prefer `"exact"` or `"prefix"` so unrelated messages don't fire the rule.

Don't write a rule whose match is entirely empty AND that has no effective action (it would
swallow every message and disable agent fallback — it will be rejected).

## actions — run in order

Each action is `{ "kind": "...", ... }`. **Use the `rtk_` prefix on `kind`.** Valid values:

| kind | purpose | how to configure |
|---|---|---|
| `rtk_agent` | hand off to the LLM agent | no input needed |
| `rtk_cap` | call a capability | `cap` = capability id, `input` = **JSON object** of arguments |
| `rtk_send` | send one IM message | `cap` = target channel (omit = reply to source), `input` = **plain-text string** |
| `rtk_emit` | derive a new event and re-enqueue it so **another rule** picks it up (reuse / normalize / forward) | `input` = **JSON object** (see below) |
| `rtk_drop` | silently drop the event | none |

**A rule produces NO user-visible reply unless it has an `rtk_send` or `rtk_agent`.** `rtk_cap`
only fetches/acts; its result goes into `@{last.output}`, not to the user. A rule that only calls
`rtk_cap` (even with `capture_output`) sends nothing back except the `ack`. If you fetched data,
you MUST add an `rtk_send` to actually deliver it.

**`cap` / `input` are overloaded — read them by kind**: for `rtk_cap`, `cap` = capability id and
`input` = JSON object; for `rtk_send`, `cap` = target channel and `input` = plain text.

**Use the exact `cap` id** as it appears in the capability/tool list — don't guess or add
prefixes (e.g. the search cap is `web_search`, not `cap_web_search`). A wrong id makes the
`rtk_cap` action fail silently.

**`rtk_emit` reuses an existing rule (don't copy its logic)**: the derived event is re-matched. In
`input`, `text` / `event_type` / `channel` / `chat_id` **rewrite the derived event's routing** so it
hits **another rule** (unset keys are inherited from the current event; a `payload` key or the whole
object becomes the payload). So when a new rule wants to "also do what another rule already does",
have it `rtk_emit` an event that the other rule will match (e.g.
`"input":{"text":"<that rule's trigger> <arg>"}`) instead of duplicating that rule's actions.
Chains are depth-bounded (default 3 hops; over the limit it logs a warning and drops), but still
avoid making the derived event match itself.

**Per-action optional fields**:
- `on_error`: `"continue"` (default) or `"stop"` (on failure, stop the rule's remaining actions).
- `capture_output`: `true` stores this action's output into `@{last.output}` for the **next** action.
- `only_if`: single guard `{"left":"@{...}","op":"gt","right":30}`, `op` ∈
  `eq/ne/gt/lt/ge/le/contains/exists`; numeric compare when both sides are numbers, else string; if
  it fails the action is skipped. Single comparison only — anything more complex goes to the agent.

## Template variables (use `@{dotted.path}` in strings, not `{{}}`; a miss renders empty)

`@{event.text}` `@{event.chat_id}` `@{event.sender_id}` `@{event.source_channel}`
`@{event.event_type}` `@{event.source_cap}` `@{event.payload.xxx}` (payload JSON fields);
short aliases `@{ev.text}` `@{ev.chat}` `@{ev.sender}` `@{ev.channel}` `@{ev.type}` `@{ev.cap}`;
`@{vars.xxx}`; `@{match.remainder}` `@{match.text}` `@{match.rule}`; `@{last.output}` (previous
action's **full** output), `@{last.output.<field>}` (pick one field when the previous action
returned JSON, e.g. `@{last.output.answer}`); `@{rule.id}`.

**Action chaining**: the earlier action must set `capture_output:true` before a later action can
use `@{last.output}`.
When calling a cap that returns JSON (e.g. `web_search` returns `{answer,results}`), prefer
`@{last.output.answer}` in `rtk_send` to send just the conclusion — don't dump the whole JSON with
`@{last.output}`.
`ack` is sent **before** actions run, so `@{last.output}` must not be used in `ack`.

**`@{last.output}` is a single slot and gets overwritten (important)**: it only ever holds the
**most recent** `capture_output` action's output. Another `capture_output` overwrites the previous
one, which can no longer be referenced — **there is no `@{action_N.output}` syntax**. So **a single
`rtk_send` cannot combine the outputs of two different tools.** To combine results from several
tools:
- **Send them separately**: put an `rtk_send` right after **each** `rtk_cap` (each send reads only
  its immediately-preceding capture) — this sends multiple messages;
- **Merge into one message**: the current harness can't express this. **Don't sacrifice picking the
  right tool** to force a single capture — don't switch to an unsuitable cap just to gather several
  facts at once (e.g. asking a search cap "what time is it" — it can't give the device's real clock).
  Leave such merge cases unhandled and let the message fall through to the agent.

## Examples

Command routing + call cap + send the result back (action chain):
```json
{ "id": "weather_cmd",
  "match": { "text": "/weather", "text_match_rule": "prefix" },
  "actions": [
    { "kind": "rtk_cap", "cap": "web_search",
      "input": { "query": "weather @{match.remainder}" }, "capture_output": true },
    { "kind": "rtk_send", "input": "Weather for @{match.remainder}: @{last.output.answer}" } ] }
```

Sensor threshold alert (guard + cooldown):
```json
{ "id": "temp_alert", "match": { "event_type": "sensor" }, "cooldown_ms": 60000,
  "actions": [ { "kind": "rtk_send", "cap": "telegram",
    "input": "Temperature too high: @{event.payload.temp}℃",
    "only_if": { "left": "@{event.payload.temp}", "op": "gt", "right": 30 } } ] }
```

## Common mistakes

- `"kind":"send"`/`"cap"` → treated as `rtk_agent`; the `rtk_` prefix is required.
- `{{ev.text}}` → not substituted; use `@{ev.text}`.
- A later action uses `@{last.output}` but the earlier one lacked `capture_output:true` → empty string.
- After two `capture_output` actions, trying to use both outputs in the final send → you only get
  the **last** one, the rest were overwritten (see multi-tool handling under "Action chaining":
  one send after each cap, or let it fall through to the agent).
- Using `@{last.output}` in `ack` → empty (ack is sent before actions).
- Writing `rtk_send`'s `input` as an object → it goes out as JSON text; it must be a plain-text string.
- Only `rtk_cap` actions with no `rtk_send`/`rtk_agent` → nothing reaches the user (see the reply warning above).

## After creating a rule — self-check (don't assume success)

After `router_add_rule`:
1. Call `router_get_rule` / `router_list_rules` to confirm the rule persisted with the actions you
   intended — in particular that a data-fetching rule ends with an `rtk_send`.
2. Remember rule actions run on a single dispatcher thread and `rtk_cap` is **synchronous** — a slow
   cap (e.g. a network call) delays the reply and briefly stalls other events. Keep rule actions
   fast; put heavy or uncertain work on the agent.
3. If you can, trigger it once and verify the **delivered output** is actually correct (right
   content, from the right source — not stale or guessed data), not merely that the rule exists.

## Troubleshooting "why didn't my rule fire?"

Use `router_get_rule` to read the rule's live `fire_count` / `last_fired_ts`: a `fire_count` stuck
at 0 means the match never hit (check field spelling / case / `text_match_rule`); an overly large
`cooldown_ms` also suppresses repeated fires.

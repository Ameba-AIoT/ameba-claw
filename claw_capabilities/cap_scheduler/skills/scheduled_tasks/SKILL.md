---
name: scheduled_tasks
description: "How to schedule things on the device — reminders, alarms, recurring jobs, and run-on-boot scripts. Activate when the user wants something to happen at a time, on a repeat, or every time the device starts."
compatibility: RTL8721F
metadata:
  manage_mode: readonly
  category: automation
---
# scheduled_tasks

All scheduling is done with the `scheduler_*` tools. A task = **one TRIGGER (when)** + **one ACTION (what)** — pick one of each. The task remembers which conversation created it, so an agent reply comes back to that same chat automatically.

## 1. Pick a TRIGGER (when)

| Want | kind | Set |
|---|---|---|
| One specific moment | `once` | `at`="YYYY-MM-DD HH:MM" (local time) or `in_sec` |
| A repeating clock time | `cron` | `cron_expr` = "min hour day month weekday" |
| Every N seconds | `interval` | `interval_sec` |
| Whenever a system event happens (e.g. each boot) | `on_event` | `trigger_event` |

cron supports ranges / lists / steps: `0 9 * * 1-5` = 09:00 Mon–Fri · `30 8 * * *` = 08:30 daily · `0 */2 * * *` = every 2 hours · `0 9 1,15 * *` = 09:00 on the 1st and 15th.

`on_event` only accepts events the device actually fires. If `add` rejects your `trigger_event`, read the error — it lists the supported ones. `on_event` fires before the clock is synced, which is exactly why it is the right trigger for "do X every time the device powers on".

## 2. Pick an ACTION (what)

| Want | action | Set |
|---|---|---|
| Something needing judgement, tools, or a reply to the user | `agent` (default) | `prompt` |
| Call a capability directly, no LLM (deterministic) | `cap` | `cap_id` (+ `cap_args`) |
| Emit a raw event for router rules (advanced) | `emit` | `event_type` |

For `action=agent`, phrase `prompt` as the message to **deliver** or the thing to **do once** — e.g. "tell the user it's time for the meeting" — not "remind me to…". It runs a single turn and must not create more scheduled tasks.

## 3. Common combinations (guidance, not a fixed menu)

- **Reminder / alarm at a time** → `once` or `cron` + `action=agent`.
- **Periodic check that talks to the user** → `cron`/`interval` + `action=agent`.
- **Deterministic hardware action on a schedule** (drive a pin, play a tone, water a plant) → `cron` + `action=cap` pointing at the capability that does it.
- **Run a script every time the device boots** → `on_event` + `trigger_event` (the device-online event) + `action=cap` with the lua-run capability. For example:
  ```json
  {"kind":"on_event","trigger_event":"wifi_connected","action":"cap",
   "cap_id":"lua_run","cap_args":{"path":"vfs:/scripts/<name>.lua"}}
  ```
  Use `cap_id:"lua_run_async"` instead for a script that loops forever (a monitor or animation). Persist the script under `vfs:/scripts/` (survives reboot) and give it a global `run(args)`.

Mix freely — any trigger pairs with any action. The table is a starting point, not the only valid shapes.

## 4. Clock-based tasks need a clock + timezone

`once` / `cron` (and any `interval` you want aligned to wall time) only fire once the clock is synced AND a timezone is set. If `add` warns the task is **SUSPENDED**, or `get_local_time` says the timezone is not configured, ask the user their timezone and call `set_timezone` first — otherwise the task never fires. `on_event` does not need the clock.

## 5. Manage & verify

- `scheduler_list_jobs` / `scheduler_get_job` — see `next_fire` (local time) and run/missed counts.
- `scheduler_trigger_now` — run the action once to confirm it works, without touching the schedule.
- `scheduler_pause_job` / `scheduler_resume_job` — skip temporarily (e.g. "no alarm today") without deleting.
- `scheduler_disable_job` / `scheduler_enable_job` / `scheduler_remove_job` — stop or delete.

After creating a clock-based reminder, it's good practice to `trigger_now` once so the user sees it works before they rely on it.

# thread — require("thread")

`thread` gives skill scripts two things: native job orchestration
(`run/start/list/get/stop` — start and manage other scripts as jobs without
going through `cap.call()`'s JSON round-trip) and cross-job synchronization
primitives (`thread.sync` — queue/semaphore/lock) so those jobs can hand data
/ coordinate access without polling a `vfs:` file.

## thread.run/start/list/get/stop — job orchestration

```lua
local thread = require("thread")

ok, result = thread.run(path [, args] [, opts])
-- Synchronous: blocks THIS job until the script at `path` finishes.
-- opts.timeout_ms: 0/absent -> default (~30s).
-- On success: true, result  (result is a table: {result=.., stdout=..,
--   stdout_truncated=..}). On failure: nil, err_string.
-- Consumes a SECOND concurrent slot from the shared job budget for the
-- duration (this job + the one it's running) — see thread.start if you
-- don't need to block.

job_id, err = thread.start(path [, args] [, opts])
-- Async: starts `path` as a new job and returns immediately.
-- opts.timeout_ms: 0/absent -> unbounded (runs until thread.stop).
-- opts.name / opts.exclusive: optional job-name / exclusive-group strings —
--   starting a 2nd job with the same name/exclusive while one is still
--   active fails with "conflict: ..." unless opts.replace=true (which stops
--   the old one first).
-- On success: job_id (a number, pass it to get/stop). On failure: nil, err.

jobs = thread.list([status])
-- status: exact-case match against "QUEUED"|"RUNNING"|"DONE"|"FAILED"|
--   "TIMEOUT"|"STOPPED", or nil/"all" for everything.
-- Returns an array of {job_id=.., status=.., name=..}.

info, err = thread.get(job_id)
-- info = {job_id=.., status=.., path=.., log_seq=.., log_truncated=..,
--          started_ms=.., finished_ms=.., log=..}
-- `log` is the job's captured print() output (ring buffer, may be
-- [TRUNCATED]-suffixed). On failure (job_id not found): nil, err.

result, err = thread.stop(job_id)
-- Requests a stop and waits up to a fixed ~2s grace period for the job to
-- actually terminate (no per-call override).
-- result = {job_id=.., stopped=true|false, status=..[, hint=..]}.
-- stopped=true means it reached a terminal state in time — status may
-- still read "DONE" rather than "STOPPED" if the script's own code (e.g. a
-- thread.sync call) observed the cancellation and returned normally instead
-- of erroring; check the job's log via thread.get() if you need to
-- distinguish "finished on its own" from "was cancelled".
```

### Cancellation model — what `stop` does to the target job

A stopped job is cancelled **cooperatively**: at its next checkpoint the runtime
raises a Lua error that propagates up and unwinds the whole job. `pcall` cannot
swallow it (it is re-raised), so `stop` always wins. Checkpoints are the
instruction-count hook (fires periodically inside any Lua loop / computation) and
blocking library calls — in particular **`sys.sleep_ms` throws**
`"skill execution cancelled"` if cancelled mid-sleep. The one exception is
`thread.sync.*` (queue/sem/lock): those RETURN `nil/false, "stopped"` instead of
throwing (see Errors), so a job blocked in one of them can observe the stop and
exit cleanly.

Consequence: **a job you stop generally cannot run its own cleanup / final-report
code** — if it was sleeping or computing at the moment of stop it dies at that
line, mid-work. If you need a worker's final state (counts, summary, etc.) after
stopping it, do NOT expect the worker to print/return it on the way out. Instead
either: (a) have the PARENT read/aggregate that state after `thread.stop` returns
(e.g. from a shared `thread.sync` object or the worker's own log), or (b) don't
stop it at all — have the worker poll a shared cooperative flag between iterations
and return normally on its own, so its post-loop cleanup actually runs.

Deviations from the original design (documented, not oversights):

- `get`/`stop` take a numeric `job_id` only — the original additionally accepts
  a job name string.
- `stop`'s grace period is fixed, not a per-call `wait_ms` argument.

Started jobs run in the SAME shared concurrency budget as the LLM's
`lua_run_async` tool calls (`LUA_JOB_MAX_RUNNING`, currently 4) — a script
that starts 2 children while itself running synchronously (`thread.run` /
launched via `lua_run`) is using up to 3 of those slots at once.

## thread.sync — named cross-job objects

All objects are identified by a shared **name string** (1-32 chars,
`[A-Za-z0-9_.:-]`) — any job that knows the name can attach to the same
object created by another job. Objects do NOT survive past a device reboot.

### Lifetime — auto-reclaimed when the job world goes idle

A sync object created from inside a job (or a `lua_run`) lives as long as **any**
job is still running, then is **automatically reclaimed the moment the last job
finishes** and the system goes idle. You do not have to `*_delete` them, and —
importantly — they do **not** linger into the next run.

Why this matters: because of the cancellation model (see above), a job that is
`stop`ped mid-work cannot run its own cleanup — a stopped producer never gets to
`*_delete` its queue, and a stopped holder never gives its semaphore/lock back.
Without auto-reclaim, that half-used object would survive globally and poison the
next run that re-creates the same name (e.g. a semaphore stuck at 0 → workers
that can never acquire it). Auto-reclaim at idle wipes the slate, so every run
starts from a clean, freshly-created set of objects regardless of how the
previous run ended.

Consequences to design around:
- A "launcher" parent that creates the shared objects and then exits immediately
  is fine — the objects stay alive for its children and are reclaimed only once
  **all** those children have also finished.
- Do NOT rely on a sync object persisting across a gap when **no** job is
  running (e.g. handing data from one run to a later, separate run) — it will
  have been reclaimed. Use `claw_memory` / a file for cross-run state.
- `*_delete` still works and is the way to release an object early, while jobs
  are still running.
- Objects created from an interactive `AT+CLAW=lua_repl` session are NOT
  job-scoped and persist until you delete them or reboot.

### Queue — producer/consumer byte-string messages

```lua
local thread = require("thread")

ok, err = thread.sync.queue_create(name [, opts])
-- opts.depth      : 1..32,   default 8   (number of slots)
-- opts.item_size  : 1..4096, default 256 (max bytes per message)
-- returns true on success; nil, err on failure ("exists"|"limit"|"no_mem")

ok, err = thread.sync.queue_send(name, value_string [, timeout_ms])
-- value_string must be <= item_size bytes. Blocks up to timeout_ms (default
-- 0 = no wait) if the queue is full. Returns false, "timeout"/"not_found"/
-- "stopped" on failure ("stopped" = the job was cancelled while waiting).

value, err = thread.sync.queue_recv(name [, timeout_ms])
-- Blocks up to timeout_ms waiting for a message. Returns nil, err
-- ("timeout"|"not_found"|"stopped") on failure.

ok, err = thread.sync.queue_delete(name)
-- Fails with "busy" if any job is currently blocked in send/recv on it, or
-- if unread messages remain.
```

### Semaphore — counting signal

```lua
ok, err = thread.sync.sem_create(name [, opts])
-- opts.max     : 1..255, default 1  (ceiling)
-- opts.initial : 0..max, default 0 (starting count)

ok, err = thread.sync.sem_give(name)      -- wakes exactly ONE waiter (not a
                                          -- broadcast); false, "full" at ceiling
ok, err = thread.sync.sem_take(name [, timeout_ms])
ok, err = thread.sync.sem_delete(name)    -- "busy" if any job waiting
```

One `sem_give` unblocks exactly one `sem_take`. It is NOT a broadcast/barrier: to
release N jobs that are all waiting on a single event (e.g. a shared "go" / start
gate), send N messages on a queue and have each job `queue_recv` one, or call
`sem_give` N times — a single give only ever wakes one waiter.

### Lock — mutex with ownership check

```lua
ok, err = thread.sync.lock_create(name)
ok, err = thread.sync.lock(name [, timeout_ms])     -- acquire
ok, err = thread.sync.unlock(name)                  -- "not_owner" if you
                                                      -- don't hold it
ok, err = thread.sync.lock_delete(name)             -- "busy" if held/waited
```

## Errors

Every function returns `true`/`value` on success, or `nil-or-false, err` on
failure (never throws for expected conditions). Error strings: `"not_found"`,
`"exists"`, `"limit"` (hit the global object cap), `"no_mem"`, `"busy"`,
`"timeout"`, `"full"` (sem at ceiling), `"not_owner"` (unlock by non-holder),
`"stopped"` (job was cancelled while blocked).

## Pattern: a parent job starting + coordinating two children

```lua
-- parent.lua — started via AT+CLAW=lua_execute_sync / lua_execute_async, or by the LLM's lua_run tool
local thread = require("thread")
thread.sync.queue_create("frames", {depth = 4, item_size = 2048})

local producer_id = thread.start("vfs:/skills/producer.lua")
local consumer_id = thread.start("vfs:/skills/consumer.lua")

-- poll until both terminate
while true do
  local p = thread.get(producer_id)
  local c = thread.get(consumer_id)
  if p.status ~= "RUNNING" and p.status ~= "QUEUED"
     and c.status ~= "RUNNING" and c.status ~= "QUEUED" then
    break
  end
  sys.sleep_ms(100)
end
thread.sync.queue_delete("frames")
```

```lua
-- producer.lua
local thread = require("thread")
function run(args)
  while true do
    local frame = capture_one_frame()
    local ok = thread.sync.queue_send("frames", frame, 200)
    if not ok then print("frames queue full, dropped a frame") end
  end
end
```

```lua
-- consumer.lua
local thread = require("thread")
function run(args)
  while true do
    local frame, err = thread.sync.queue_recv("frames", 5000)
    if frame then process(frame) end
  end
end
```

**Available in skill scripts only** (not the REPL).

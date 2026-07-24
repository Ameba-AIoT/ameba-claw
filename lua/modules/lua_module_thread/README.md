# lua_module_thread

Job orchestration + cross-job synchronization primitives for skill scripts
(`require("thread")`), built on ameba's `os_wrapper` (rtos_*) primitives,
cooperative-cancel convention, and (for job binding) a new plain-C API added
to `cap_lua.h` (D2).

- `src/lua_module_thread.c` — `luaopen_thread`, assembles the `thread` table
- `src/thread_sync.c` — `thread.sync`: named queue / counting-semaphore / lock
  registry, shared across all jobs in the process
- `src/thread_job.c` — `thread.run/start/list/get/stop`, calling
  `cap_lua_run_script{,_async}` / `cap_lua_{list_jobs,get_job,stop_job}`
  directly (no `cap.call()` JSON round-trip); table<->JSON conversion is
  delegated to the already-loaded `cjson` module
- `docs/thread.md` — LLM-facing API reference (staged into `rolfs:/docs`)
- `test/test_thread_sync.lua` — single-script self-test of create/timeout/busy
  semantics (no cross-job case, since it predates job binding)
- `test/thread_full_flow_{parent,child_producer,child_consumer}.lua` — a
  parent job starts 2 children via `thread.start()`; they hand off 5 items
  through a `thread.sync` queue; parent polls `thread.get()` to completion
- `test/test_thread_stop.lua` + `thread_stop_child_blocker.lua` — starts a
  child blocked on a 20s `queue_recv`, `thread.stop()`s it, and asserts it
  terminates in ~50ms (not 20s) with the wait call observing `"stopped"`
- `test/test_thread_job_named_replace.lua` / `test_thread_job_exclusive.lua`
  — start a long-blocked job under `opts.name` (resp. `opts.exclusive`),
  confirm a same-name/-group `thread.start()` without `replace` fails with
  "conflict: ...", then confirm `replace=true` stops the old job and takes
  its place

## Status

Batches A, B, and C are all implemented and hardware-verified on
`RTL8721F_COM23_DUT`: `thread.sync` (queue/sem/lock), job lifecycle binding
(`thread.run/start/list/get/stop`), and the name/exclusive conflict +
replace tests. See
`internal_project_ctrl/dev_schedule/core_lua_gap_plan.md` 批次一 #1 and
`design_spec/lua/lua_module_thread_architecture.md` for the plan and the decisions
(D1: bump `LUA_JOB_MAX_RUNNING` 2->4; D2: add the plain-C job API; D3: SKILL
sandbox only, not REPL) it was built against.

## Design notes

- Objects live in one global singly-linked list guarded by a single registry
  mutex (`rtos_mutex_t`), same shape as the original design — this is a
  handful of objects at most (`CLAW_LUA_THREAD_SYNC_MAX_OBJECTS`, see
  `ameba_claw_defs.h`), not a hot path, so a linked list + one lock is fine.
- Blocking calls (`queue_send/recv`, `sem_take`, `lock`) chop their timeout
  into `CLAW_LUA_THREAD_WAIT_STEP_MS` steps and check the cooperative
  `__cancel_ptr` registry slot between steps — the same convention
  `lua_module_event.c::lua_event_wait` uses.
  For an ASYNC job this key is armed before `run()` starts, so cancellation
  works correctly inside a `thread.sync` wait; a *sync* `lua_run`'s
  top-level code (before `run()` is called) is the one window where it
  isn't armed yet — batch A's single-script test never blocks there long
  enough for this to matter.
- `waiter_count` on each object blocks delete-while-blocked races: delete
  returns `"busy"` while any call is inside acquire/release for that object.
- SKILL sandbox only, not the REPL (D3) — `thread.*`'s job orchestration
  would otherwise let a REPL user race the LLM for the shared job budget in
  ways nobody has tested.
- `cap_lua_run{,_async}`/`cap_lua_job_{get,list,stop}` (the LLM-facing JSON
  tools in `cap_lua_cmd.c`) were refactored (not rewritten) into thin
  JSON-parsing wrappers around the new plain-C core functions — their
  observable behavior, including the `vfs:/tmp/` + LLM-caller exception, is
  unchanged; only `thread_job.c`'s callers get the new no-JSON entry points.

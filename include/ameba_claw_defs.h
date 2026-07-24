#ifndef AMEBA_CLAW_DEFS_H
#define AMEBA_CLAW_DEFS_H

/*
 * ameba_claw_defs.h — single source of truth for all compile-time tunable
 * limits and fallback defaults.
 *
 * For WebUI-configurable runtime values (LLM model/url/key, WiFi, IM tokens,
 * search) see claw_modules/claw_config/include/claw_config.h.
 */

/* ═══════════════════════════════════════════════════════════════════
 * Tool / Cap I/O limits
 * These are compile-time only — not adjustable via WebUI.
 * ═══════════════════════════════════════════════════════════════════ */

/* Maximum bytes a single tool-call result may occupy when added to the LLM
 * messages array. Excess is JSON-aware truncated (largest string field
 * shortened) and TOOL_RESULT_TRUNCATION_SUFFIX is appended.
 * Used by: claw_agent.c → tool_output_truncate() */
#define TOOL_RESULT_MAX_BYTES           (48 * 1024)

/* Suffix appended to any content that was truncated before sending to LLM.
 * Used by: claw_agent.c → tool_output_truncate()
 *          cap_files.c  → execute_read_file() */
#define TOOL_RESULT_TRUNCATION_SUFFIX   "...[output truncated]"

/* Maximum bytes read from a file by the read_file capability tool.
 * Also used as the upper limit for the WebUI /api/files/content endpoint
 * so both the LLM and the user see the same slice of a large file.
 * Used by: cap_files.c  → s_max_read (via ameba_claw_main.c config)
 *          cap_webui.c  → handle_api_files_content() / handle_api_files_download()
 * The KB form is the single source of truth so the read_file tool description
 * (which stringifies it) can never drift from the byte limit. */
#define CAP_FILES_MAX_READ_KB           48
#define CAP_FILES_MAX_READ_SIZE         (CAP_FILES_MAX_READ_KB * 1024)


/* ═══════════════════════════════════════════════════════════════════
 * Capability lifecycle registry (claw_cap_registry)
 * These are compile-time only — not adjustable via WebUI.
 * ═══════════════════════════════════════════════════════════════════ */

/* Maximum number of capability descriptors that may self-register via
 * CLAW_CAP_REGISTER() / __attribute__((constructor)). The registry stores
 * pointers in a fixed static array (no heap, constructors run before the
 * allocator exists); registrations beyond this are silently dropped and an
 * overflow flag is set (asserted at app_example fail-fast). Size for the full
 * cap set (~25) with headroom.
 * Used by: claw_modules/claw_cap_registry/src/claw_cap_registry.c */
#define CLAW_CAP_REGISTRY_MAX           48


/* ═══════════════════════════════════════════════════════════════════
 * Agent engine
 * These are compile-time only — not adjustable via WebUI.
 * ═══════════════════════════════════════════════════════════════════ */

/* Stack size for the engine_task FreeRTOS task. Must accommodate a full
 * LLM HTTPS handshake; do not set below 28 KB.
 * Used by: ameba_claw_main.c → claw_agent_config_t.task_stack_size */
#define CLAW_AGENT_STACK_SIZE           (32 * 1024)

/* Depth of the agent request queue. Submitters block when full.
 * Used by: ameba_claw_main.c → claw_agent_config_t.request_queue_len */
#define CLAW_AGENT_REQUEST_QUEUE_LEN    4

/* Maximum number of registered context providers (system_prompt / messages /
 * tools types combined).
 * Used by: ameba_claw_main.c → claw_agent_config_t.max_context_providers */
#define CLAW_AGENT_MAX_CONTEXT_PROVIDERS 10

/* Minimum number of tool-call iterations enforced even if the runtime config
 * requests fewer. Ensures multi-step skill creation has enough headroom.
 * Used by: claw_agent.c → iter_limit clamp */
#define CLAW_AGENT_TOOL_ITER_MIN        10

/* ── LLM API call retry (application-level) ──────────────────────────
 * The transport layer (llm_agent_http.c) already retries connect / TLS
 * handshake / empty-response failures. THESE constants govern a higher-level
 * retry around the whole chat call, so a *successful HTTP exchange that
 * carries an error body* — e.g. GLM's "该模型当前访问量过大，请您稍后再试"
 * rate-limit / overload (HTTP 429 or a 200 with an `error` field) — is
 * automatically re-attempted instead of failing the turn immediately.
 *
 * Total attempts (first try + retries). 3 → original call + 2 retries.
 * Used by: claw_agent_llm.c → claw_agent_llm_chat_messages() retry loop */
#define CLAW_AGENT_LLM_RETRY_MAX_ATTEMPTS   3

/* Delay between attempts, in milliseconds. Sits in the requested 5–10 s band;
 * a single fixed wait keeps the model from hammering an overloaded endpoint.
 * Used by: claw_agent_llm.c → claw_agent_llm_chat_messages() retry loop */
#define CLAW_AGENT_LLM_RETRY_DELAY_MS       7000u


/* ═══════════════════════════════════════════════════════════════════
 * Memory / Session
 * These are compile-time only — not adjustable via WebUI.
 * ═══════════════════════════════════════════════════════════════════ */

/* Hard cap on long-term structured memory entries. Oldest entry is evicted
 * (FIFO) when the store is full. Defined here for visibility; the actual
 * constant lives in claw_memory.c (LT_MAX_ITEMS) and must be kept in sync.
 * Used by: claw_memory.c → LT_MAX_ITEMS */
#define CLAW_MEMORY_LT_MAX_ITEMS        64

/* Ring-buffer depth for per-session conversation history (turns).
 * Token-budget compaction is the real boundary; this is a safety cap.
 * Used by: ameba_claw_main.c → claw_memory_config_t.max_session_turns */
#define CLAW_MEMORY_MAX_SESSION_TURNS   64

/* Number of most-recent turns preserved verbatim during compaction.
 * Used by: ameba_claw_main.c → claw_memory_config_t.compaction_protect_last */
#define CLAW_MEMORY_COMPACTION_PROTECT  4

/* Per-turn cap on the serialized tool round-trip blob stored in the session
 * file for byte-identical cross-turn replay. A turn whose tool history exceeds
 * this is stored as text only (loses cross-turn tool replay). MUST exceed
 * TOOL_RESULT_MAX_BYTES plus JSON-escaping overhead, otherwise a single
 * full-size tool result (e.g. a large read_file) would never survive into the
 * next turn — which silently strips all tool history from the session.
 * Whole-file size is still bounded separately by the session soft-max shedding.
 * Used by: claw_memory.c → CLAW_MEMORY_TOOLMSGS_PER_TURN_MAX */
#define CLAW_MEMORY_TOOLMSGS_PER_TURN_MAX  (TOOL_RESULT_MAX_BYTES + 16 * 1024)


/* ═══════════════════════════════════════════════════════════════════
 * IM Local (WebUI live chat)
 * These are compile-time only — not adjustable via WebUI.
 * ═══════════════════════════════════════════════════════════════════ */

/* Number of slots in the /updates ring buffer.
 * Used by: cap_im_local.c → MSG_BUF_MAX */
#define CLAW_IM_LOCAL_MSG_BUF_MAX       32

/* Maximum bytes stored per message in the ring buffer (heap-allocated, per slot).
 * Used by: cap_im_local.c → MSG_TEXT_MAX */
#define CLAW_IM_LOCAL_MSG_TEXT_MAX      (16 * 1024)

/* Maximum bytes accepted from a single WebSocket message (heap-allocated
 * transiently in ws_on_message, freed after dispatch).  Governs how much text
 * a WebUI user may send in one turn.  Must not exceed available heap headroom.
 * Used by: cap_im_local.c → ws_on_message */
#define CLAW_IM_LOCAL_WS_TEXT_MAX       (16 * 1024)

/* Maximum tool-progress messages pushed to an IM channel per LLM request.
 * Prevents rate-limiting on channels such as WeChat.
 * Used by: ameba_claw_main.c → on_tool_progress callback */
#define CLAW_IM_PROGRESS_BUDGET         4

/* Instant acknowledgement sent to IM channels while the agent loop runs.
 * Single source of truth — used by claw_event_dispatcher and cap_atcmd.
 * Used by: claw_event_dispatcher.c → handle_event()
 *          cap_atcmd.c            → AT+CLAW=ask handler */
#define CLAW_IM_ACK_MSG                 "ameba claw is working on it..."


/* ═══════════════════════════════════════════════════════════════════
 * Event dispatcher / rules
 * These are compile-time only — not adjustable via WebUI.
 * ═══════════════════════════════════════════════════════════════════ */

/* Max length of a rule's instant-ack template (rendered and sent to the
 * source IM channel the moment a rule matches, BEFORE its actions run).
 * Stored inline in claw_event_dispatcher_rule_t.
 * Used by: claw_event_dispatcher.h → rule.ack[] */
#define CLAW_DISPATCHER_RULE_ACK_MAX      256

/* Max length of a match text pattern (EXACT / PREFIX modes). The legacy
 * substring field (text_contains) keeps its own 96-byte size.
 * Used by: claw_event_dispatcher.h → match.text[] */
#define CLAW_DISPATCHER_MATCH_TEXT_MAX    96

/* Max length of a per-action only_if guard's left template / right literal.
 * Used by: claw_event_dispatcher.h → action.only_if */
#define CLAW_DISPATCHER_GUARD_LEFT_MAX    128
#define CLAW_DISPATCHER_GUARD_RIGHT_MAX   64

/* Truncation cap for a captured action output fed into @{last.output} when it
 * is stored as a plain string (non-JSON, or JSON above the parse cap below).
 * Bounds the heap held by the per-rule render context during a tool-call
 * chain (one cap output can be large; we only keep a bounded prefix).
 * Used by: claw_event_dispatcher.c → ctx_set_last() last.output capture */
#define CLAW_DISPATCHER_LAST_OUTPUT_MAX   1024

/* When a captured action output parses as a JSON object/array AND its raw text
 * is within this bound, it is stored PARSED into ctx.last.output so a later
 * action can reference a single field (@{last.output.answer}) instead of being
 * forced to splice the whole serialized blob — mirrors how event.payload is
 * parsed. Above this size we fall back to the bounded string form. Generous
 * (a structured cap result like web_search is a few KB) yet bounds the
 * transient parsed tree per event; the tree is freed when the rule finishes.
 * Used by: claw_event_dispatcher.c → ctx_set_last() */
#define CLAW_DISPATCHER_LAST_OUTPUT_PARSE_MAX   4096

/* Max rules snapshotted per event in Phase 2. Each matched rule is deep-
 * copied onto the HEAP (not the stack) to preserve rule boundaries for the
 * per-rule render context; this only bounds work/heap per event. Reuses the
 * match-per-event ceiling (a rule can only be snapshotted if it matched).
 * Used by: claw_event_dispatcher.c → handle_event() */
#define CLAW_DISPATCHER_MAX_SNAP_RULES    8

/* Max hops in an rtk_emit chain (a rule's action re-publishes a derived event
 * that other rules may match). Each derived event carries a generation counter
 * (claw_event_t.emit_depth); an emit is refused once the current event is
 * already at this depth. Without this bound a rule whose emitted event re-
 * matches (e.g. a self-matching rule) would storm the queue forever — the
 * derived event keeps the parent's text, so it re-hits the same rule. Value 3
 * allows short A→B→C funnels while capping any runaway to a few log lines.
 * Used by: claw_event_dispatcher.c → dispatch_one_action() EMIT case */
#define CLAW_DISPATCHER_MAX_EMIT_DEPTH    3


/* ═══════════════════════════════════════════════════════════════════
 * HTTP server
 * These are compile-time only — not adjustable via WebUI.
 * ═══════════════════════════════════════════════════════════════════ */

/* TCP port the embedded HTTP server listens on.
 * Used by: ameba_claw_main.c → claw_http_server_config_t.port */
#define CLAW_HTTP_PORT                  8080

/* Maximum POST body size accepted. Requests exceeding this return 413.
 * Sized to comfortably fit a multipart upload of a near-1 MB image
 * (CLAW_DISPLAY_JPEG_MAX_FILE) plus its multipart framing overhead — the
 * WebUI file manager streams the whole body into one on-demand heap buffer
 * (rtos_mem_malloc(content_length+1)), so this only costs heap while an
 * upload is in flight (device has 10 MB+ free).
 * Used by: claw_http_server.c → HTTP_MAX_BODY_SIZE */
#define CLAW_HTTP_MAX_BODY_SIZE         (2 * 1024 * 1024)


/* ═══════════════════════════════════════════════════════════════════
 * Time / SNTP
 * These are compile-time only — not adjustable via WebUI.
 * ═══════════════════════════════════════════════════════════════════ */

/* Minimum Unix timestamp accepted as a valid, SNTP-synced wall-clock time.
 * On Realtek RTL8721F the C-library time() returns the system uptime in
 * seconds (a small positive integer) before SNTP sets the wall clock, so
 * the naive `now <= 0` check in cap_time is insufficient — it lets through
 * uptime-based near-epoch values that look like 1970-01-01.
 * Value: 2025-01-01 00:00:00 UTC = 1735689600.  Any timestamp below this
 * is treated as "not yet synced" and returns an error to callers.
 * Used by: cap_time.c → execute_get_local_time(), collect_time_context() */
#define CLAW_TIME_MIN_VALID_UNIX        1735689600L   /* 2025-01-01 00:00:00 UTC */

/* ═══════════════════════════════════════════════════════════════════
 * Scheduler
 * These are compile-time only — not adjustable via WebUI.
 * ═══════════════════════════════════════════════════════════════════ */

/* Maximum number of persistent scheduled jobs.
 * Must match cap_scheduler.c MAX_JOBS (kept in sync manually).
 * Used by: ameba_claw_main.c → cap_scheduler_config_t.max_jobs */
#define CLAW_SCHEDULER_MAX_JOBS         16


/* ═══════════════════════════════════════════════════════════════════
 * WebUI-adjustable runtime defaults (fallback values only)
 *
 * These are the initial values written into s_core_cfg at startup.
 * At boot, claw_config_get() overrides them from vfs:/claw_config.json.
 * The user can change them via WebUI or AT+CLAW=cfg.
 * ═══════════════════════════════════════════════════════════════════ */

/* LLM limits come from claw_config — see CLAW_CONFIG_DEFAULT_LLM_MAX_TOKENS
 * and CLAW_CONFIG_DEFAULT_LLM_MAX_TOKENS in claw_config.h.
 * ameba_claw_main.c reads them from cfg->llm.* directly. */

/* Tunable: maximum time-to-first-byte allowed per LLM call (streaming mode only).
 * In streaming mode the server sends the first token within seconds; if TTFB
 * exceeds this threshold the server is considered unresponsive and the request
 * is aborted. Not checked in non-streaming mode (where TTFB ≈ full generation
 * time and can legitimately be long). */
#define CLAW_AGENT_LLM_TTFB_TIMEOUT_MS  300000u   /* 300 seconds */

/* Tunable: socket recv timeout for TLS LLM calls.
 * In non-streaming mode the server buffers the entire generation before sending
 * the first byte, so this must cover the full generation time. For a 16K-token
 * output at ~30 tok/s that is ~9 minutes — 300 s gives comfortable headroom.
 * Also applies as the inter-chunk idle timeout in streaming mode (rarely trips
 * because tokens arrive continuously). */
#define CLAW_AGENT_LLM_RECV_TIMEOUT_MS  300000u  /* 300 seconds, TLS path */

/* Tunable: socket recv timeout for plain-HTTP LLM calls (local debug proxy, e.g. ccglass).
 * Plain HTTP is only used when the configured URL port is not 443.  SO_RCVTIMEO is a
 * per-recv()-call deadline: after the HTTP headers arrive (TTFB), the next recv() waits
 * for the first SSE data chunk.  For large contexts (>60KB) the upstream LLM can take
 * >60 s to emit the first token, so this must match the TLS path (300 s).
 * Used by: llm_agent_http.c llm_http_post_once */
#define CLAW_AGENT_LLM_PLAIN_RECV_TIMEOUT_MS  300000u  /* 300 seconds, plain-HTTP path */


/* Enable lua script time check */
// #define CLAW_LUA_TIME_LOG_ENABLE

/* ── Lua execution ──────────────────────────────────────────────────
 * Constants for cap_lua's async job task and error capture.
 * ─────────────────────────────────────────────────────────────────── */

/* Size of the heap-allocated error message buffer used in lua_async_task
 * to capture early-init failures (Lua state OOM, script not found, compile
 * error, missing run()) before the job log is accessible. Sized to hold a
 * full compile error with file/line info (luaO_chunkid + error message).
 * Used by: cap_lua.c → lua_async_task() init_err */
#define CLAW_LUA_INIT_ERR_MAX        192

/* FreeRTOS stack size for async Lua job tasks (lua_async_task).
 * Must fit sandbox init (multiple luaopen_* / luaL_requiref frames),
 * luaL_loadstring parse chain, run() pcall frame, and the heap overhead
 * of CLAW_LUA_INIT_ERR_MAX allocated on first entry.
 * Used by: cap_lua.c → cap_lua_run_async() rtos_task_create
 *
 * NOTE: raised from 8448 to 16384 for the LVGL display binding: draw_* →
 * lv_canvas_init/finish_layer → lv_draw_dispatch has a deeper call chain than
 * the other drivers.  Heavy rasterization (glyph bitmaps, blends) runs on the
 * separate swdraw thread (LV_DRAW_THREAD_STACK_SIZE=8KB), not here, so this is
 * a safety margin — measure both stacks with uxTaskGetStackHighWaterMark and
 * tune (phase1 §8/§9).
 *
 * NOTE: raised again from 16384 to 32768 so a background job may itself open a
 * TLS connection — e.g. a continuous camera→vision monitor that calls
 * cap.call("vision_describe", ...) inside its loop.  The vision HTTPS/mbedTLS
 * handshake runs ON THIS TASK's stack (cap.call is synchronous), and 16K
 * overflowed: engine_task needs its full 32K to "barely" fit one TLS handshake
 * (see CLAUDE.md — memory_extract was split to its own task for exactly this),
 * and this task must also hold the Lua sandbox + run() frames on top.  Only the
 * currently-running job slots pay this (tasks are created on demand in
 * cap_lua_run_async and freed on completion), not a fixed pool. */
#define CLAW_LUA_ASYNC_TASK_STACK    32768

/* ── Lua thread module (lua_module_thread: thread.sync queue/sem/lock) ──
 * Cross-job synchronization primitives. Registry is a global singly-linked
 * list guarded by one mutex; these bound its memory footprint and the size
 * of any one queue payload.
 * Used by: lua/modules/lua_module_thread/src/thread_sync.c
 * ─────────────────────────────────────────────────────────────────── */

/* Max number of live sync objects (queues+sems+locks combined) across all
 * jobs. Set to 32; ameba's job budget (LUA_JOB_MAX_RUNNING=4, see
 * cap_lua_internal.h) is small, so this is generous headroom, not a tight fit. */
#define CLAW_LUA_THREAD_SYNC_MAX_OBJECTS      32

/* Max length of a thread.sync object name (queue/sem/lock). */
#define CLAW_LUA_THREAD_SYNC_NAME_MAX         32

/* thread.sync.queue_create default/max depth (number of slots). */
#define CLAW_LUA_THREAD_QUEUE_DEPTH_DEFAULT   8
#define CLAW_LUA_THREAD_QUEUE_DEPTH_MAX       32

/* thread.sync.queue_create default/max per-item payload size in bytes. */
#define CLAW_LUA_THREAD_QUEUE_ITEM_SIZE_DEFAULT  256
#define CLAW_LUA_THREAD_QUEUE_ITEM_SIZE_MAX      4096

/* thread.sync.sem_create max counting-semaphore ceiling. */
#define CLAW_LUA_THREAD_SEM_MAX_COUNT         255

/* Blocking-call poll granularity (ms) for queue_send/recv, sem_take, lock —
 * each wait is chopped into steps of this size so the cooperative
 * __cancel_ptr check (same convention as lua_module_event.c::lua_event_wait)
 * runs between FreeRTOS ticks instead of blocking the whole timeout. */
#define CLAW_LUA_THREAD_WAIT_STEP_MS          50

/* ── Display (lua_module_display: LVGL ST7789 command canvas) ───────
 * Compile-time knobs for the SPI TX-only panel transport and the LVGL
 * draw-buffer allocation ladder.  Runtime pins/resolution come from
 * board.json via cap_board_mgr, not from here.
 * Used by: lua/modules/lua_module_display/src/{panel_spi_st7789,display_lua}.c
 * ─────────────────────────────────────────────────────────────────── */

/* SSI clock divider for the panel.  Base SPI clock ~100MHz → /4 = 25MHz.
 * 25MHz is a conservative bring-up default that is reliable over dupont /
 * breadboard wiring; ST7789 writes tolerate up to ~50MHz (/2) on clean PCB
 * traces.  Lower the divider (raise the clock) only once wiring is solid. */
#define CLAW_DISPLAY_SPI_CLKDIV       2

/* Busy-wait guard for the CPU polling command writer (panel_send_cmd).
 * Bounds a wedged TX FIFO so a HW fault degrades to a blank frame, not a hang. */
#define CLAW_DISPLAY_SPI_POLL_GUARD   1000000u

/* Timeout (ms) waiting for a color-block TX-DMA completion in panel_send_color.
 * A full 240x240 frame at 25MHz is ~35ms; 2000ms leaves ample slack. */
#define CLAW_DISPLAY_DMA_TIMEOUT_MS   2000

/* ST7789 hard-reset timing: RST low >=10ms, then wait >=120ms after release
 * before issuing commands (datasheet).  Values include a small margin. */
#define CLAW_DISPLAY_RESET_LOW_MS     15
#define CLAW_DISPLAY_RESET_HIGH_MS    130

/* Default panel geometry used when board.json omits a `resolution` field. */
#define CLAW_DISPLAY_DEFAULT_W        240
#define CLAW_DISPLAY_DEFAULT_H        240

/* Partial draw-buffer allocation ladder (fraction of full-screen height).
 * The canvas widget already holds the full-screen image (mandatory 112KB for
 * 240x240 RGB565); the draw buffer is only LVGL's flush staging area, so a
 * partial buffer is the memory-sane default (see phase1 §4.7 memory account).
 * Start at 1/START_DIV of the screen and shrink toward 1/FLOOR_DIV on OOM.
 * FLOOR_DIV=8 keeps each partial >=1/10 screen (LVGL partial-mode minimum). */
#define CLAW_DISPLAY_DRAWBUF_START_DIV  4
#define CLAW_DISPLAY_DRAWBUF_FLOOR_DIV  8

/* ── LCDC/RGB display backend (phase3_lcdc_rgb_st7701p.md) ──────────
 * Cross-panel compile-time constants for display_backend_lcdc.c.  Per-panel
 * electrical values (timing / polarity / SPI-init sequence) live in
 * lua/modules/lua_module_display/src/panel/lcdc/lcdc_panels.h; per-board pins
 * live in board.json.  Only knobs common to all RGB panels belong here.
 * Used by: lua/modules/lua_module_display/src/backend/display_backend_lcdc.c */

/* 9-bit register-init SPI clock (Hz) for panels that need it (e.g. ST7701P).
 * Matches the SDK's panel_spi_init.c (spi_frequency 5MHz, 9-bit mode-3). */
#define CLAW_DISPLAY_LCDC_SPI_HZ      5000000

/* LCDC scan-out DMA burst: LCDC_DMA_BURSTSIZE_2X64BYTES (=1) → 128-byte AXI
 * burst.  Matches the KNOWN-GOOD lcdc test's rgb_init() default (which lights
 * this exact panel with no striping); the 256-byte burst (=2) was tried and
 * left dense horizontal striping.  Value is the enum, NOT a beat count. */
#define CLAW_DISPLAY_LCDC_DMA_BURST   1

/* Upper bound on RGB framebuffer geometry, used to sanity-check a panel-table
 * entry before allocating W*H*2 bytes (jd9165ba 1024x600 is the largest). */
#define CLAW_DISPLAY_LCDC_MAX_W       1024
#define CLAW_DISPLAY_LCDC_MAX_H       600

/* Tiny partial draw buffer for the LVGL display object.  We NEVER lv_refr_now
 * (canvas renders straight into the framebuffer), but the display must own a
 * valid buffer to be legal; a few lines' worth is plenty. */
#define CLAW_DISPLAY_LCDC_DUMMY_LINES 8

/* Number of full-frame LCDC scan-out buffers (page-flip pool).
 *   2 = double-buffered.  Tear-free, but present() must block ~one vblank per
 *       frame: with only two buffers the CPU's next drawing target is exactly
 *       the buffer the DMA is still scanning, so it cannot start until the flip
 *       latches.  Lowest RAM (2 x W*H*2).  Pick this when PSRAM is tight.
 *   3 = triple-buffered.  Tear-free AND the vblank wait leaves the critical
 *       path: after arming a flip the CPU always has a third, already-free
 *       buffer to draw into immediately, so frame time drops from
 *       (render+push+vblank_wait) to max(render+push, refresh_period).  Costs
 *       one extra W*H*2 buffer (480x480 RGB565 = 0.45 MB).  Default — we have
 *       PSRAM to spare and want the higher framerate.
 * Either way present() waits interrupt-driven (LCDC frame-start IRQ → semaphore),
 * NOT by busy-spinning, so the CPU is yielded to other tasks during any wait.
 * Must be 2 or 3; buffers[] is sized 3.
 * Used by: lua/modules/lua_module_display/src/backend/display_backend_lcdc.c */
#define CLAW_DISPLAY_LCDC_BUF_COUNT   3

/* present_full() page-flip: max ms to wait for the LCDC to apply the shadow-
 * reload at vblank (LCDC_BIT_VBR self-clears when done).  One frame is ~15 ms
 * @65 Hz; the cap only guards against a stalled/disabled controller so present
 * never hangs — normal exit is VBR clearing within a frame or two.  The wait is
 * interrupt-driven (frame-done IRQ posts a semaphore); this is the take timeout.
 * Used by: lua/modules/lua_module_display/src/backend/display_backend_lcdc.c */
#define CLAW_DISPLAY_LCDC_FLIP_TIMEOUT_MS 100

/* present_full() flip wait: max ms to block per iteration before re-polling the
 * VBR bit.  The frame-start IRQ normally wakes the waiter sooner; this cap makes
 * correctness independent of that IRQ firing (VBR self-clears in hardware at
 * vblank no matter what, so a short yielding re-poll always catches it).  ~2 ms
 * is well under one 65 Hz frame (~15 ms) yet still yields the CPU to other tasks.
 * Used by: lua/modules/lua_module_display/src/backend/display_backend_lcdc.c */
#define CLAW_DISPLAY_LCDC_FLIP_POLL_MS    2

/* ── Hardware JPEG image decode (display.draw_image) ────────────────
 * Constants for the hardware JPEG + PP decode path (jpeg_hw.c) behind
 * display.draw_image.  The PP downscales during decode, so RAM never holds the
 * full-res bitmap; this caps only the *compressed* JPEG file we read into RAM
 * before handing it to the decoder.  A camera-grade photo is well under this;
 * the cap rejects an accidentally-huge / non-image file rather than attempting
 * a multi-MB malloc.  Bytes.
 * Used by: lua/modules/lua_module_display/src/display_lua.c */
#define CLAW_DISPLAY_JPEG_MAX_FILE    (1u * 1024u * 1024u)

/* ── agent_auto_test harness (compile-time gated) ───────────────────
 * Master switch for the test/agent_auto_test/ peripheral-stimulus code
 * (currently AT+CLAW=gpio_ctrl). DISABLED by default so release images
 * carry none of it (zero code/image-size cost). Enable at configure
 * time via CMake `-DCLAW_AGENT_AUTO_TEST` or env CLAW_AGENT_AUTO_TEST=1,
 * which makes claw_atcmd's CMakeLists compile the source AND inject
 * -DCLAW_AGENT_AUTO_TEST=1; this fallback keeps every other TU building.
 * Used by: test/agent_auto_test/atcmd_gpio_ctrl.c, claw_atcmd/src/cap_atcmd.c */
#ifndef CLAW_AGENT_AUTO_TEST
#  define CLAW_AGENT_AUTO_TEST 0
#endif

/* ── GPIO stimulus (AT+CLAW=gpio_ctrl) ──────────────────────────────
 * Test-bench input emulation: a TESTER board drives a GPIO that is
 * wired one-to-one to a DUT button line, pulling it low (open-drain
 * emulation: drive 0 = "press", hi-Z input = "release") to synthesise
 * button gestures the DUT firmware sees as real key events.
 * Timing MUST run on-device (host serial can't hit few-ms bounce).
 * Used by: test/agent_auto_test/atcmd_gpio_ctrl.c (gated by CLAW_AGENT_AUTO_TEST)
 * ─────────────────────────────────────────────────────────────────── */

/* Default "press" duration for a single click (ms).
 * Must exceed the button engine's hardware debounce window
 * (CLAW_BTN_HW_DEBOUNCE_DIV_COUNT, ~8.2 ms) to be recognised as a valid press;
 * 100 ms is a comfortable margin. */
#define CLAW_GPIOCTRL_CLICK_MS          100

/* Default hold duration for a long-press when no explicit ms given (ms). */
#define CLAW_GPIOCTRL_LONG_MS           800

/* Gap between the two presses of a double-click (ms). */
#define CLAW_GPIOCTRL_DOUBLE_GAP_MS     150

/* Bounce test: half-period of each contact-bounce edge (ms) and the
 * number of bounce edges emitted before the contact settles pressed. */
#define CLAW_GPIOCTRL_BOUNCE_EDGE_MS    2
#define CLAW_GPIOCTRL_BOUNCE_COUNT      6

/* Max number of durations accepted in a custom `seq` gesture. */
#define CLAW_GPIOCTRL_SEQ_MAX           32

/* Per-step duration clamp (ms): bounds how long one gpio_ctrl call can
 * block the AT command task. Steps above this are capped. */
#define CLAW_GPIOCTRL_MAX_MS            10000

/* ── AT-CMD background task stacks (claw_atcmd/src) ─────────────────
 * Several AT+CLAW handlers persist config via claw_config_save() (cJSON +
 * VFS) or drive a WeChat login (TLS handshake). Both need far more stack
 * than the AT command task provides, so the handlers spawn a short-lived
 * worker and return immediately. Sizes in bytes.
 * Used by: claw_atcmd/src/atcmd_im.c, claw_atcmd/src/atcmd_cfg.c
 * ─────────────────────────────────────────────────────────────────── */

/* Config-save worker: cJSON serialize + VFS write. Covers cfg_save,
 * wifi_connect, and all IM (telegram/feishu/qq/wechat) config persists. */
#define CLAW_ATCMD_CFG_SAVE_STACK       8192

/* WiFi-clear + reboot worker: claw_config_save() via sscanf (~416 B frame). */
#define CLAW_ATCMD_WIFI_CLR_STACK       4096

/* WeChat login/QR worker: fetches QR then blocks on the confirm poll; the
 * fetch alone drives a TLS handshake, hence the large stack. */
#define CLAW_ATCMD_WECHAT_STACK         16384

/* ── Button subsystem (lua_driver_gpio_button.c) ────────────────────
 * Interrupt-driven push-button event engine layered over the gpio driver.
 * Debounce is in hardware (the GPIO debounce filter, see
 * CLAW_BTN_HW_DEBOUNCE_DIV_COUNT).  ISR top-half only timestamps the edge; a
 * lazy bottom-half (btn_drain) in the consumer lua_State resamples the level
 * once to recover press/release direction, then runs the
 * click/double/long_press/hold state machine.  All timing lives here; the
 * Lua `button` module exposes no per-pin config (single firmware, one
 * polarity).  Time base is DTimestamp_Get() (1 MHz / 1 µs free-run timer).
 * Used by: lua/modules/lua_driver_gpio/src/lua_driver_gpio_button.c
 * ─────────────────────────────────────────────────────────────────── */

/* Max simultaneously-registered button pins.  Small array + linear scan
 * (not GPIO_PIN_MAX-wide) — `on` of the 9th pin returns nil,err. */
#define CLAW_BTN_MAX_PINS            8

/* Finished-event FreeRTOS queue depth.  Full → drop (rate-limited warn);
 * `hold` repeats tolerate loss, click/double rarely overflow at 16. */
#define CLAW_BTN_EVENT_QUEUE_DEPTH   16

/* (Debounce handled entirely by hardware; see CLAW_BTN_HW_DEBOUNCE_DIV_COUNT.) */

/* Hold threshold (ms): press held this long emits one `long_press`. */
#define CLAW_BTN_LONG_MS             1500

/* Double-click window (ms): max gap between releases to coalesce as double. */
#define CLAW_BTN_DOUBLE_GAP_MS       300

/* Hold-repeat period (ms): after long_press, emit `hold` every this many ms
 * while still pressed (press-and-repeat / 连发). First hold at long+repeat. */
#define CLAW_BTN_HOLD_REPEAT_MS      200

/* get_event blocking-chunk floor (ms): clamps the computed sema wait so a
 * just-passed deadline can't yield wait<=0 and busy-spin. */
#define CLAW_BTN_WAKE_MIN_MS         1

/* get_event blocking-chunk ceiling (ms): cap each sema wait so the Lua
 * cooperative-cancel hook (__cancel_ptr) can fire — same pattern as
 * event.wait / sys.sleep_ms. */
#define CLAW_BTN_WAKE_MAX_MS         50

/* Logical active level: GPIO raw level that means "pressed".  0 = active-low
 * (button to GND + internal pull-up), the dominant push-button wiring.
 * Global (one polarity per firmware); active-high boards rebuild with 1. */
#define CLAW_BTN_ACTIVE_LEVEL        0

/* Debounce clock divisor for the GPIO hardware debounce circuit (0x00–0x7F).
 * debounce_time = (n+1) * 2 * 32µs  (32.768 kHz clock source, ~32 µs period).
 * 127 → 128 * 64 µs ≈ 8.2 ms — safely covers typical mechanical bounce (<5 ms).
 * The ISR fires only after the signal has been stable for this long, so no
 * software settle window is needed.  GPIO_DebounceClock() applies per-PORT;
 * all debounce-enabled pins on the same port share this divisor.
 * Used by: lua/modules/lua_driver_gpio/src/lua_driver_gpio.c
 *          → lua_gpio_btn_setup_hw → GPIO_DebounceClock */
#define CLAW_BTN_HW_DEBOUNCE_DIV_COUNT   127

/* Enable verbose button debug logs (edge detected / level settled / event
 * emitted).  Set to 0 before release to eliminate the log strings from rodata.
 * Used by: lua/modules/lua_driver_gpio/src/lua_driver_gpio_button.c */
#define CLAW_BTN_DEBUG               0

/* ═══════════════════════════════════════════════════════════════════
 *  Capacitive touch panel — GT911 (lua_driver_touch, phase4_touch_gt911.md)
 * ═══════════════════════════════════════════════════════════════════ */

/* Minimum pointer displacement (pixels, Euclidean) before get_event() reports
 * a "move".  Filters GT911 sub-pixel jitter while a finger is held still so a
 * stationary contact does not spam move events every INT.
 * Used by: lua/modules/lua_driver_touch/src/lua_driver_touch.c */
#define CLAW_TOUCH_MOVE_THRESHOLD        3

/* Release watchdog (ms).  GT911 normally emits a final INT with touch-count 0
 * on finger lift, which yields the "up" event; but that trailing INT is
 * config-dependent.  If a contact is active and no new INT has arrived for this
 * long, get_event() synthesises an "up" so a dropped release-INT cannot wedge
 * the FSM in the pressed state forever.  ~60 ms >> GT911 scan period (~5-10 ms).
 * Used by: lua/modules/lua_driver_touch/src/lua_driver_touch.c */
#define CLAW_TOUCH_RELEASE_TIMEOUT_MS    60

/* I2C IC_FILTER value written to work around RGB-LCD noise coupled onto the
 * shared I2C lines (st7701p dclk/data toggling).  The i2c_api does not expose
 * the filter field, so the driver writes the I2C controller register directly
 * (mirrors SDK input_touch_gt911.c PATCH_FOR_LCD_NOISE).  Low 9 bits only.
 * Used by: lua/modules/lua_driver_touch/src/lua_driver_touch.c */
#define CLAW_TOUCH_I2C_FILTER            0x108u

/* Reader task: a small FreeRTOS task woken by the INT ISR that performs the I2C
 * point reads (I2C is illegal in ISR) and pushes raw down/move/up events into a
 * queue.  This decouples event CAPTURE from the Lua poll rate, so a fast tap
 * that starts and ends between two get_event() calls is never lost (mirrors the
 * SDK gt911_work task + the button subsystem's ISR→bottom-half→queue shape).
 * Stack: I2C reads only, NO TLS/heavy call chain, so a small stack suffices.
 * Prio: above the game lua_job (prio 1) so INT-driven reads preempt the frame
 * loop; runs in brief bursts only.  Used by: lua_driver_touch.c */
#define CLAW_TOUCH_READER_TASK_STACK     4096
#define CLAW_TOUCH_READER_TASK_PRIO      3

/* Depth of the raw-event queue drained by get_event().  down/up are never
 * dropped; redundant moves are coalesced (only enqueued once travel exceeds
 * CLAW_TOUCH_MOVE_THRESHOLD) and may be dropped if the queue backs up.
 * Used by: lua_driver_touch.c */
#define CLAW_TOUCH_EVENT_QUEUE_DEPTH     64

/* ═══════════════════════════════════════════════════════════════════
 *  lvgl widget layer (lua_module_lvgl, phase5_lvgl_full.md)
 * ═══════════════════════════════════════════════════════════════════ */

/* Stack size for lvgl_timer_task (drives lv_timer_handler()).  Rendering
 * itself still runs on the shared swdraw thread (display_lua.c); this task
 * only dispatches — start conservative, retune from uxTaskGetStackHighWaterMark.
 * Used by: lua/modules/lua_module_lvgl/src/lvgl_lua.c */
#define CLAW_LVGL_TIMER_TASK_STACK       8192

/* lvgl_timer_task priority.  Deliberately modest (same band as agent/wifi_mgr):
 * widget UI does not need to preempt like the `display` game-loop path.
 * Used by: lua/modules/lua_module_lvgl/src/lvgl_lua.c */
#define CLAW_LVGL_TIMER_TASK_PRIO        (2)

/* Max Lua event callbacks ('.on()'/':on_click()'/...) a single widget may
 * carry at once.  Bounds the per-widget registry-ref + handle array so a
 * runaway skill cannot leak by re-subscribing without ever calling :off().
 * Used by: lua/modules/lua_module_lvgl/src/lvgl_lua.c */
#define CLAW_LVGL_EVENT_CB_MAX           4

/* lv_timer_handler() idle period (ms) used when it reports LV_NO_TIMER_READY.
 * Matches the SDK's LV_DEF_REFR_PERIOD (~30fps).
 * Used by: lua/modules/lua_module_lvgl/src/lvgl_lua.c */
#define CLAW_LVGL_TIMER_DEF_PERIOD_MS    33

/* Depth of the lvgl_timer_task -> script-thread event queue (see phase5 §7a.4).
 * Full queue drops the OLDEST entry (FIFO) rather than blocking the LVGL
 * dispatch thread.  Used by: lua/modules/lua_module_lvgl/src/lvgl_lua.c */
#define CLAW_LVGL_EVENT_QUEUE_MAX        16

/* disp_own_release()'s LVGL branch: how long to block waiting for
 * lvgl_timer_task to acknowledge teardown before giving up (it still
 * continues releasing the touch/backend resources afterward; a timeout only
 * means the wait itself did not confirm — see phase5 §5).
 * Used by: lua/modules/lua_module_display/src/display_lua.c */
#define CLAW_LVGL_TEARDOWN_JOIN_TIMEOUT_MS  1000

/*
 * Compaction trigger (compact_tokens) and hard context ceiling (window_tokens)
 * are WebUI-adjustable and have their compile-time defaults in:
 *   claw_modules/claw_config/include/claw_config.h
 *     CLAW_CONFIG_DEFAULT_LLM_COMPACT_TOKENS  110000
 *     CLAW_CONFIG_DEFAULT_LLM_WINDOW_TOKENS   128000
 * The compaction worker task is always started (unconditionally).
 */

#endif /* AMEBA_CLAW_DEFS_H */

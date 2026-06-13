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
 * HTTP server
 * These are compile-time only — not adjustable via WebUI.
 * ═══════════════════════════════════════════════════════════════════ */

/* TCP port the embedded HTTP server listens on.
 * Used by: ameba_claw_main.c → claw_http_server_config_t.port */
#define CLAW_HTTP_PORT                  8080

/* Maximum POST body size accepted. Requests exceeding this return 413.
 * Used by: claw_http_server.c → HTTP_MAX_BODY_SIZE */
#define CLAW_HTTP_MAX_BODY_SIZE         (32 * 1024)


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

/* Tunable: socket recv timeout for non-streaming LLM calls (both TLS and plain HTTP).
 * In non-streaming mode the server buffers the entire generation before sending
 * the first byte, so this must cover the full generation time. For a 16K-token
 * output at ~30 tok/s that is ~9 minutes — 300 s gives comfortable headroom.
 * Also applies as the inter-chunk idle timeout in streaming mode (rarely trips
 * because tokens arrive continuously). */
#define CLAW_AGENT_LLM_RECV_TIMEOUT_MS  300000u  /* 300 seconds */


/* Enable lua script time check */
// #define CLAW_LUA_TIME_LOG_ENABLE

/*
 * Compaction trigger (compact_tokens) and hard context ceiling (window_tokens)
 * are WebUI-adjustable and have their compile-time defaults in:
 *   claw_modules/claw_config/include/claw_config.h
 *     CLAW_CONFIG_DEFAULT_LLM_COMPACT_TOKENS  110000
 *     CLAW_CONFIG_DEFAULT_LLM_WINDOW_TOKENS   128000
 * The compaction worker task is always started (unconditionally).
 */

#endif /* AMEBA_CLAW_DEFS_H */

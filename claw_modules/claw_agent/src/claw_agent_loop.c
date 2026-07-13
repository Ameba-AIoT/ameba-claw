/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The agentic tool-call loop.
 *
 * claw_agent_process_request() is the heart of the engine. It is deliberately
 * kept as a thin orchestrator so the control flow reads top-to-bottom; each
 * substantial block is a named helper:
 *
 *   agent_invoke_llm()        — one LLM round: assemble context → call → TTFB guard
 *   agent_handle_tool_round() — materialize + execute the tool round-trips
 *   finalize_success_turn()   — persist the completed turn + fire observers
 *   finalize_failed_turn()    — persist partial tool work + shape the error reply
 *
 * Context assembly and tool-round materialization live in claw_agent_context.c;
 * the engine shell / lifecycle / queues live in claw_agent.c. This file is the
 * natural home for future control-flow features such as mid-request abort.
 */

#include "ameba_soc.h"
#include "claw_agent_internal.h"
#include "claw_config.h"
#include "ameba_claw_defs.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "claw_agent";

/* ---- One LLM round ----
 * Assemble the context, call the model, and apply the streaming TTFB guard.
 * On success *out_llm is filled (the caller owns it and must free it with
 * claw_agent_llm_response_free, including on the error paths — it is safe to
 * free a zeroed/already-freed llm_resp_t). On failure resp->pub.error_message
 * is set; the TTFB-timeout path additionally marks resp->pub.status ERROR. */
static int agent_invoke_llm(const rtk_req_node_t *rn, const cJSON *rt_msgs,
                            rtk_resp_node_t *resp,
                            char *prov_tags, size_t prov_tags_sz,
                            llm_resp_t *out_llm)
{
    llm_ctx_t ctx = {0};
    int rc;

    rc = claw_agent_assemble_context(rn, rt_msgs, &ctx, prov_tags, prov_tags_sz);
    if (rc != RTK_SUCCESS) {
        free(resp->pub.error_message);
        resp->pub.error_message = claw_agent_str_clone(rtk_err_to_name(rc));
        return rc;
    }

    rc = claw_agent_llm_chat_messages(ctx.sys_prompt, ctx.messages,
                                      ctx.tools_json, out_llm,
                                      &resp->pub.error_message);
    claw_agent_llm_ctx_free(&ctx);
    if (rc != RTK_SUCCESS) {
        return rc;
    }

    /* TTFB guard (streaming mode only): in streaming the first token arrives
     * within seconds; a long TTFB means the server is unresponsive.
     * Skipped in non-streaming mode because TTFB ≈ full generation time there. */
    if (claw_config_get()->llm.stream_enabled &&
            out_llm->ttfb_ms > CLAW_AGENT_LLM_TTFB_TIMEOUT_MS) {
        RTK_LOGW(TAG, "req=%" PRIu32 " TTFB %u ms exceeded limit\n",
                 rn->pub.request_id, (unsigned)out_llm->ttfb_ms);
        free(resp->pub.error_message);
        resp->pub.error_message = claw_agent_str_clone("LLM response timeout (TTFB)");
        resp->pub.status = CLAW_AGENT_RESPONSE_STATUS_ERROR;
        return RTK_FAIL;
    }
    return RTK_SUCCESS;
}

/* ---- Tool round ----
 * Record the requested tool names for observer reporting, forward any narration
 * text to the user, then serialize the assistant tool-call message and execute
 * every tool, appending the results to rt_msgs. Returns the round's rc. */
static int agent_handle_tool_round(const rtk_req_node_t *rn, cJSON *rt_msgs,
                                   const llm_resp_t *llm,
                                   char *tool_tags, size_t tool_tags_sz,
                                   char *tlog)
{
    int rc;

    /* Record tool names for observer reporting */
    for (size_t tc = 0; tc < llm->call_cnt; tc++) {
        claw_agent_tagbuf_add(tool_tags, tool_tags_sz, llm->calls[tc].fn_name, false);
    }

    /* Forward narration text (LLM's plan/thinking before tool calls) to the
     * user in real time. tool_name=NULL distinguishes narration from tool-call
     * progress in on_tool_progress. Does not consume the tool progress budget. */
    if (llm->reply && llm->reply[0] && g_engine->on_tool_progress) {
        g_engine->on_tool_progress(rn->pub.request_id, NULL, llm->reply,
                                   rn->pub.source_channel, rn->pub.source_chat_id,
                                   rn->pub.source_message_id,
                                   g_engine->on_tool_progress_ctx);
    }

    rc = claw_agent_build_tool_call_round(rt_msgs, llm);
    if (rc == RTK_SUCCESS) {
        rc = claw_agent_build_tool_result_round(rt_msgs, llm, &rn->pub, tlog, TOOL_LOG_BUFSIZE);
    }
    return rc;
}

/* ---- Finalize: completed turn ----
 * Mark OK, persist the turn (with its verbatim tool round-trips), and fire the
 * completion observers. */
static void finalize_success_turn(const rtk_req_node_t *rn, rtk_resp_node_t *resp,
                                  const char *tool_msgs_json, uint32_t final_prompt_tokens,
                                  const char *prov_tags, const char *tool_tags,
                                  bool tools_ran)
{
    resp->pub.status = CLAW_AGENT_RESPONSE_STATUS_OK;

    /* Save when there is a reply to record, OR when tools ran (a checkpoint
     * turn already exists on disk and must be finalized with completed:true). */
    if ((resp->pub.text[0] || tools_ran) && g_engine->save_turn &&
            rn->pub.session_id && rn->pub.session_id[0]) {
        int ae = g_engine->save_turn(rn->pub.session_id,
                                      rn->pub.user_text,
                                      resp->pub.text,
                                      tool_msgs_json,
                                      (int)claw_config_get()->llm.backend,
                                      final_prompt_tokens,
                                      rn->pub.request_id,
                                      g_engine->save_turn_ctx);
        if (ae != RTK_SUCCESS) {
            RTK_LOGW(TAG, "save_turn failed: %s\n", rtk_err_to_name(ae));
        }
    }

    if (g_engine->observer_cnt > 0) {
        claw_agent_completion_summary_t s = {
            .request_id            = rn->pub.request_id,
            .session_id            = rn->pub.session_id,
            .user_text             = rn->pub.user_text,
            .final_text            = resp->pub.text,
            .context_providers_csv = prov_tags,
            .tool_calls_csv        = tool_tags,
        };
        for (size_t oi = 0; oi < g_engine->observer_cnt; oi++) {
            g_engine->observers[oi].fn(&s, g_engine->observers[oi].ctx);
        }
    }
}

/* ---- Finalize: failed turn ----
 * Log the failure, persist any completed tool work so the next turn can resume
 * it, ensure an error message exists, and surface partially-run tools in it. */
static void finalize_failed_turn(const rtk_req_node_t *rn, rtk_resp_node_t *resp,
                                 const char *tool_msgs_json, const char *tool_tags, int rc,
                                 bool tools_ran)
{
    RTK_LOGE(TAG, "req=%" PRIu32 " failed: %s\n",
             rn->pub.request_id,
             resp->pub.error_message ?
             resp->pub.error_message : rtk_err_to_name(rc));

    /* Save completed tool round-trips to session history whenever the turn
     * had ALREADY executed one or more tool calls before failing. Those tool
     * calls carry real side-effects (file writes, job runs, memory_store …)
     * that persisted regardless of the final error, so dropping the context
     * makes the NEXT turn lose the entire in-progress task and "forget" work
     * the user can plainly see took effect.
     *
     * This now covers two mid-task failure modes uniformly:
     *   1. Preempted by a new message (abort_flag): next request replays the
     *      identical tool prefix and cache-hits.
     *   2. LLM / transport error mid-loop (e.g. HTTP 429 rate-limit, TLS or
     *      socket drop) AFTER tools ran. This case was previously dropped —
     *      that was the bug where an interrupted multi-step task vanished
     *      from history, leaving the model amnesiac on the next turn.
     *
     * Gate on tools_ran: if at least one tool call executed, a checkpoint turn
     * was written to VFS and must be finalized (completed:true) regardless of
     * whether the final tool_msgs serialization succeeded.  Pure LLM errors
     * with no tool work are still not saved (would poison the next turn). */
    if (tools_ran &&
            rn->pub.session_id && rn->pub.session_id[0] && g_engine->save_turn) {
        const char *interrupt_note =
            g_engine->abort_flag ? "(task was interrupted by a new message)"
                                 : "(request failed before completing; partial work above was already done)";
        g_engine->save_turn(rn->pub.session_id,
                            rn->pub.user_text,
                            interrupt_note,
                            tool_msgs_json,
                            (int)claw_config_get()->llm.backend,
                            0,
                            rn->pub.request_id,
                            g_engine->save_turn_ctx);
    }

    /* Failed requests with NO completed tool work are not saved to session
     * history — recording a bare error note as assistant text causes the LLM
     * to fixate on the failure in subsequent turns instead of answering the
     * new question. (Mid-task failures with tool side-effects are saved
     * above so the next turn can resume / report on the partial work.) */

    if (!resp->pub.error_message) {
        resp->pub.error_message = claw_agent_str_clone(rtk_err_to_name(rc));
    }

    /* When the failure happened mid-iteration AFTER one or more tool calls
     * already executed, those side effects (memory_store, file writes …)
     * persisted but the user only sees ERROR. Surface the partial-success
     * tools in the error message so the next turn can reference them. */
    if (tool_tags && tool_tags[0] && resp->pub.error_message) {
        char *old = resp->pub.error_message;
        size_t need = strlen(old) + strlen(tool_tags) + 32;
        char *combined = (char *)malloc(need);
        if (combined) {
            snprintf(combined, need, "%s (partial: ran [%s])", old, tool_tags);
            free(old);
            resp->pub.error_message = combined;
        }
    }
}

/* ---- Per-request driver ---- */

void claw_agent_process_request(rtk_req_node_t *rn, rtk_resp_node_t *resp,
                                char *prov_tags, size_t prov_tags_sz,
                                char *tool_tags, size_t tool_tags_sz)
{
    /* Use struct-resident tlog instead of stack local (~768B saved) */
    char     *tlog = g_engine->tlog;
    cJSON    *rt_msgs;
    uint32_t  iter;
    uint32_t  final_prompt_tokens = 0;   /* real context size of the last request */
    int       rc;

    _memset(tlog, 0, TOOL_LOG_BUFSIZE);
    rt_msgs = cJSON_CreateArray();
    if (!rt_msgs) {
        resp->pub.error_message = claw_agent_str_clone("alloc rt_msgs failed");
        return;
    }

    rc = RTK_SUCCESS;
    for (iter = 0; ; iter++) {
        llm_resp_t llm = {0};

        if (g_engine->abort_flag) {
            free(resp->pub.error_message);
            resp->pub.error_message = claw_agent_str_clone("request cancelled");
            resp->pub.status = CLAW_AGENT_RESPONSE_STATUS_ERROR;
            rc = RTK_FAIL;
            break;
        }

        rc = agent_invoke_llm(rn, rt_msgs, resp, prov_tags, prov_tags_sz, &llm);
        if (rc != RTK_SUCCESS) {
            claw_agent_llm_response_free(&llm);
            break;
        }

        if (llm.call_cnt == 0) {
            const char *txt = llm.reply ? llm.reply : "";

            RTK_LOGI(TAG, "== FINAL iter=%lu  reply=%.160s\n", (unsigned long)iter, txt);
            RTK_LOGI(TAG, "done req=%" PRIu32 " text_len=%zu prompt_tokens=%u\n",
                     rn->pub.request_id, strlen(txt), (unsigned)llm.prompt_tokens);
            free(resp->pub.text);
            free(resp->pub.error_message);
            resp->pub.text          = claw_agent_str_clone(txt);
            resp->pub.error_message = NULL;
            /* Real context size of this (final) request — drives token-budget
             * compaction. 0 if the endpoint did not report usage. */
            final_prompt_tokens     = llm.prompt_tokens;
            claw_agent_llm_response_free(&llm);
            rc = RTK_SUCCESS;
            break;
        }

        RTK_LOGI(TAG, "-- iter=%lu  req=%" PRIu32 "  LLM wants %u tool call(s)\n",
                 (unsigned long)iter, rn->pub.request_id, (unsigned)llm.call_cnt);

        rc = agent_handle_tool_round(rn, rt_msgs, &llm, tool_tags, tool_tags_sz, tlog);
        claw_agent_llm_response_free(&llm);
        if (rc != RTK_SUCCESS) {
            free(resp->pub.error_message);
            resp->pub.error_message = claw_agent_str_clone(rtk_err_to_name(rc));
            break;
        }

        /* Checkpoint: persist accumulated tool rounds after each iteration so
         * a power-loss mid-loop leaves the session with completed:false rather
         * than losing the tool history entirely.  assistant_text=NULL signals
         * "in progress"; finalize_*_turn upserts with the real reply. */
        if (g_engine->save_turn && rn->pub.session_id && rn->pub.session_id[0]) {
            char *snap_json = cJSON_GetArraySize(rt_msgs) > 0
                              ? cJSON_PrintUnformatted(rt_msgs) : NULL;
            g_engine->save_turn(rn->pub.session_id,
                                rn->pub.user_text,
                                NULL,
                                snap_json,
                                (int)claw_config_get()->llm.backend,
                                0,
                                rn->pub.request_id,
                                g_engine->save_turn_ctx);
            free(snap_json);
        }

        {
            const claw_config_t *ccfg = claw_config_get();
            uint32_t iter_limit = (ccfg->llm.max_iterations > 0)
                                  ? (uint32_t)ccfg->llm.max_iterations : CLAW_CONFIG_DEFAULT_LLM_MAX_ITER;
            if (iter_limit < CLAW_AGENT_TOOL_ITER_MIN) iter_limit = CLAW_AGENT_TOOL_ITER_MIN;
            if (iter + 1 >= iter_limit) {
                free(resp->pub.error_message);
                resp->pub.error_message = claw_agent_str_clone("max tool call rounds exceeded");
                rc = RTK_FAIL;
                break;
            }
        }
    }

    /* Serialize this turn's tool round-trips (assistant tool_calls + tool
     * results, already in the active backend's wire format) BEFORE freeing
     * rt_msgs, so the session layer can persist them for verbatim cross-turn
     * replay. NULL when no tool call happened this turn. The final assistant
     * reply is NOT in rt_msgs — it is stored separately as the turn's
     * `assistant` text and replayed after the tool history. */
    bool tools_ran = cJSON_GetArraySize(rt_msgs) > 0;
    char *tool_msgs_json = tools_ran ? cJSON_PrintUnformatted(rt_msgs) : NULL;
    cJSON_Delete(rt_msgs);

    if (tlog[0])
        resp->pub.tool_trace = claw_agent_str_clone(tlog);

    if (rc == RTK_SUCCESS && resp->pub.text) {
        finalize_success_turn(rn, resp, tool_msgs_json, final_prompt_tokens,
                              prov_tags, tool_tags, tools_ran);
    } else if (rc != RTK_SUCCESS) {
        finalize_failed_turn(rn, resp, tool_msgs_json, tool_tags, rc, tools_ran);
    }

    free(tool_msgs_json);
}

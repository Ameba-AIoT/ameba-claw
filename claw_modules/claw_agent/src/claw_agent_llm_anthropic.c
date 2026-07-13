/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Anthropic (Claude) chat backend.
 *
 * Owns everything specific to the Anthropic wire format: the OpenAI→Anthropic
 * tools conversion, the three prompt-cache anchors (tools / system / rolling
 * last-message breakpoint), and the request/response exchange in
 * claw_agent_llm_chat_anthropic(). Always non-streaming — the SSE accumulator
 * in the OpenAI TU understands only OpenAI delta chunks. Shares the frontend's
 * config resolution and URL parsing via claw_agent_llm_internal.h.
 */

#include "ameba_soc.h"
#include "claw_agent_llm_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "llm_agent_http.h"
#include "claw_config.h"
#include "os_wrapper.h"
#include "ameba_claw_defs.h"

#define TAG "claw_agent_llm"

/*
 * Convert OpenAI tools array to Anthropic format.
 * OpenAI: [{"type":"function","function":{"name":"...","description":"...","parameters":{...}}}]
 * Anthropic: [{"name":"...","description":"...","input_schema":{...}}]
 */
static cJSON *convert_tools_to_anthropic(const char *tools_json)
{
    cJSON *openai_tools = cJSON_Parse(tools_json);
    if (!openai_tools || !cJSON_IsArray(openai_tools)) {
        cJSON_Delete(openai_tools);
        return NULL;
    }

    cJSON *anthropic_tools = cJSON_CreateArray();
    cJSON *item;
    cJSON_ArrayForEach(item, openai_tools) {
        cJSON *func = cJSON_GetObjectItem(item, "function");
        if (!func) func = item; /* already unwrapped */
        cJSON *name = cJSON_GetObjectItem(func, "name");
        cJSON *desc = cJSON_GetObjectItem(func, "description");
        cJSON *params = cJSON_GetObjectItem(func, "parameters");
        if (!params) params = cJSON_GetObjectItem(func, "input_schema");

        cJSON *t = cJSON_CreateObject();
        if (name && cJSON_IsString(name))
            cJSON_AddStringToObject(t, "name", name->valuestring);
        if (desc && cJSON_IsString(desc))
            cJSON_AddStringToObject(t, "description", desc->valuestring);
        if (params) {
            cJSON *schema = cJSON_Duplicate(params, true);
            cJSON_AddItemToObject(t, "input_schema", schema);
        } else {
            cJSON_AddItemToObject(t, "input_schema", cJSON_CreateObject());
        }
        cJSON_AddItemToArray(anthropic_tools, t);
    }
    cJSON_Delete(openai_tools);

    /* Anthropic prompt-cache anchor #1: mark the LAST tool with cache_control
     * so the whole tool-definition block (the largest, whole-session-stable
     * prefix segment — see caching order tools→system→messages) is cached.
     * Without this, Anthropic caches nothing and every tool iteration re-pays
     * the full prompt; GLM/OpenAI get this automatically via prefix caching. */
    {
        int n = cJSON_GetArraySize(anthropic_tools);
        cJSON *last = (n > 0) ? cJSON_GetArrayItem(anthropic_tools, n - 1) : NULL;
        if (last) {
            cJSON *cc = cJSON_CreateObject();
            if (cc) {
                cJSON_AddStringToObject(cc, "type", "ephemeral");
                cJSON_AddItemToObject(last, "cache_control", cc);
            }
        }
    }
    return anthropic_tools;
}

/* Anthropic prompt-cache rolling breakpoint: attach cache_control to the last
 * content block of the final message. As the conversation grows (tool
 * iterations within a request, and new turns across requests), this breakpoint
 * advances; Anthropic matches the longest previously-cached prefix, so the
 * verbatim-replayed tool history is read from cache instead of re-billed.
 * cache_control is positional metadata, not hashed content, so prior turns
 * replayed without the marker still match the cached token prefix. */
static void anthropic_mark_last_cacheable(cJSON *msgs)
{
    int n = msgs ? cJSON_GetArraySize(msgs) : 0;
    cJSON *last = (n > 0) ? cJSON_GetArrayItem(msgs, n - 1) : NULL;
    if (!last) return;

    cJSON *content = cJSON_GetObjectItem(last, "content");
    cJSON *cc = cJSON_CreateObject();
    if (!cc) return;
    cJSON_AddStringToObject(cc, "type", "ephemeral");

    if (content && cJSON_IsArray(content) && cJSON_GetArraySize(content) > 0) {
        cJSON *blk = cJSON_GetArrayItem(content, cJSON_GetArraySize(content) - 1);
        if (blk) { cJSON_AddItemToObject(blk, "cache_control", cc); return; }
        cJSON_Delete(cc);
    } else if (content && cJSON_IsString(content)) {
        /* Promote a plain string body to a single text block so the marker has
         * somewhere to live: content:[{type:text,text:<s>,cache_control:...}] */
        cJSON *blk = cJSON_CreateObject();
        cJSON *arr = cJSON_CreateArray();
        if (blk && arr) {
            cJSON_AddStringToObject(blk, "type", "text");
            cJSON_AddStringToObject(blk, "text", content->valuestring);
            cJSON_AddItemToObject(blk, "cache_control", cc);
            cJSON_AddItemToArray(arr, blk);
            cJSON_ReplaceItemInObject(last, "content", arr);
        } else {
            cJSON_Delete(cc); cJSON_Delete(blk); cJSON_Delete(arr);
        }
    } else {
        cJSON_Delete(cc);
    }
}

/*
 * Convert an Anthropic-format messages array to Anthropic API format.
 * Incoming messages may contain tool_use/tool_result role entries from a previous
 * tool-call round-trip (which claw_agent stores in OpenAI format).
 * For simplicity we pass through user/assistant messages as-is.
 */
static cJSON *build_anthropic_messages(cJSON *messages)
{
    cJSON *out = cJSON_CreateArray();
    cJSON *item;
    cJSON_ArrayForEach(item, messages) {
        cJSON *dup = cJSON_Duplicate(item, true);
        cJSON_AddItemToArray(out, dup);
    }
    return out;
}

int claw_agent_llm_chat_anthropic(const char *system_prompt,
                                  cJSON *messages,
                                  const char *tools_json,
                                  llm_resp_t *out_response,
                                  char **out_error_message)
{
    const claw_config_t *cfg = claw_config_get();
    const char *api_key = resolve_api_key(cfg);
    const char *model   = resolve_model(cfg, "claude-3-5-sonnet-20241022");
    uint32_t max_tokens = cfg->llm.max_tokens ? (uint32_t)cfg->llm.max_tokens
                                              : CLAW_AGENT_LLM_DEFAULT_TOKENS;
    char host[128], api_path[256];
    llm_parse_url(cfg->llm.api_url, CLAW_LLM_BACKEND_ANTHROPIC,
                  host, sizeof(host), api_path, sizeof(api_path));

    cJSON *req = NULL, *msgs = NULL, *tools_arr = NULL;
    char *req_json = NULL;
    llm_http_resp_t http_resp;
    cJSON *resp_root = NULL;
    int err = RTK_FAIL;
    char parse_preview[257] = {0};

    req = cJSON_CreateObject();
    if (!req) { *out_error_message = dup_printf("OOM req"); return RTK_ERR_NOMEM; }

    cJSON_AddStringToObject(req, "model", model);
    cJSON_AddNumberToObject(req, "max_tokens", (double)max_tokens);
    /* Always non-streaming for Anthropic: the SSE accumulator in the HTTP layer
     * only understands OpenAI delta chunks, not Anthropic's content_block_delta
     * SSE format. cfg->llm.stream_enabled deliberately does NOT apply here. */
    cJSON_AddFalseToObject(req, "stream");

    if (system_prompt && system_prompt[0]) {
        /* Anthropic prompt-cache anchor #2: system as a one-block array so we
         * can attach cache_control, caching the tools+system prefix (system is
         * stable across a session). Fall back to a plain string on OOM. */
        cJSON *sys_arr = cJSON_CreateArray();
        cJSON *sys_blk = cJSON_CreateObject();
        cJSON *sys_cc  = cJSON_CreateObject();
        if (sys_arr && sys_blk && sys_cc) {
            cJSON_AddStringToObject(sys_blk, "type", "text");
            cJSON_AddStringToObject(sys_blk, "text", system_prompt);
            cJSON_AddStringToObject(sys_cc, "type", "ephemeral");
            cJSON_AddItemToObject(sys_blk, "cache_control", sys_cc);
            cJSON_AddItemToArray(sys_arr, sys_blk);
            cJSON_AddItemToObject(req, "system", sys_arr);
        } else {
            cJSON_Delete(sys_arr); cJSON_Delete(sys_blk); cJSON_Delete(sys_cc);
            cJSON_AddStringToObject(req, "system", system_prompt);
        }
    }

    msgs = build_anthropic_messages(messages);
    anthropic_mark_last_cacheable(msgs);   /* cache anchor #3 (rolling) */
    cJSON_AddItemToObject(req, "messages", msgs);
    msgs = NULL;

    if (tools_json && tools_json[0]) {
        tools_arr = convert_tools_to_anthropic(tools_json);
        if (tools_arr && cJSON_GetArraySize(tools_arr) > 0) {
            cJSON_AddItemToObject(req, "tools", tools_arr);
            tools_arr = NULL;
        } else {
            cJSON_Delete(tools_arr);
            tools_arr = NULL;
        }
    }

    req_json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req); req = NULL;
    if (!req_json) { *out_error_message = dup_printf("OOM serial"); err = RTK_ERR_NOMEM; goto cleanup; }

    RTK_LOGI(TAG, "POST Anthropic %s (%u bytes)\n", api_path, (unsigned)strlen(req_json));

    if (llm_http_resp_init(&http_resp) != 0) {
        *out_error_message = dup_printf("OOM http resp"); err = RTK_ERR_NOMEM; goto cleanup;
    }

    {
        int http_ret = llm_http_post(host, api_path, req_json, strlen(req_json), api_key, &http_resp);
        free(req_json); req_json = NULL;
        if (http_ret != 0) {
            *out_error_message = dup_printf("HTTP POST failed (%d)", http_ret);
            llm_http_resp_free(&http_resp);
            err = RTK_FAIL; goto cleanup;
        }
    }

    if (sse_reassemble(&http_resp) != 0) {
        *out_error_message = dup_printf("OOM sse");
        llm_http_resp_free(&http_resp);
        err = RTK_ERR_NOMEM; goto cleanup;
    }

    /* Capture preview before freeing — used for logging on parse failure */
    {
        size_t plen2 = http_resp.len < 256 ? http_resp.len : 256;
        _memcpy(parse_preview, http_resp.buf, plen2);
        parse_preview[plen2] = '\0';
    }
    RTK_LOGD(TAG, "response %u bytes\n", (unsigned)http_resp.len);
    out_response->ttfb_ms = http_resp.ttfb_ms;
    resp_root = cJSON_Parse(http_resp.buf);
    llm_http_resp_free(&http_resp);   /* buf freed here */

    if (!resp_root) {
        RTK_LOGE(TAG, "JSON parse failed, raw (first 256): %s\n", parse_preview);
        *out_error_message = dup_printf("JSON parse failed");
        err = RTK_FAIL;
        goto cleanup;
    }

    /* Check Anthropic error format: {"type":"error","error":{"type":"...","message":"..."}} */
    {
        cJSON *type_item = cJSON_GetObjectItem(resp_root, "type");
        if (type_item && cJSON_IsString(type_item) && strcmp(type_item->valuestring, "error") == 0) {
            cJSON *error = cJSON_GetObjectItem(resp_root, "error");
            cJSON *msg   = error ? cJSON_GetObjectItem(error, "message") : NULL;
            *out_error_message = dup_printf("Anthropic API error: %s",
                                            (msg && cJSON_IsString(msg)) ? msg->valuestring : "unknown");
            err = RTK_FAIL; goto cleanup;
        }
    }

    /* Parse Anthropic response: {"content":[{"type":"text","text":"..."},{"type":"tool_use",...}]} */
    {
        cJSON *content_arr = cJSON_GetObjectItem(resp_root, "content");
        if (!content_arr || !cJSON_IsArray(content_arr)) {
            /* Dump first 256 bytes as hex to see what the proxy returned */
            char hexbuf[129] = {0};
            for (int _i = 0; _i < 64 && parse_preview[_i]; _i++)
                DiagSnPrintf(hexbuf + _i*2, 3, "%02x", (unsigned char)parse_preview[_i]);
            RTK_LOGE(TAG, "No content (arr=%s). hex: %s txt: %.64s\n",
                     content_arr ? "not-array" : "null", hexbuf, parse_preview);
            *out_error_message = dup_printf("No content in Anthropic response");
            err = RTK_FAIL; goto cleanup;
        }
        /* Empty content array = model finished after tool use with no text reply — treat as OK */
        if (cJSON_GetArraySize(content_arr) == 0) {
            out_response->reply = dup_printf("");
            err = RTK_SUCCESS; goto cleanup;
        }

        int tool_count = 0;
        cJSON *block;
        cJSON_ArrayForEach(block, content_arr) {
            cJSON *btype = cJSON_GetObjectItem(block, "type");
            if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "tool_use") == 0) {
                tool_count++;
            }
        }

        if (tool_count > 0) {
            if (tool_count > CLAW_AGENT_LLM_MAX_TOOL_CALLS) tool_count = CLAW_AGENT_LLM_MAX_TOOL_CALLS;
            out_response->calls = calloc((size_t)tool_count, sizeof(llm_call_t));
            if (!out_response->calls) {
                *out_error_message = dup_printf("OOM tool_calls"); err = RTK_ERR_NOMEM; goto cleanup;
            }
        }

        int tc_idx = 0;
        cJSON_ArrayForEach(block, content_arr) {
            cJSON *btype = cJSON_GetObjectItem(block, "type");
            if (!btype || !cJSON_IsString(btype)) continue;

            if (strcmp(btype->valuestring, "text") == 0) {
                cJSON *text_item = cJSON_GetObjectItem(block, "text");
                if (text_item && cJSON_IsString(text_item) && text_item->valuestring[0]) {
                    free(out_response->reply);
                    out_response->reply = strdup(text_item->valuestring);
                }
            } else if (strcmp(btype->valuestring, "tool_use") == 0 && tc_idx < tool_count) {
                cJSON *id_item   = cJSON_GetObjectItem(block, "id");
                cJSON *name_item = cJSON_GetObjectItem(block, "name");
                cJSON *input_obj = cJSON_GetObjectItem(block, "input");

                out_response->calls[tc_idx].call_id =
                    (id_item && cJSON_IsString(id_item)) ? strdup(id_item->valuestring) : strdup("");
                out_response->calls[tc_idx].fn_name =
                    (name_item && cJSON_IsString(name_item)) ? strdup(name_item->valuestring) : strdup("");
                /* Convert input object → JSON string for compatibility with claw_agent */
                if (input_obj) {
                    char *input_str = cJSON_PrintUnformatted(input_obj);
                    out_response->calls[tc_idx].args_json = input_str ? input_str : strdup("{}");
                } else {
                    out_response->calls[tc_idx].args_json = strdup("{}");
                }
                tc_idx++;
            }
        }
        out_response->call_cnt = (size_t)tc_idx;
    }

    /* Anthropic usage: input_tokens (uncached) + cache_read + cache_creation =
     * real context size. May be absent on some proxy endpoints → caller falls
     * back to char estimate. */
    {
        cJSON *usage = cJSON_GetObjectItem(resp_root, "usage");
        if (usage) {
            cJSON *it = cJSON_GetObjectItem(usage, "input_tokens");
            cJSON *ot = cJSON_GetObjectItem(usage, "output_tokens");
            cJSON *cr = cJSON_GetObjectItem(usage, "cache_read_input_tokens");
            cJSON *cc = cJSON_GetObjectItem(usage, "cache_creation_input_tokens");
            uint32_t in = (it && cJSON_IsNumber(it)) ? (uint32_t)it->valuedouble : 0;
            uint32_t rd = (cr && cJSON_IsNumber(cr)) ? (uint32_t)cr->valuedouble : 0;
            uint32_t wr = (cc && cJSON_IsNumber(cc)) ? (uint32_t)cc->valuedouble : 0;
            out_response->prompt_tokens     = in + rd + wr;
            out_response->cached_tokens     = rd;
            if (ot && cJSON_IsNumber(ot)) out_response->completion_tokens = (uint32_t)ot->valuedouble;
        }
    }

    RTK_LOGD(TAG, "done text_len=%u tool_calls=%u prompt_tokens=%u cached=%u\n",
             (unsigned)(out_response->reply ? strlen(out_response->reply) : 0),
             (unsigned)out_response->call_cnt,
             (unsigned)out_response->prompt_tokens,
             (unsigned)out_response->cached_tokens);
    err = RTK_SUCCESS;

cleanup:
    cJSON_Delete(resp_root); cJSON_Delete(req); cJSON_Delete(msgs);
    cJSON_Delete(tools_arr); free(req_json);
    if (err != RTK_SUCCESS) claw_agent_llm_response_free(out_response);
    return err;
}

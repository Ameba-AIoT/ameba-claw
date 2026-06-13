/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "llm_agent.h"
#include "os_wrapper.h"
#include "memproc.h"
#include "diag.h"
#include "llm_agent_http.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LLM_API_HOST        "open.bigmodel.cn"
#define LLM_API_RESOURCE    "/api/anthropic/v1/messages"
#define LLM_DEFAULT_MODEL   "glm-5.1"
#define LLM_DEFAULT_TOKENS  1024

/* --- Ring buffer helpers --- */

/* Truncate a buffer to at most max_len bytes, respecting UTF-8 boundaries.
 * Returns the safe length (<= max_len) that does not cut a multi-byte char. */
static size_t utf8_safe_truncate(const char *src, size_t src_len, size_t max_len)
{
    if (src_len <= max_len) {
        return src_len;
    }
    /* Walk backwards from max_len to find a valid UTF-8 boundary */
    size_t pos = max_len;
    while (pos > 0) {
        unsigned char c = (unsigned char)src[pos];
        /* If byte is not a UTF-8 continuation byte (10xxxxxx), it's a boundary */
        if ((c & 0xC0) != 0x80) {
            /* Check that this is a valid leading byte for the remaining length */
            int expected;
            if (c < 0x80)       expected = 1;
            else if (c < 0xE0)  expected = 2;
            else if (c < 0xF0)  expected = 3;
            else                expected = 4;
            if (pos + expected <= max_len) {
                return pos + expected;
            }
            return pos;
        }
        pos--;
    }
    return 0;
}

static void history_add(llm_agent_t *agent, int role, const char *content)
{
    int idx;
    size_t content_len, safe_len;

    if (agent->history_count < LLM_MAX_HISTORY_MSGS) {
        idx = agent->history_count;
        agent->history_count++;
    } else {
        /* Overwrite oldest */
        idx = agent->history_head;
        agent->history_head = (agent->history_head + 1) % LLM_MAX_HISTORY_MSGS;
    }
    agent->history[idx].role = role;

    content_len = strlen(content);
    safe_len = utf8_safe_truncate(content, content_len, LLM_MAX_MESSAGE_LEN - 1);
    _memcpy(agent->history[idx].content, content, safe_len);
    agent->history[idx].content[safe_len] = '\0';
}

static cJSON *build_messages_array(llm_agent_t *agent, const char *new_msg)
{
    cJSON *arr = cJSON_CreateArray();
    int i;

    /* Add history entries in order (from oldest to newest) */
    for (i = 0; i < agent->history_count; i++) {
        int idx = (agent->history_head + i) % LLM_MAX_HISTORY_MSGS;
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role",
                                agent->history[idx].role == LLM_ROLE_USER
                                    ? "user" : "assistant");
        cJSON_AddStringToObject(msg, "content", agent->history[idx].content);
        cJSON_AddItemToArray(arr, msg);
    }

    /* Add the new user message */
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", new_msg);
    cJSON_AddItemToArray(arr, msg);

    return arr;
}

/* --- Tool helpers --- */

int llm_agent_register_tool(llm_agent_t *agent, const llm_tool_t *tool)
{
    if (!agent || !tool) {
        return -1;
    }
    if (agent->tool_count >= LLM_MAX_TOOLS) {
        DiagPrintf("[llm] tool register failed: max %d reached\n", LLM_MAX_TOOLS);
        return -2;
    }
    _memcpy(&agent->tools[agent->tool_count], tool, sizeof(llm_tool_t));
    agent->tool_count++;
    DiagPrintf("[llm] registered tool: %s\n", tool->name);
    return 0;
}

/* Build the "tools" JSON array from agent->tools[].
 * Returns a heap-allocated JSON string (caller must cJSON_free).
 * Returns NULL if no tools or on error. */
static char *build_tools_json(llm_agent_t *agent)
{
    cJSON *arr;
    char *json_str;
    int i;

    if (agent->tool_count == 0) {
        return NULL;
    }

    arr = cJSON_CreateArray();
    if (!arr) {
        return NULL;
    }

    for (i = 0; i < agent->tool_count; i++) {
        const llm_tool_t *t = &agent->tools[i];
        cJSON *tool_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_obj, "name", t->name);
        cJSON_AddStringToObject(tool_obj, "description", t->description);

        /* Build input_schema */
        cJSON *schema = cJSON_CreateObject();
        cJSON_AddStringToObject(schema, "type", "object");
        cJSON *props = cJSON_CreateObject();
        cJSON *required_arr = cJSON_CreateArray();
        int j;
        for (j = 0; j < t->param_count; j++) {
            const llm_tool_param_t *p = &t->params[j];
            cJSON *prop = cJSON_CreateObject();
            cJSON_AddStringToObject(prop, "type", p->type);
            cJSON_AddStringToObject(prop, "description", p->description);
            cJSON_AddItemToObject(props, p->name, prop);
            if (p->required) {
                cJSON_AddItemToArray(required_arr, cJSON_CreateString(p->name));
            }
        }
        cJSON_AddItemToObject(schema, "properties", props);
        if (cJSON_GetArraySize(required_arr) > 0) {
            cJSON_AddItemToObject(schema, "required", required_arr);
        } else {
            cJSON_Delete(required_arr);
        }
        cJSON_AddItemToObject(tool_obj, "input_schema", schema);

        cJSON_AddItemToArray(arr, tool_obj);
    }

    json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json_str;
}

/* Find a registered tool by name and execute it.
 * Returns a heap-allocated result string (caller must free), or NULL on error. */
static char *execute_tool(llm_agent_t *agent, const char *name, const char *input_json)
{
    int i;
    char *result;
    int rc;

    for (i = 0; i < agent->tool_count; i++) {
        if (strcmp(agent->tools[i].name, name) == 0) {
            break;
        }
    }
    if (i >= agent->tool_count) {
        DiagPrintf("[llm] tool not found: %s\n", name);
        result = (char *)rtos_mem_malloc(64);
        if (result) {
            DiagSnPrintf(result, 64, "Error: tool '%s' not found", name);
        }
        return result;
    }

    DiagPrintf("[llm] executing tool: %s (input: %s)\n", name, input_json);

    result = (char *)rtos_mem_malloc(LLM_TOOL_RESULT_SIZE);
    if (!result) {
        return NULL;
    }

    rc = agent->tools[i].handler(input_json, result, LLM_TOOL_RESULT_SIZE);
    if (rc != 0) {
        DiagSnPrintf(result, LLM_TOOL_RESULT_SIZE, "Error: tool '%s' execution failed (rc=%d)", name, rc);
    }

    DiagPrintf("[llm] tool result: %s\n", result);
    return result;
}

/* Extract final text from a content array by concatenating all "text" blocks.
 * Writes into out_buf. Returns 0 on success. */
static int extract_final_text(cJSON *content_arr, char *out_buf, size_t out_buf_sz)
{
    size_t written = 0;
    int count = cJSON_GetArraySize(content_arr);
    int i;

    out_buf[0] = '\0';

    for (i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(content_arr, i);
        cJSON *type = cJSON_GetObjectItem(item, "type");
        if (type && cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0) {
            cJSON *text = cJSON_GetObjectItem(item, "text");
            if (text && cJSON_IsString(text)) {
                size_t tlen = strlen(text->valuestring);
                if (written + tlen >= out_buf_sz) {
                    tlen = out_buf_sz - written - 1;
                }
                if (tlen > 0) {
                    _memcpy(out_buf + written, text->valuestring, tlen);
                    written += tlen;
                }
            }
        }
    }
    out_buf[written] = '\0';
    return (written > 0) ? 0 : -1;
}

/* --- Init / System Prompt --- */

int llm_agent_init(llm_agent_t *agent, const char *api_key)
{
    if (!agent || !api_key) {
        return -1;
    }

    _memset(agent, 0, sizeof(*agent));
    strncpy(agent->api_key, api_key, LLM_API_KEY_SIZE - 1);
    strncpy(agent->model, LLM_DEFAULT_MODEL, LLM_MODEL_NAME_SIZE - 1);
    agent->max_tokens = LLM_DEFAULT_TOKENS;
    strncpy(agent->system_prompt,
            "You are a helpful AI assistant on an embedded device. Keep replies under 200 chars.",
            LLM_MAX_MESSAGE_LEN - 1);

    DiagPrintf("[llm] Agent initialized (model=%s)\n", agent->model);
    return 0;
}

void llm_agent_set_system_prompt(llm_agent_t *agent, const char *prompt)
{
    if (!agent || !prompt) {
        return;
    }
    strncpy(agent->system_prompt, prompt, LLM_MAX_MESSAGE_LEN - 1);
    agent->system_prompt[LLM_MAX_MESSAGE_LEN - 1] = '\0';
}

/* --- Main chat with tool call loop --- */

int llm_agent_chat(llm_agent_t *agent, const char *user_msg,
                   char *out_buf, size_t out_buf_sz)
{
    cJSON *messages_arr = NULL;
    char *tools_json_cache = NULL;
    cJSON *tools_arr_parsed = NULL;
    int iteration;
    int ret = -1;

    if (!agent || !user_msg || !out_buf || out_buf_sz == 0) {
        return -1;
    }

    out_buf[0] = '\0';

    /* Build initial messages array from history + new user message */
    messages_arr = build_messages_array(agent, user_msg);
    if (!messages_arr) {
        DiagPrintf("[llm] build_messages_array failed\n");
        return -1;
    }

    /* Pre-build tools JSON cache (only if tools are registered) */
    if (agent->tool_count > 0) {
        tools_json_cache = build_tools_json(agent);
        if (tools_json_cache) {
            tools_arr_parsed = cJSON_Parse(tools_json_cache);
        }
    }

    for (iteration = 0; iteration <= LLM_MAX_TOOL_ITERATIONS; iteration++) {
        cJSON *req_root = NULL;
        char *req_json = NULL;
        llm_http_resp_t resp;
        cJSON *resp_root = NULL;
        cJSON *stop_reason_item = NULL;
        const char *stop_reason = "";
        cJSON *content_arr = NULL;

        DiagPrintf("[llm] iter %d: building request\n", iteration);

        /* Build request JSON */
        req_root = cJSON_CreateObject();
        if (!req_root) {
            DiagPrintf("[llm] cJSON_CreateObject failed\n");
            ret = -1;
            goto cleanup;
        }

        cJSON_AddStringToObject(req_root, "model", agent->model);
        cJSON_AddNumberToObject(req_root, "max_tokens", agent->max_tokens);
        cJSON_AddStringToObject(req_root, "system", agent->system_prompt);

        /* Deep copy messages array into request */
        {
            cJSON *msg_copy = cJSON_Duplicate(messages_arr, 1);
            if (!msg_copy) {
                DiagPrintf("[llm] messages deep copy failed\n");
                cJSON_Delete(req_root);
                ret = -1;
                goto cleanup;
            }
            cJSON_AddItemToObject(req_root, "messages", msg_copy);
        }

        /* Add tools array if present */
        if (tools_arr_parsed) {
            cJSON *tools_copy = cJSON_Duplicate(tools_arr_parsed, 1);
            if (tools_copy) {
                cJSON_AddItemToObject(req_root, "tools", tools_copy);
            }
        }

        /* Serialize request */
        req_json = cJSON_PrintUnformatted(req_root);
        cJSON_Delete(req_root);
        req_root = NULL;

        if (!req_json) {
            DiagPrintf("[llm] cJSON_Print failed\n");
            ret = -1;
            goto cleanup;
        }

        DiagPrintf("[llm] iter %d: sending request (%u bytes)\n",
               iteration, (unsigned)strlen(req_json));

        /* HTTP POST */
        if (llm_http_resp_init(&resp) != 0) {
            DiagPrintf("[llm] response init failed\n");
            cJSON_free(req_json);
            ret = -1;
            goto cleanup;
        }

        ret = llm_http_post(LLM_API_HOST, LLM_API_RESOURCE,
                            req_json, strlen(req_json),
                            agent->api_key, &resp);
        cJSON_free(req_json);
        req_json = NULL;

        if (ret != 0) {
            DiagPrintf("[llm] HTTP POST failed (%d)\n", ret);
            llm_http_resp_free(&resp);
            ret = -2;
            goto cleanup;
        }

        DiagPrintf("[llm] iter %d: response (%u bytes)\n",
               iteration, (unsigned)resp.len);

        /* Parse response */
        resp_root = cJSON_Parse(resp.buf);
        llm_http_resp_free(&resp);

        if (!resp_root) {
            DiagPrintf("[llm] JSON parse failed\n");
            ret = -3;
            goto cleanup;
        }

        /* Check for API error */
        {
            cJSON *error = cJSON_GetObjectItem(resp_root, "error");
            if (error) {
                cJSON *err_msg = cJSON_GetObjectItem(error, "message");
                if (err_msg && cJSON_IsString(err_msg)) {
                    DiagPrintf("[llm] API error: %s\n", err_msg->valuestring);
                } else {
                    DiagPrintf("[llm] API error (unknown)\n");
                }
                cJSON_Delete(resp_root);
                ret = -4;
                goto cleanup;
            }
        }

        /* Read stop_reason */
        stop_reason_item = cJSON_GetObjectItem(resp_root, "stop_reason");
        if (stop_reason_item && cJSON_IsString(stop_reason_item)) {
            stop_reason = stop_reason_item->valuestring;
        }

        /* Get content array */
        content_arr = cJSON_GetObjectItem(resp_root, "content");
        if (!content_arr || !cJSON_IsArray(content_arr)) {
            DiagPrintf("[llm] no content array in response\n");
            cJSON_Delete(resp_root);
            ret = -4;
            goto cleanup;
        }

        DiagPrintf("[llm] iter %d: stop_reason=%s\n", iteration, stop_reason);

        /* If not tool_use or reached max iterations, extract final text */
        if (strcmp(stop_reason, "tool_use") != 0 || iteration >= LLM_MAX_TOOL_ITERATIONS) {
            extract_final_text(content_arr, out_buf, out_buf_sz);

            /* Save to last_response */
            {
                size_t lp_len = strlen(out_buf);
                if (lp_len >= LLM_RESPONSE_BUF_SIZE) {
                    lp_len = LLM_RESPONSE_BUF_SIZE - 1;
                }
                _memcpy(agent->last_response, out_buf, lp_len);
                agent->last_response[lp_len] = '\0';
            }

            /* Save to history: original user msg + final assistant text */
            history_add(agent, LLM_ROLE_USER, user_msg);
            history_add(agent, LLM_ROLE_ASSISTANT, out_buf);

            cJSON_Delete(resp_root);
            ret = 0;
            goto cleanup;
        }

        /* --- tool_use path --- */

        /* Append assistant message (with full content array) to messages_arr */
        {
            cJSON *asst_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(asst_msg, "role", "assistant");
            cJSON *content_copy = cJSON_Duplicate(content_arr, 1);
            cJSON_AddItemToObject(asst_msg, "content", content_copy);
            cJSON_AddItemToArray(messages_arr, asst_msg);
        }

        /* Build user message with tool_result blocks */
        {
            cJSON *user_tool_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(user_tool_msg, "role", "user");
            cJSON *tool_results = cJSON_CreateArray();

            int count = cJSON_GetArraySize(content_arr);
            int k;
            for (k = 0; k < count; k++) {
                cJSON *item = cJSON_GetArrayItem(content_arr, k);
                cJSON *type = cJSON_GetObjectItem(item, "type");
                if (type && cJSON_IsString(type) && strcmp(type->valuestring, "tool_use") == 0) {
                    cJSON *id_item = cJSON_GetObjectItem(item, "id");
                    cJSON *name_item = cJSON_GetObjectItem(item, "name");
                    cJSON *input_item = cJSON_GetObjectItem(item, "input");

                    const char *tool_id = (id_item && cJSON_IsString(id_item)) ? id_item->valuestring : "";
                    const char *tool_name = (name_item && cJSON_IsString(name_item)) ? name_item->valuestring : "";

                    /* Serialize input to JSON string */
                    char *input_str = NULL;
                    if (input_item) {
                        input_str = cJSON_PrintUnformatted(input_item);
                    }

                    /* Execute the tool */
                    char *tool_result = execute_tool(agent, tool_name,
                                                     input_str ? input_str : "{}");
                    if (input_str) {
                        cJSON_free(input_str);
                        input_str = NULL;
                    }

                    /* Build tool_result block */
                    {
                        cJSON *tr_obj = cJSON_CreateObject();
                        cJSON_AddStringToObject(tr_obj, "type", "tool_result");
                        cJSON_AddStringToObject(tr_obj, "tool_use_id", tool_id);
                        cJSON_AddStringToObject(tr_obj, "content",
                                                tool_result ? tool_result : "Error: no result");
                        cJSON_AddItemToArray(tool_results, tr_obj);
                    }

                    if (tool_result) {
                        rtos_mem_free(tool_result);
                    }
                }
            }

            cJSON_AddItemToObject(user_tool_msg, "content", tool_results);
            cJSON_AddItemToArray(messages_arr, user_tool_msg);
        }

        cJSON_Delete(resp_root);
        /* Continue to next iteration */
    }

    /* Should not reach here, but just in case */
    ret = -5;

cleanup:
    if (messages_arr) {
        cJSON_Delete(messages_arr);
    }
    if (tools_json_cache) {
        cJSON_free(tools_json_cache);
    }
    if (tools_arr_parsed) {
        cJSON_Delete(tools_arr_parsed);
    }
    return ret;
}

void llm_agent_clear_history(llm_agent_t *agent)
{
    if (!agent) {
        return;
    }
    agent->history_count = 0;
    agent->history_head = 0;
    DiagPrintf("[llm] History cleared\n");
}

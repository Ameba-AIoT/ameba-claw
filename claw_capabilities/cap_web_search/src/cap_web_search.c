/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_web_search.h"
#include "claw_cap.h"
#include "claw_config.h"
#include "llm_agent_http.h"
#include <cJSON.h>
#include "platform_stdlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "cap_web_search"

/* ---- execute: web_search ---- */

static int cap_search(const char *input_json,
                             const claw_cap_call_context_t *ctx,
                             char **output)
{
    (void)ctx;

    const claw_web_search_config_t *cfg = &claw_config_get()->web_search;
    const char *query      = NULL;
    int         n_results  = cfg->max_results > 0 ? cfg->max_results : 3;
    cJSON      *root       = NULL;
    char       *body       = NULL;
    llm_http_resp_t resp   = {0};
    cJSON      *jresp      = NULL;

    /* --- Parse input --- */
    root = cJSON_Parse(input_json);
    if (!root) {
        claw_cap_set_output(output, "{\"error\":\"invalid input JSON\"}");
        return RTK_FAIL;
    }

    cJSON *jq = cJSON_GetObjectItem(root, "query");
    if (!jq || !cJSON_IsString(jq) || !jq->valuestring || jq->valuestring[0] == '\0') {
        claw_cap_set_output(output, "{\"error\":\"missing required field: query\"}");
        cJSON_Delete(root);
        return RTK_FAIL;
    }
    query = jq->valuestring;

    cJSON *jn = cJSON_GetObjectItem(root, "max_results");
    if (jn && cJSON_IsNumber(jn)) {
        n_results = (int)jn->valuedouble;
        if (n_results < 1) n_results = 1;
        if (n_results > 5) n_results = 5;
    }

    /* --- Check api_key --- */
    if (cfg->api_key[0] == '\0') {
        claw_cap_set_output(output, "{\"error\":\"web search not configured, api_key is empty\"}");
        cJSON_Delete(root);
        return RTK_FAIL;
    }

    /* --- Build Tavily request body --- */
    /* Escape query string via cJSON to handle quotes/backslashes */
    cJSON *jbody = cJSON_CreateObject();
    if (!jbody) {
        claw_cap_set_output(output, "{\"error\":\"out of memory\"}");
        cJSON_Delete(root);
        return RTK_ERR_NOMEM;
    }
    cJSON_AddStringToObject(jbody, "query", query);
    cJSON_AddNumberToObject(jbody, "max_results", n_results);
    cJSON_AddStringToObject(jbody, "search_depth", "basic");
    cJSON_AddTrueToObject(jbody,   "include_answer");

    body = cJSON_PrintUnformatted(jbody);
    cJSON_Delete(jbody);
    cJSON_Delete(root);
    root = NULL;

    if (!body) {
        claw_cap_set_output(output, "{\"error\":\"out of memory building request\"}");
        return RTK_ERR_NOMEM;
    }

    /* --- Call Tavily API --- */
    if (llm_http_resp_init(&resp) != 0) {
        free(body);
        claw_cap_set_output(output, "{\"error\":\"search failed: resp init\"}");
        return RTK_FAIL;
    }

    int rc = llm_http_post_bearer("api.tavily.com", "/search",
                                  body, strlen(body),
                                  cfg->api_key, &resp);
    free(body);
    body = NULL;

    if (rc != 0 || !resp.buf) {
        llm_http_resp_free(&resp);
        claw_cap_set_output(output, "{\"error\":\"search failed: HTTP error %d\"}", rc);
        return RTK_FAIL;
    }

    /* --- Parse Tavily response --- */
    jresp = cJSON_Parse(resp.buf);
    llm_http_resp_free(&resp);

    if (!jresp) {
        claw_cap_set_output(output, "{\"error\":\"search failed: response parse error\"}");
        return RTK_FAIL;
    }

    /* Build output JSON */
    cJSON *jout     = cJSON_CreateObject();
    cJSON *jresults = cJSON_CreateArray();

    if (!jout || !jresults) {
        cJSON_Delete(jout);
        cJSON_Delete(jresults);
        cJSON_Delete(jresp);
        claw_cap_set_output(output, "{\"error\":\"out of memory building output\"}");
        return RTK_ERR_NOMEM;
    }

    /* answer field (optional) */
    cJSON *janswer = cJSON_GetObjectItem(jresp, "answer");
    if (janswer && cJSON_IsString(janswer) && janswer->valuestring) {
        cJSON_AddStringToObject(jout, "answer", janswer->valuestring);
    } else {
        cJSON_AddStringToObject(jout, "answer", "");
    }

    /* results array */
    cJSON *jres_arr = cJSON_GetObjectItem(jresp, "results");
    if (jres_arr && cJSON_IsArray(jres_arr)) {
        int count = cJSON_GetArraySize(jres_arr);
        int i;
        for (i = 0; i < count; i++) {
            cJSON *item = cJSON_GetArrayItem(jres_arr, i);
            if (!item) continue;

            cJSON *jtitle   = cJSON_GetObjectItem(item, "title");
            cJSON *jcontent = cJSON_GetObjectItem(item, "content");

            const char *title   = (jtitle   && cJSON_IsString(jtitle))   ? jtitle->valuestring   : "";
            const char *content = (jcontent && cJSON_IsString(jcontent)) ? jcontent->valuestring : "";

            /* Truncate content to 200 UTF-8-safe bytes */
            char snippet[201];
            size_t clen = strlen(content);
            size_t snap = clen > 200 ? 200 : clen;
            if (snap < clen) {
                /* Walk back to a valid UTF-8 boundary */
                while (snap > 0 && ((unsigned char)content[snap] & 0xC0) == 0x80)
                    snap--;
            }
            memcpy(snippet, content, snap);
            snippet[snap] = '\0';

            cJSON *jentry = cJSON_CreateObject();
            if (jentry) {
                cJSON_AddStringToObject(jentry, "title",   title);
                cJSON_AddStringToObject(jentry, "snippet", snippet);
                cJSON_AddItemToArray(jresults, jentry);
            }
        }
    }

    cJSON_AddItemToObject(jout, "results", jresults);
    cJSON_Delete(jresp);

    char *out_str = cJSON_PrintUnformatted(jout);
    cJSON_Delete(jout);

    if (!out_str) {
        claw_cap_set_output(output, "{\"error\":\"out of memory serializing output\"}");
        return RTK_ERR_NOMEM;
    }

    *output = out_str;
    return RTK_SUCCESS;
}

/* ---- Cap descriptor & group ---- */

static const claw_cap_descriptor_t s_desc[] = {
    {
        .id          = "web_search",
        .name        = "web_search",
        .family      = "search",
        .description = "Search the web for information. Returns JSON with fields: answer (string), results (array of {title, snippet}).",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"query\":{\"type\":\"string\",\"description\":\"Search query\"},"
            "\"max_results\":{\"type\":\"integer\",\"description\":\"Number of results (1-5)\"}"
            "},"
            "\"required\":[\"query\"]}",
        .execute = cap_search,
    },
};

static const claw_cap_group_t s_group = {
    .group_id         = "web_search",
    .plugin_name      = "cap_web_search",
    .version          = "1",
    .descriptors      = s_desc,
    .descriptor_count = 1,
};

/* ---- Public init ---- */

int cap_web_search_init(const cap_web_search_config_t *config)
{
    (void)config; /* config read from claw_config at call time */

    int err = claw_cap_register_group(&s_group);
    if (err != RTK_SUCCESS) {
        RTK_LOGE(TAG, "Failed to register group: %d\n", (int)err);
        return err;
    }

    const claw_web_search_config_t *cfg = &claw_config_get()->web_search;
    RTK_LOGI(TAG, "Initialized (endpoint=api.tavily.com, max_results=%d, key=%s)\n",
             cfg->max_results,
             cfg->api_key[0] != '\0' ? "configured" : "empty");

    return RTK_SUCCESS;
}

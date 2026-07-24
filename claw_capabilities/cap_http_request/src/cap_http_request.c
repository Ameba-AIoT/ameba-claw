/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_http_request.h"
#include "claw_cap.h"
#include "claw_cap_registry.h"
#include "claw_config.h"
#include "llm_agent_http.h"
#include <cJSON.h>
#include "platform_stdlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "cap_http_request"

/* ---- URL parsing ---- */

/* Parse "http[s]://host[:port]/path?q=1" → host_port + resource.
 * For http:// with no explicit port, host_port is set to "host:80" so that
 * llm_http_request's parse_host_port selects plain TCP (port != 443).
 * Host ends at the first '/', '?', or '#' (or end of string).
 * If separator is '?' or '#' (no explicit path), resource is prepended with '/'. */
static int parse_url(const char *url,
                     char *host, size_t host_sz,
                     char *resource, size_t res_sz)
{
    bool is_https;
    const char *p;
    if (strncmp(url, "https://", 8) == 0) {
        is_https = true;
        p = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        is_https = false;
        p = url + 7;
    } else {
        return -1;
    }
    const char *sep = p;
    while (*sep && *sep != '/' && *sep != '?' && *sep != '#') sep++;
    size_t hlen = (size_t)(sep - p);
    if (hlen == 0 || hlen >= host_sz) return -1;
    memcpy(host, p, hlen);
    host[hlen] = '\0';
    /* For http:// without explicit port, append ":80" so parse_host_port
     * picks plain TCP (it treats any port != 443 as non-TLS). */
    if (!is_https && !memchr(p, ':', hlen)) {
        if (hlen + 3 >= host_sz) return -1;
        strlcat(host, ":80", host_sz);
    }
    if (*sep == '\0') {
        strlcpy(resource, "/", res_sz);
    } else if (*sep == '/') {
        strlcpy(resource, sep, res_sz);
    } else {
        /* '?' or '#' without explicit path — prepend '/' */
        if (res_sz < 2) return -1;
        resource[0] = '/';
        strlcpy(resource + 1, sep, res_sz - 1);
    }
    return 0;
}

/* ---- Allowlist ---- */

/* Wildcard pattern match supporting zero or more '*' wildcards.
 * Uses the two-pointer greedy algorithm (O(n), non-recursive).
 * Examples: *.example.com, 192.168.1.*, *.s3.*.amazonaws.com, * */
static bool wildcard_match(const char *host, const char *pattern)
{
    const char *h = host, *p = pattern;
    const char *star_p = NULL, *star_h = NULL;
    while (*h) {
        if (*p == '*') {
            star_p = p++;
            star_h = h;
        } else if (*p == *h) {
            p++; h++;
        } else if (star_p) {
            p = star_p + 1;
            h = ++star_h;
        } else {
            return false;
        }
    }
    while (*p == '*') p++;
    return *p == '\0';
}

/* Returns true if host is permitted by the allowlist.
 * Empty allowlist = deny all (no rules → nothing matches).
 * "*" alone = allow all.  Otherwise match host against each rule in order. */
static bool allowlist_permit(const char *host_with_port)
{
    const claw_http_request_config_t *cfg = &claw_config_get()->http_request;
    if (cfg->allowlist[0] == '\0') return false;

    /* Strip port for matching, handling IPv6 bracket literals correctly.
     * Heap-allocate to (a) stay within the 128-byte stack-variable limit and
     * (b) accommodate DNS hostnames up to 253 chars (validated at save time). */
    char *host = (char *)malloc(256);
    if (!host) return false;
    if (host_with_port[0] == '[') {
        /* IPv6: "[addr]:port" or "[addr]" → extract addr without brackets */
        const char *end_bracket = strchr(host_with_port + 1, ']');
        if (!end_bracket) end_bracket = host_with_port + strlen(host_with_port);
        size_t alen = (size_t)(end_bracket - (host_with_port + 1));
        if (alen >= 256) alen = 255;
        memcpy(host, host_with_port + 1, alen);
        host[alen] = '\0';
    } else {
        strlcpy(host, host_with_port, 256);
        char *colon = strrchr(host, ':');
        if (colon) *colon = '\0';
    }

    /* Walk newline-separated rules.  Heap-allocate the working copy so we
     * don't put CLAW_HTTP_REQUEST_ALLOWLIST_MAX (512) bytes on the task stack. */
    char *rules = (char *)malloc(CLAW_HTTP_REQUEST_ALLOWLIST_MAX);
    if (!rules) { free(host); return false; }
    strlcpy(rules, cfg->allowlist, CLAW_HTTP_REQUEST_ALLOWLIST_MAX);
    bool permit = false;
    char *line = rules;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        /* trim leading spaces/CR */
        while (*line == ' ' || *line == '\r') line++;
        /* trim trailing spaces/CR */
        char *end = line + strlen(line);
        while (end > line && (*(end - 1) == ' ' || *(end - 1) == '\r')) end--;
        *end = '\0';
        if (*line) {
            /* Strip port from rule (e.g. "api.example.com:8443" → "api.example.com").
             * Matching is hostname-only, consistent with how the target host is stripped. */
            char *rule_colon = strrchr(line, ':');
            if (rule_colon) *rule_colon = '\0';
            if (wildcard_match(host, line)) { permit = true; break; }
        }
        line = next;
    }
    free(rules);
    free(host);
    return permit;
}

/* ---- Capability execute ---- */

static int cap_http_request_execute(const char *input_json,
                                     const claw_cap_call_context_t *ctx,
                                     char **output)
{
    (void)ctx;

    cJSON *root     = NULL;
    char  *body_str = NULL;
    char  *extra_hdr = NULL;
    llm_http_resp_t resp = {0};
    int    ret      = RTK_FAIL;

    /* --- Parse input --- */
    root = cJSON_Parse(input_json);
    if (!root) {
        claw_cap_set_output(output, "{\"error\":\"invalid input JSON\"}");
        return RTK_FAIL;
    }

    cJSON *jmethod = cJSON_GetObjectItem(root, "method");
    cJSON *jurl    = cJSON_GetObjectItem(root, "url");
    if (!jmethod || !cJSON_IsString(jmethod) || !jurl || !cJSON_IsString(jurl)) {
        claw_cap_set_output(output, "{\"error\":\"missing required fields: method, url\"}");
        cJSON_Delete(root);
        return RTK_FAIL;
    }

    const char *method = jmethod->valuestring;
    const char *url    = jurl->valuestring;

    /* Validate method */
    static const char *valid_methods[] = {
        "GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", NULL
    };
    int method_ok = 0;
    for (int i = 0; valid_methods[i]; i++) {
        if (strcmp(method, valid_methods[i]) == 0) { method_ok = 1; break; }
    }
    if (!method_ok) {
        claw_cap_set_output(output, "{\"error\":\"invalid method; use GET/POST/PUT/PATCH/DELETE/HEAD\"}");
        cJSON_Delete(root);
        return RTK_FAIL;
    }

    /* --- Parse URL --- */
    /* Heap-allocate to stay within the 128-byte stack-variable limit. */
    char *host     = (char *)malloc(256);
    char *resource = (char *)malloc(512);
    if (!host || !resource) {
        free(host); free(resource);
        claw_cap_set_output(output, "{\"error\":\"out of memory parsing URL\"}");
        cJSON_Delete(root);
        return RTK_FAIL;
    }
    if (parse_url(url, host, 256, resource, 512) != 0) {
        free(host); free(resource);
        claw_cap_set_output(output, "{\"error\":\"invalid URL; must start with http:// or https://\"}");
        cJSON_Delete(root);
        return RTK_FAIL;
    }

    /* --- Allowlist check --- */
    if (!allowlist_permit(host)) {
        free(host); free(resource);
        claw_cap_set_output(output, "{\"error\":\"blocked by allowlist; configure allowed hosts in HTTP Request settings\"}");
        cJSON_Delete(root);
        return RTK_FAIL;
    }

    /* --- Build extra_headers from headers object --- */
    cJSON *jheaders = cJSON_GetObjectItem(root, "headers");
    if (jheaders && cJSON_IsObject(jheaders)) {
        /* Estimate size needed: each header "Key: Value\r\n" */
        size_t hdr_est = 0;
        cJSON *item = jheaders->child;
        while (item) {
            hdr_est += strlen(item->string) + 2 + /* ": " */
                       (cJSON_IsString(item) ? strlen(item->valuestring) : 0) + 2; /* \r\n */
            item = item->next;
        }
        if (hdr_est > 0) {
            extra_hdr = (char *)malloc(hdr_est + 4);
            if (!extra_hdr) {
                free(host); free(resource);
                claw_cap_set_output(output, "{\"error\":\"out of memory\"}");
                cJSON_Delete(root);
                return RTK_ERR_NOMEM;
            }
            char *p = extra_hdr;
            cJSON *it = jheaders->child;
            while (it) {
                if (cJSON_IsString(it)) {
                    int n = DiagSnPrintf(p, (int)(extra_hdr + hdr_est + 4 - p),
                                        "%s: %s\r\n", it->string, it->valuestring);
                    if (n > 0) p += n;
                }
                it = it->next;
            }
            *p = '\0';
        }
    }

    /* --- Get body --- */
    size_t body_len = 0;
    cJSON *jbody = cJSON_GetObjectItem(root, "body");
    if (jbody && cJSON_IsString(jbody) && jbody->valuestring[0]) {
        body_str = jbody->valuestring; /* points into root JSON, valid until cJSON_Delete */
        body_len = strlen(body_str);
    }

    /* --- Execute HTTP request --- */
    if (llm_http_resp_init(&resp) != 0) {
        free(host); free(resource);
        claw_cap_set_output(output, "{\"error\":\"out of memory initializing response\"}");
        free(extra_hdr);
        cJSON_Delete(root);
        return RTK_FAIL;
    }

    int status_code = 0;
    int rc = llm_http_request(method, host, resource,
                               extra_hdr,
                               body_str, body_len,
                               &status_code, &resp);
    free(host); free(resource);
    host = NULL; resource = NULL;
    free(extra_hdr);
    extra_hdr = NULL;

    cJSON_Delete(root);
    root = NULL;

    if (rc != 0) {
        llm_http_resp_free(&resp);
        claw_cap_set_output(output, "{\"error\":\"HTTP request failed (rc=%d)\"}", rc);
        return RTK_FAIL;
    }

    /* --- Build output JSON --- */
    /* Truncate very long bodies to protect LLM context (keep 8KB) */
    const size_t MAX_BODY_OUT = 8192;
    if (resp.len > MAX_BODY_OUT) {
        resp.buf[MAX_BODY_OUT] = '\0';
        resp.len = MAX_BODY_OUT;
    }

    cJSON *jout = cJSON_CreateObject();
    if (!jout) {
        llm_http_resp_free(&resp);
        claw_cap_set_output(output, "{\"error\":\"out of memory building output\"}");
        return RTK_ERR_NOMEM;
    }
    cJSON_AddNumberToObject(jout, "status_code", status_code);

    /* Try to parse body as JSON for cleaner output; fall back to raw string */
    cJSON *jparsed = resp.len > 0 ? cJSON_Parse(resp.buf) : NULL;
    if (jparsed) {
        cJSON_AddItemToObject(jout, "body", jparsed);
    } else {
        cJSON_AddStringToObject(jout, "body", resp.len > 0 ? resp.buf : "");
    }
    llm_http_resp_free(&resp);

    char *out_str = cJSON_PrintUnformatted(jout);
    cJSON_Delete(jout);

    if (!out_str) {
        claw_cap_set_output(output, "{\"error\":\"out of memory serializing output\"}");
        return RTK_ERR_NOMEM;
    }

    *output = out_str;
    ret = RTK_SUCCESS;
    return ret;
}

/* ---- Cap descriptor & group ---- */

static const claw_cap_descriptor_t s_desc[] = {
    {
        .id          = "http_request",
        .name        = "http_request",
        .family      = "network",
        .description =
            "Send an HTTP or HTTPS request and return the response. "
            "Supports GET/POST/PUT/PATCH/DELETE/HEAD. "
            "Both http:// and https:// URLs are supported. "
            "Returns {status_code: N, body: ...}. "
            "URL allowlist: empty=deny all, *=allow all, otherwise host must match a rule. "
            "Configure via WebUI HTTP Request settings. "
            "Note: timeout parameter is accepted but uses system default.",
        .kind      = CLAW_CAP_KIND_INVOKE,
        .cap_flags = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"method\":{\"type\":\"string\","
              "\"enum\":[\"GET\",\"POST\",\"PUT\",\"PATCH\",\"DELETE\",\"HEAD\"],"
              "\"description\":\"HTTP method\"},"
            "\"url\":{\"type\":\"string\","
              "\"description\":\"Full URL (http:// or https://), e.g. http://ip-api.com/json or https://api.example.com/path\"},"
            "\"headers\":{\"type\":\"object\","
              "\"description\":\"Optional HTTP request headers as key-value pairs\"},"
            "\"body\":{\"type\":\"string\","
              "\"description\":\"Optional request body (for POST/PUT/PATCH)\"},"
            "\"timeout\":{\"type\":\"integer\","
              "\"description\":\"Timeout in seconds (accepted but uses system default)\"}"
            "},"
            "\"required\":[\"method\",\"url\"]}",
        .execute = cap_http_request_execute,
    },
};

static const claw_cap_group_t s_group = {
    .group_id         = "http_request",
    .plugin_name      = "cap_http_request",
    .version          = "1",
    .descriptors      = s_desc,
    .descriptor_count = 1,
};

/* ---- Public init ---- */

int cap_http_request_init(void)
{
    int err = claw_cap_register_group(&s_group);
    if (err != RTK_SUCCESS) {
        RTK_LOGE(TAG, "Failed to register group: %d\n", (int)err);
        return err;
    }

    const claw_http_request_config_t *cfg = &claw_config_get()->http_request;
    RTK_LOGI(TAG, "Initialized (allowlist=%s)\n",
             cfg->allowlist[0] == '\0' ? "empty (deny all)" :
             (strcmp(cfg->allowlist, "*") == 0 ? "* (allow all)" : "configured"));
    return RTK_SUCCESS;
}

/* ---- Lifecycle registration (claw_cap_registry): pure INIT phase ---- */
static void http_request_on_init(const claw_config_t *cfg)
{
    (void)cfg;
    cap_http_request_init();
}
CLAW_CAP_REGISTER(http_request, {
    .group   = "http_request",
    .order   = 70,
    .on_init = http_request_on_init,
});

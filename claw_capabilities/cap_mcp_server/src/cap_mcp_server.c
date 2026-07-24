/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cap_mcp_server.h"
#include "claw_cap.h"
#include "claw_cap_registry.h"
#include "claw_http_server.h"
#include "claw_event_publisher.h"
#include <cJSON.h>
#include "platform_stdlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "rtk_mcp_srv"

#define MCP_PROTOCOL_VERSION_OLD "2024-11-05"
#define MCP_PROTOCOL_VERSION_NEW "2025-03-26"
#define MCP_SERVER_VERSION   "1.0.0"

static struct {
    char endpoint[64];
    char server_name[64];
    int  initialized;
} s;

/* ---- JSON-RPC helpers ---- */

static cJSON *make_jsonrpc_result(cJSON *id, cJSON *result)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) {
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    } else {
        cJSON_AddNullToObject(resp, "id");
    }
    cJSON_AddItemToObject(resp, "result", result);
    return resp;
}

static cJSON *make_jsonrpc_error(cJSON *id, int code, const char *message)
{
    cJSON *resp  = cJSON_CreateObject();
    cJSON *error = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) {
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    } else {
        cJSON_AddNullToObject(resp, "id");
    }
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddItemToObject(resp, "error", error);
    return resp;
}

static void send_json(claw_http_send_fn_t send_fn, int sock, int http_status, cJSON *obj)
{
    char *body = cJSON_PrintUnformatted(obj);
    if (body) {
        send_fn(sock, http_status, "application/json", body, strlen(body));
        free(body);
    } else {
        send_fn(sock, 500, "text/plain", "internal error", 14);
    }
    cJSON_Delete(obj);
}

/* ---- MCP method handlers ---- */

static cJSON *handle_initialize(cJSON *params, cJSON *id)
{
    /* MCP-5: negotiate version — echo client's if supported, else downgrade */
    const char *neg_ver = MCP_PROTOCOL_VERSION_OLD;
    if (params) {
        cJSON *pv = cJSON_GetObjectItem(params, "protocolVersion");
        if (pv && cJSON_IsString(pv)) {
            if (strcmp(pv->valuestring, MCP_PROTOCOL_VERSION_NEW) == 0)
                neg_ver = MCP_PROTOCOL_VERSION_NEW;
        }
    }

    cJSON *result = cJSON_CreateObject();
    cJSON *info   = cJSON_CreateObject();
    cJSON *caps   = cJSON_CreateObject();
    cJSON *tools  = cJSON_CreateObject();

    cJSON_AddStringToObject(result, "protocolVersion", neg_ver);
    cJSON_AddStringToObject(info, "name", s.server_name);
    cJSON_AddStringToObject(info, "version", MCP_SERVER_VERSION);
    cJSON_AddItemToObject(result, "serverInfo", info);
    cJSON_AddBoolToObject(tools, "listChanged", false);
    cJSON_AddItemToObject(caps, "tools", tools);
    cJSON_AddItemToObject(result, "capabilities", caps);

    return make_jsonrpc_result(id, result);
}

static cJSON *handle_tools_list(cJSON *params, cJSON *id)
{
    (void)params;
    cJSON *result     = cJSON_CreateObject();
    cJSON *tools_arr  = cJSON_CreateArray();

    /* Tool: device.report_state */
    {
        cJSON *tool  = cJSON_CreateObject();
        cJSON *schema = cJSON_CreateObject();
        cJSON *props  = cJSON_CreateObject();
        cJSON *req    = cJSON_CreateArray();

        cJSON_AddStringToObject(tool, "name", "rtk.device.state");
        cJSON_AddStringToObject(tool, "description",
            "Report a device state update. Publishes a trigger event to the agent.");

        cJSON *p_dev   = cJSON_CreateObject();
        cJSON *p_state = cJSON_CreateObject();
        cJSON *p_val   = cJSON_CreateObject();
        cJSON_AddStringToObject(p_dev,   "type", "string");
        cJSON_AddStringToObject(p_dev,   "description", "Device identifier");
        cJSON_AddStringToObject(p_state, "type", "string");
        cJSON_AddStringToObject(p_state, "description", "State name");
        cJSON_AddStringToObject(p_val,   "type", "string");
        cJSON_AddStringToObject(p_val,   "description", "State value");
        cJSON_AddItemToObject(props, "device_id",  p_dev);
        cJSON_AddItemToObject(props, "state_name", p_state);
        cJSON_AddItemToObject(props, "value",      p_val);
        cJSON_AddItemToArray(req, cJSON_CreateString("device_id"));
        cJSON_AddItemToArray(req, cJSON_CreateString("state_name"));
        cJSON_AddItemToArray(req, cJSON_CreateString("value"));

        cJSON_AddStringToObject(schema, "type", "object");
        cJSON_AddItemToObject(schema, "properties", props);
        cJSON_AddItemToObject(schema, "required", req);
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools_arr, tool);
    }

    /* Tool: router.emit_event */
    {
        cJSON *tool  = cJSON_CreateObject();
        cJSON *schema = cJSON_CreateObject();
        cJSON *props  = cJSON_CreateObject();
        cJSON *req    = cJSON_CreateArray();

        cJSON_AddStringToObject(tool, "name", "rtk.router.trigger");
        cJSON_AddStringToObject(tool, "description",
            "Emit a trigger event into the agent event router.");

        cJSON *p_type    = cJSON_CreateObject();
        cJSON *p_text    = cJSON_CreateObject();
        cJSON *p_channel = cJSON_CreateObject();
        cJSON *p_payload = cJSON_CreateObject();
        cJSON_AddStringToObject(p_type,    "type", "string");
        cJSON_AddStringToObject(p_type,    "description", "Event type");
        cJSON_AddStringToObject(p_text,    "type", "string");
        cJSON_AddStringToObject(p_text,    "description", "Optional text payload");
        cJSON_AddStringToObject(p_channel, "type", "string");
        cJSON_AddStringToObject(p_channel, "description", "Optional target channel");
        cJSON_AddStringToObject(p_payload, "type", "string");
        cJSON_AddStringToObject(p_payload, "description", "Optional JSON payload string");
        cJSON_AddItemToObject(props, "event_type",      p_type);
        cJSON_AddItemToObject(props, "text",            p_text);
        cJSON_AddItemToObject(props, "target_channel",  p_channel);
        cJSON_AddItemToObject(props, "payload_json",    p_payload);
        cJSON_AddItemToArray(req, cJSON_CreateString("event_type"));

        cJSON_AddStringToObject(schema, "type", "object");
        cJSON_AddItemToObject(schema, "properties", props);
        cJSON_AddItemToObject(schema, "required", req);
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(tools_arr, tool);
    }

    cJSON_AddItemToObject(result, "tools", tools_arr);
    return make_jsonrpc_result(id, result);
}

static cJSON *handle_tools_call(cJSON *params, cJSON *id)
{
    if (!params) {
        return make_jsonrpc_error(id, -32602, "missing params");
    }

    cJSON *jname   = cJSON_GetObjectItem(params, "name");
    cJSON *jargs   = cJSON_GetObjectItem(params, "arguments");
    if (!jname || !cJSON_IsString(jname)) {
        return make_jsonrpc_error(id, -32602, "missing tool name");
    }

    const char *name = jname->valuestring;
    cJSON *args = jargs ? jargs : cJSON_CreateObject();
    int   free_args = (jargs == NULL);

    cJSON *content_arr = cJSON_CreateArray();
    cJSON *text_item   = cJSON_CreateObject();
    cJSON_AddStringToObject(text_item, "type", "text");
    int is_error = 0;

    if (strcmp(name, "rtk.device.state") == 0) {
        cJSON *j_dev   = cJSON_GetObjectItem(args, "device_id");
        cJSON *j_state = cJSON_GetObjectItem(args, "state_name");
        cJSON *j_val   = cJSON_GetObjectItem(args, "value");

        if (!j_dev || !cJSON_IsString(j_dev) ||
            !j_state || !cJSON_IsString(j_state) ||
            !j_val   || !cJSON_IsString(j_val)) {
            is_error = 1;
            cJSON_AddStringToObject(text_item, "text",
                "error: missing device_id, state_name or value");
        } else {
            cJSON *payload = cJSON_CreateObject();
            cJSON_AddStringToObject(payload, "device_id",  j_dev->valuestring);
            cJSON_AddStringToObject(payload, "state_name", j_state->valuestring);
            cJSON_AddStringToObject(payload, "value",      j_val->valuestring);
            char *payload_str = cJSON_PrintUnformatted(payload);
            cJSON_Delete(payload);

            claw_event_dispatcher_publish_trigger("mcp_server", "mcp_device_state_report",
                                              j_state->valuestring, payload_str);
            free(payload_str);

            char out[128];
            DiagSnPrintf(out, sizeof(out), "accepted: device_id=%s state_name=%s value=%s",
                     j_dev->valuestring, j_state->valuestring, j_val->valuestring);
            cJSON_AddStringToObject(text_item, "text", out);
        }
    } else if (strcmp(name, "rtk.router.trigger") == 0) {
        cJSON *j_type    = cJSON_GetObjectItem(args, "event_type");
        cJSON *j_text    = cJSON_GetObjectItem(args, "text");
        cJSON *j_channel = cJSON_GetObjectItem(args, "target_channel");
        cJSON *j_payload = cJSON_GetObjectItem(args, "payload_json");

        if (!j_type || !cJSON_IsString(j_type)) {
            is_error = 1;
            cJSON_AddStringToObject(text_item, "text", "error: missing event_type");
        } else {
            const char *payload_str = (j_payload && cJSON_IsString(j_payload))
                                      ? j_payload->valuestring : "{}";
            claw_event_dispatcher_publish_trigger("mcp_server",
                                              j_type->valuestring,
                                              j_type->valuestring,
                                              payload_str);

            /* If text was provided and there's a target channel, also publish a message */
            if (j_text && cJSON_IsString(j_text) && j_text->valuestring[0]
                && j_channel && cJSON_IsString(j_channel) && j_channel->valuestring[0]) {
                claw_event_dispatcher_publish_message("mcp_server",
                                                  j_channel->valuestring,
                                                  j_channel->valuestring,
                                                  j_text->valuestring,
                                                  NULL, NULL);
            }

            char out[96];
            DiagSnPrintf(out, sizeof(out), "accepted: event_type=%s", j_type->valuestring);
            cJSON_AddStringToObject(text_item, "text", out);
        }
    } else {
        char msg[80];
        DiagSnPrintf(msg, sizeof(msg), "unknown tool: %s", name);
        cJSON_Delete(text_item);
        cJSON_Delete(content_arr);
        if (free_args) cJSON_Delete(args);
        return make_jsonrpc_error(id, -32601, msg);
    }

    if (free_args) cJSON_Delete(args);
    cJSON_AddItemToArray(content_arr, text_item);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "content", content_arr);
    cJSON_AddBoolToObject(result, "isError", is_error ? true : false);
    return make_jsonrpc_result(id, result);
}

/* ---- HTTP handler ---- */

static void mcp_http_handler(const claw_http_request_t *req,
                              claw_http_send_fn_t send_fn, int sock)
{
    if (req->method != HTTP_POST || !req->body || req->body_len == 0) {
        send_fn(sock, 405, "text/plain", "Method Not Allowed", 18);
        return;
    }

    cJSON *root = cJSON_ParseWithLength(req->body, req->body_len);
    if (!root) {
        cJSON *err = make_jsonrpc_error(NULL, -32700, "parse error");
        send_json(send_fn, sock, 200, err);
        return;
    }

    cJSON *jid     = cJSON_GetObjectItem(root, "id");
    cJSON *jmethod = cJSON_GetObjectItem(root, "method");
    cJSON *jparams = cJSON_GetObjectItem(root, "params");

    if (!jmethod || !cJSON_IsString(jmethod)) {
        cJSON *err = make_jsonrpc_error(jid, -32600, "invalid request");
        cJSON_Delete(root);
        send_json(send_fn, sock, 200, err);
        return;
    }

    const char *method = jmethod->valuestring;
    cJSON *resp = NULL;

    if (strcmp(method, "initialize") == 0) {
        resp = handle_initialize(jparams, jid);
    } else if (strcmp(method, "notifications/initialized") == 0) {
        /* Notification — no response needed */
        cJSON_Delete(root);
        send_fn(sock, 204, "application/json", "", 0);
        return;
    } else if (strcmp(method, "tools/list") == 0) {
        resp = handle_tools_list(jparams, jid);
    } else if (strcmp(method, "tools/call") == 0) {
        resp = handle_tools_call(jparams, jid);
    } else if (strcmp(method, "ping") == 0) {
        cJSON *empty = cJSON_CreateObject();
        resp = make_jsonrpc_result(jid, empty);
    } else {
        resp = make_jsonrpc_error(jid, -32601, "method not found");
    }

    cJSON_Delete(root);
    if (resp) {
        send_json(send_fn, sock, 200, resp);
    } else {
        send_fn(sock, 500, "text/plain", "internal error", 14);
    }
}

/* ---- LLM-callable cap ---- */

static int execute_mcp_server_status(const char *input_json,
                                      const claw_cap_call_context_t *ctx,
                                      char **output)
{
    (void)input_json;
    (void)ctx;
    return claw_cap_set_output(output,
             "{\"mcp_server\":{\"endpoint\":\"%s\",\"server_name\":\"%s\","
             "\"protocol_version\":\"%s\",\"status\":\"%s\"}}",
             s.endpoint, s.server_name,
             MCP_PROTOCOL_VERSION_NEW,
             s.initialized ? "running" : "not initialized");
}

static const claw_cap_descriptor_t s_caps[] = {
    {
        .id          = "mcp_server_status",
        .name        = "mcp_server_status",
        .family      = "mcp_server",
        .description = "Query local MCP server running status and endpoint address.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute     = execute_mcp_server_status,
    },
};

static const claw_cap_group_t s_group = {
    .group_id         = "mcp_server",
    .plugin_name      = "cap_mcp_server",
    .version          = "1",
    .descriptors      = s_caps,
    .descriptor_count = 1,
};

/* ---- Public API ---- */

int cap_mcp_server_init(const cap_mcp_server_config_t *cfg)
{
    const char *endpoint    = (cfg && cfg->endpoint    && cfg->endpoint[0])
                              ? cfg->endpoint    : CAP_MCP_SERVER_DEFAULT_ENDPOINT;
    const char *server_name = (cfg && cfg->server_name && cfg->server_name[0])
                              ? cfg->server_name : "ameba-claw";

    strlcpy(s.endpoint,    endpoint,    sizeof(s.endpoint));
    strlcpy(s.server_name, server_name, sizeof(s.server_name));

    claw_http_server_add_route(HTTP_POST, s.endpoint, mcp_http_handler);
    claw_cap_register_group(&s_group);

    s.initialized = 1;
    RTK_LOGI(TAG, "MCP server registered at POST %s\n", s.endpoint);
    return RTK_SUCCESS;
}

/* ---- Lifecycle registration (claw_cap_registry): IO phase (HTTP route) ----
 * Registers the POST /mcp route, so it must run before claw_http_server_start()
 * — guaranteed because registry_run(IO) precedes http_server_start(). */
static void mcp_server_on_io(const claw_config_t *cfg)
{
    (void)cfg;
    cap_mcp_server_config_t c = CAP_MCP_SERVER_DEFAULT_CONFIG();
    cap_mcp_server_init(&c);
}
CLAW_CAP_REGISTER(mcp_server, {
    .group = "mcp_server",
    .order = 170,
    .on_io = mcp_server_on_io,
});

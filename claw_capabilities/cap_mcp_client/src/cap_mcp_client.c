/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * cap_mcp_client.c — MCP (Model Context Protocol) over HTTP/JSON-RPC 2.0 client.
 *
 * Reads /mcp/servers.json at init, connects to each configured server,
 * discovers tools via tools/list, and registers each as a claw_cap.
 *
 * Supports up to 4 servers × 8 tools = 32 tool slots.
 */

#include "cap_mcp_client.h"
#include "claw_cap.h"
#include "claw_cap_registry.h"
#include "claw_wifi_mgr.h"
#include "os_wrapper.h"
#include "llm_agent_http.h"
#include <cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>

#define TAG "cap_mcp_client"

/* ---- Limits ---- */
#define MCP_MAX_SERVERS    4
#define MCP_MAX_PER_SERVER 8
#define MCP_MAX_TOOLS      (MCP_MAX_SERVERS * MCP_MAX_PER_SERVER)  /* 32 */

#define MCP_CAP_ID_LEN         72
#define MCP_HOST_LEN           128
#define MCP_PATH_LEN           64
#define MCP_TOOL_NAME_LEN      64
#define MCP_API_KEY_LEN        512  /* JWT Bearer tokens can be 350+ bytes */
#define MCP_DESC_LEN           256
#define MCP_SCHEMA_LEN         512
#define MCP_GROUP_ID_LEN       72

/* ---- Tool entry (filled at discovery time) ---- */
typedef struct {
    char cap_id[MCP_CAP_ID_LEN];
    char host[MCP_HOST_LEN];
    char path[MCP_PATH_LEN];
    char tool_name[MCP_TOOL_NAME_LEN];
    char api_key[MCP_API_KEY_LEN];
    int  use_bearer;
    /* These are used as the .description / .input_schema_json pointer targets */
    char description[MCP_DESC_LEN];
    char input_schema_json[MCP_SCHEMA_LEN];
} mcp_tool_entry_t;

static mcp_tool_entry_t s_tools[MCP_MAX_TOOLS];
static int              s_tool_count = 0;

/* ---- Cap descriptors and groups (one group per server) ---- */
static claw_cap_descriptor_t s_descs[MCP_MAX_TOOLS];

/* Per-server group storage */
typedef struct {
    char group_id[MCP_GROUP_ID_LEN];
    char plugin_name[MCP_GROUP_ID_LEN];
    /* Descriptors for this server start at desc_offset, count = desc_count */
    int  desc_offset;
    int  desc_count;
    claw_cap_group_t group;
} mcp_server_group_t;

static mcp_server_group_t s_groups[MCP_MAX_SERVERS];
static int                s_group_count = 0;

/* ---- Forward declaration ---- */
static int mcp_execute_tool(int idx, const char *input_json,
                                  char **output);

/* ---- Generate 32 per-index execute wrappers via macro ---- */
#define MCP_EXEC_FN(N) \
static int mcp_exec_##N(const char *in, const claw_cap_call_context_t *ctx, \
                               char **out) \
{ (void)ctx; return mcp_execute_tool(N, in, out); }

MCP_EXEC_FN(0)  MCP_EXEC_FN(1)  MCP_EXEC_FN(2)  MCP_EXEC_FN(3)
MCP_EXEC_FN(4)  MCP_EXEC_FN(5)  MCP_EXEC_FN(6)  MCP_EXEC_FN(7)
MCP_EXEC_FN(8)  MCP_EXEC_FN(9)  MCP_EXEC_FN(10) MCP_EXEC_FN(11)
MCP_EXEC_FN(12) MCP_EXEC_FN(13) MCP_EXEC_FN(14) MCP_EXEC_FN(15)
MCP_EXEC_FN(16) MCP_EXEC_FN(17) MCP_EXEC_FN(18) MCP_EXEC_FN(19)
MCP_EXEC_FN(20) MCP_EXEC_FN(21) MCP_EXEC_FN(22) MCP_EXEC_FN(23)
MCP_EXEC_FN(24) MCP_EXEC_FN(25) MCP_EXEC_FN(26) MCP_EXEC_FN(27)
MCP_EXEC_FN(28) MCP_EXEC_FN(29) MCP_EXEC_FN(30) MCP_EXEC_FN(31)

static claw_cap_execute_fn s_exec_fns[MCP_MAX_TOOLS] = {
    mcp_exec_0,  mcp_exec_1,  mcp_exec_2,  mcp_exec_3,
    mcp_exec_4,  mcp_exec_5,  mcp_exec_6,  mcp_exec_7,
    mcp_exec_8,  mcp_exec_9,  mcp_exec_10, mcp_exec_11,
    mcp_exec_12, mcp_exec_13, mcp_exec_14, mcp_exec_15,
    mcp_exec_16, mcp_exec_17, mcp_exec_18, mcp_exec_19,
    mcp_exec_20, mcp_exec_21, mcp_exec_22, mcp_exec_23,
    mcp_exec_24, mcp_exec_25, mcp_exec_26, mcp_exec_27,
    mcp_exec_28, mcp_exec_29, mcp_exec_30, mcp_exec_31,
};

/* MCP-3: auto-increment request id (initialize=1, tools/list=2 are per-connection) */
static int s_req_id = 2;

/* ---- Build MCP extra headers (Content-Type + Accept + Auth) ---- */
/* Returns heap-allocated string; caller must free with rtos_mem_free. */
static char *mcp_build_extra_headers(const char *api_key, int use_bearer)
{
    size_t key_len = api_key ? strlen(api_key) : 0;
    size_t sz = 128 + key_len + 1;
    char *buf = rtos_mem_malloc(sz);
    if (!buf) return NULL;
    const char *base =
        "Content-Type: application/json\r\n"
        "Accept: application/json, text/event-stream\r\n";
    if (use_bearer && key_len) {
        DiagSnPrintf(buf, sz, "%sAuthorization: Bearer %s\r\n", base, api_key);
    } else if (!use_bearer && key_len) {
        DiagSnPrintf(buf, sz, "%sx-api-key: %s\r\n", base, api_key);
    } else {
        DiagSnPrintf(buf, sz, "%s", base);
    }
    return buf;
}

/* ---- Extract JSON from SSE "data: {...}" line or pass through plain JSON ---- */
static const char *mcp_sse_extract_data(const char *buf)
{
    if (!buf) return buf;
    /* SSE line inside body: \ndata: {...} */
    const char *p = strstr(buf, "\ndata: ");
    if (p) return p + 7;
    /* SSE line at buffer start */
    if (strncmp(buf, "data: ", 6) == 0) return buf + 6;
    return buf;  /* plain JSON fallback */
}

/* ---- Build tools/call JSON body ---- */
static char *build_tools_call_body(const char *tool_name, const char *arguments_json)
{
    /* {"jsonrpc":"2.0","method":"tools/call","params":{"name":"<tool>","arguments":<args>},"id":1} */
    const char *args = (arguments_json && arguments_json[0]) ? arguments_json : "{}";
    size_t len = 128 + strlen(tool_name) + strlen(args);
    char *body = rtos_mem_malloc(len);
    if (!body) return NULL;
    int req_id = ++s_req_id;
    DiagSnPrintf(body, len,
             "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\","
             "\"params\":{\"name\":\"%s\",\"arguments\":%s},\"id\":%d}",
             tool_name, args, req_id);
    return body;
}

/* ---- Execute a discovered MCP tool by index ---- */
static int mcp_execute_tool(int idx, const char *input_json,
                                  char **output)
{
    if (idx < 0 || idx >= s_tool_count) {
        claw_cap_set_output(output, "{\"error\":\"invalid tool index %d\"}", idx);
        return RTK_ERR_BADARG;
    }

    mcp_tool_entry_t *t = &s_tools[idx];
    char *body = build_tools_call_body(t->tool_name, input_json);
    if (!body) {
        claw_cap_set_output(output, "{\"error\":\"out of memory\"}");
        return RTK_ERR_NOMEM;
    }

    llm_http_resp_t resp;
    if (llm_http_resp_init(&resp) != 0) {
        rtos_mem_free(body);
        claw_cap_set_output(output, "{\"error\":\"resp_init failed\"}");
        return RTK_FAIL;
    }

    char *hdrs = mcp_build_extra_headers(t->api_key, t->use_bearer);
    if (!hdrs) {
        rtos_mem_free(body);
        llm_http_resp_free(&resp);
        claw_cap_set_output(output, "{\"error\":\"out of memory\"}");
        return RTK_ERR_NOMEM;
    }
    int rc = llm_http_request("POST", t->host, t->path, hdrs, body, strlen(body), NULL, &resp);
    rtos_mem_free(hdrs);
    rtos_mem_free(body);

    if (rc != 0) {
        llm_http_resp_free(&resp);
        claw_cap_set_output(output, "{\"error\":\"http_post failed (%d)\"}", rc);
        return RTK_FAIL;
    }

    /* Parse result.content[0].text */
    int err = RTK_SUCCESS;
    cJSON *root = cJSON_Parse(mcp_sse_extract_data(resp.buf));
    if (!root) {
        claw_cap_set_output(output, "{\"error\":\"invalid JSON response\"}");
        err = RTK_FAIL;
        goto done;
    }

    /* Check for error field */
    cJSON *jerror = cJSON_GetObjectItem(root, "error");
    if (jerror) {
        cJSON *emsg = cJSON_GetObjectItem(jerror, "message");
        if (emsg && cJSON_IsString(emsg)) {
            claw_cap_set_output(output, "{\"error\":\"%s\"}", emsg->valuestring);
        } else {
            claw_cap_set_output(output, "{\"error\":\"MCP server error\"}");
        }
        err = RTK_FAIL;
        goto cleanup;
    }

    {
        cJSON *result   = cJSON_GetObjectItem(root, "result");
        cJSON *content  = result ? cJSON_GetObjectItem(result, "content") : NULL;
        cJSON *item0    = (content && cJSON_IsArray(content)) ? cJSON_GetArrayItem(content, 0) : NULL;
        cJSON *text_obj = item0 ? cJSON_GetObjectItem(item0, "text") : NULL;

        if (text_obj && cJSON_IsString(text_obj)) {
            char *dup = strdup(text_obj->valuestring);
            *output = dup;
            if (!dup) err = RTK_ERR_NOMEM;
        } else {
            /* Fallback: return raw result as string */
            char *raw = result ? cJSON_PrintUnformatted(result) : NULL;
            if (raw) {
                *output = raw;  /* cJSON_Print* already mallocs */
            } else {
                claw_cap_set_output(output, "{\"error\":\"no content in response\"}");
                err = RTK_FAIL;
            }
        }
    }

cleanup:
    cJSON_Delete(root);
done:
    llm_http_resp_free(&resp);
    return err;
}

/* ---- Discover tools from one MCP server ---- */
static void connect_and_discover(int server_idx, const char *name,
                                  const char *host, const char *path,
                                  const char *api_key, int use_bearer)
{
    if (s_group_count >= MCP_MAX_SERVERS) {
        RTK_LOGW(TAG, "Max server groups reached, skipping %s\n", name);
        return;
    }

    /* --- Step 1: initialize handshake (optional, ignore failure) --- */
    {
        const char *init_body =
            "{\"jsonrpc\":\"2.0\",\"method\":\"initialize\","
            "\"params\":{\"protocolVersion\":\"2025-03-26\","
            "\"capabilities\":{},\"clientInfo\":{\"name\":\"ameba_claw\",\"version\":\"1.0\"}},"
            "\"id\":1}";

        char *hdrs = mcp_build_extra_headers(api_key, use_bearer);
        llm_http_resp_t resp;
        if (hdrs && llm_http_resp_init(&resp) == 0) {
            int rc = llm_http_request("POST", host, path, hdrs, init_body, strlen(init_body), NULL, &resp);
            llm_http_resp_free(&resp);
            if (rc != 0)
                RTK_LOGW(TAG, "Server %s: initialize failed (%d), continuing anyway\n", name, rc);
        }
        rtos_mem_free(hdrs);

        /* Send initialized notification (fire-and-forget) */
        const char *notif_body =
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\",\"params\":{}}";
        char *hdrs2 = mcp_build_extra_headers(api_key, use_bearer);
        llm_http_resp_t notif_resp;
        if (hdrs2 && llm_http_resp_init(&notif_resp) == 0) {
            llm_http_request("POST", host, path, hdrs2, notif_body, strlen(notif_body), NULL, &notif_resp);
            llm_http_resp_free(&notif_resp);
        }
        rtos_mem_free(hdrs2);
    }

    /* --- Step 2+3: tools/list with cursor pagination + fill entries --- */
    int grp_idx = s_group_count;
    mcp_server_group_t *sg = &s_groups[grp_idx];
    DiagSnPrintf(sg->group_id,    sizeof(sg->group_id),    "mcp_%s", name);
    DiagSnPrintf(sg->plugin_name, sizeof(sg->plugin_name), "mcp_%s", name);
    sg->desc_offset = s_tool_count;
    sg->desc_count  = 0;

#define MCP_CURSOR_LEN 64
    char cursor[MCP_CURSOR_LEN] = "";
    int  slots_full = 0;

    do {
        /* Build tools/list body; malloc to stay under 128-byte stack-var limit */
        size_t body_sz = 96 + MCP_CURSOR_LEN;
        char *list_buf = rtos_mem_malloc(body_sz);
        if (!list_buf) {
            RTK_LOGE(TAG, "Server %s: OOM for list body\n", name);
            break;
        }
        if (cursor[0])
            DiagSnPrintf(list_buf, body_sz,
                         "{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\","
                         "\"params\":{\"cursor\":\"%s\"},\"id\":2}", cursor);
        else
            DiagSnPrintf(list_buf, body_sz,
                         "{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\","
                         "\"params\":{},\"id\":2}");

        llm_http_resp_t resp;
        if (llm_http_resp_init(&resp) != 0) {
            rtos_mem_free(list_buf);
            RTK_LOGE(TAG, "Server %s: resp_init failed\n", name);
            break;
        }
        char *list_hdrs = mcp_build_extra_headers(api_key, use_bearer);
        if (!list_hdrs) {
            rtos_mem_free(list_buf);
            llm_http_resp_free(&resp);
            RTK_LOGE(TAG, "Server %s: OOM for headers\n", name);
            break;
        }
        int rc = llm_http_request("POST", host, path, list_hdrs, list_buf, strlen(list_buf), NULL, &resp);
        rtos_mem_free(list_hdrs);
        rtos_mem_free(list_buf);

        if (rc != 0) {
            RTK_LOGW(TAG, "Server %s: tools/list failed (%d)\n", name, rc);
            llm_http_resp_free(&resp);
            break;
        }

        cJSON *root = cJSON_Parse(mcp_sse_extract_data(resp.buf));
        llm_http_resp_free(&resp);
        if (!root) {
            RTK_LOGW(TAG, "Server %s: invalid JSON from tools/list\n", name);
            break;
        }

        cJSON *result = cJSON_GetObjectItem(root, "result");
        cJSON *tools  = result ? cJSON_GetObjectItem(result, "tools") : NULL;
        if (!tools || !cJSON_IsArray(tools)) {
            RTK_LOGW(TAG, "Server %s: no tools array in response\n", name);
            cJSON_Delete(root);
            break;
        }

        int num_tools = cJSON_GetArraySize(tools);
        for (int i = 0; i < num_tools; i++) {
            if (s_tool_count >= MCP_MAX_TOOLS) {
                RTK_LOGW(TAG, "Max tool slots reached, stopping discovery for %s\n", name);
                slots_full = 1;
                break;
            }
            if (sg->desc_count >= MCP_MAX_PER_SERVER) {
                RTK_LOGW(TAG, "Server %s: max tools per server (%d) reached\n", name, MCP_MAX_PER_SERVER);
                slots_full = 1;
                break;
            }

            cJSON *tool    = cJSON_GetArrayItem(tools, i);
            cJSON *jname   = cJSON_GetObjectItem(tool, "name");
            cJSON *jdesc   = cJSON_GetObjectItem(tool, "description");
            cJSON *jschema = cJSON_GetObjectItem(tool, "inputSchema");
            if (!jname || !cJSON_IsString(jname)) continue;

            int slot = s_tool_count;
            mcp_tool_entry_t *te = &s_tools[slot];

            DiagSnPrintf(te->cap_id,   sizeof(te->cap_id),   "mcp_%s_%s", name, jname->valuestring);
            strlcpy(te->host,      host,                sizeof(te->host));
            strlcpy(te->path,      path,                sizeof(te->path));
            strlcpy(te->tool_name, jname->valuestring,  sizeof(te->tool_name));
            strlcpy(te->api_key,   api_key,             sizeof(te->api_key));
            te->use_bearer = use_bearer;

            if (jdesc && cJSON_IsString(jdesc))
                strlcpy(te->description, jdesc->valuestring, sizeof(te->description));
            else
                DiagSnPrintf(te->description, sizeof(te->description), "MCP tool: %s", jname->valuestring);

            if (jschema) {
                char *schema_str = cJSON_PrintUnformatted(jschema);
                if (schema_str) {
                    size_t src_len = strlen(schema_str);
                    strlcpy(te->input_schema_json, schema_str, sizeof(te->input_schema_json));
                    if (src_len >= MCP_SCHEMA_LEN)
                        RTK_LOGW(TAG, "tool %s: schema truncated (%u->%u)\n",
                                 jname->valuestring, (unsigned)src_len, MCP_SCHEMA_LEN - 1);
                    rtos_mem_free(schema_str);
                } else {
                    strlcpy(te->input_schema_json, "{\"type\":\"object\"}", sizeof(te->input_schema_json));
                }
            } else {
                strlcpy(te->input_schema_json, "{\"type\":\"object\"}", sizeof(te->input_schema_json));
            }

            claw_cap_descriptor_t *desc = &s_descs[slot];
            desc->id                = te->cap_id;
            desc->name              = te->cap_id;
            desc->family            = sg->group_id;
            desc->description       = te->description;
            desc->kind              = CLAW_CAP_KIND_INVOKE;
            desc->cap_flags         = CLAW_CAP_FLAG_LLM_ACCESS;
            desc->input_schema_json = te->input_schema_json;
            desc->init              = NULL;
            desc->start             = NULL;
            desc->stop              = NULL;
            desc->execute           = s_exec_fns[slot];

            RTK_LOGI(TAG, "  Discovered tool [%d]: %s\n", slot, te->cap_id);
            s_tool_count++;
            sg->desc_count++;
        }

        /* MCP-4: advance cursor for next page */
        cursor[0] = '\0';
        cJSON *next_cur = result ? cJSON_GetObjectItem(result, "nextCursor") : NULL;
        if (!slots_full && next_cur && cJSON_IsString(next_cur))
            strlcpy(cursor, next_cur->valuestring, sizeof(cursor));

        cJSON_Delete(root);

    } while (cursor[0]);

    if (sg->desc_count == 0) {
        RTK_LOGW(TAG, "Server %s: no tools discovered\n", name);
        return;
    }

    /* --- Step 4: register group --- */
    sg->group.group_id         = sg->group_id;
    sg->group.plugin_name      = sg->plugin_name;
    sg->group.version          = "1";
    sg->group.descriptors      = &s_descs[sg->desc_offset];
    sg->group.descriptor_count = (size_t)sg->desc_count;
    sg->group.plugin_ctx       = NULL;
    sg->group.group_init       = NULL;
    sg->group.group_start      = NULL;
    sg->group.group_stop       = NULL;

    int err = claw_cap_register_group(&sg->group);
    if (err != RTK_SUCCESS) {
        RTK_LOGE(TAG, "Server %s: claw_cap_register_group failed (%d)\n", name, err);
        /* Rollback tool count */
        s_tool_count = sg->desc_offset;
        return;
    }

    s_group_count++;
    RTK_LOGI(TAG, "Server %s: registered %d tools as group '%s'\n",
             name, sg->desc_count, sg->group_id);
}

/* ---- Write default config file ---- */
static void write_default_config(const char *filepath)
{
    const char *example =
        "{\n"
        "  \"servers\": []\n"
        "}\n";

    FILE *fp = fopen(filepath, "w");
    if (!fp) {
        RTK_LOGW(TAG, "Could not create default config: %s\n", filepath);
        return;
    }
    fwrite(example, 1, strlen(example), fp);
    fclose(fp);
    RTK_LOGI(TAG, "Created default config: %s\n", filepath);
}

/* ---- Read whole file into malloc'd buffer ---- */
static char *read_file(const char *filepath)
{
    FILE *fp = fopen(filepath, "r");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fsz <= 0 || fsz > 32 * 1024) {
        fclose(fp);
        return NULL;
    }

    char *buf = rtos_mem_malloc((size_t)fsz + 1);
    if (!buf) { fclose(fp); return NULL; }

    size_t br = fread(buf, 1, (size_t)fsz, fp);
    fclose(fp);
    buf[br] = '\0';
    return buf;
}

/* ---- Public init ---- */
int cap_mcp_client_init(const cap_mcp_client_config_t *config)
{
    if (!config || !config->config_dir) return RTK_ERR_BADARG;

    s_tool_count  = 0;
    s_group_count = 0;

    /* Ensure config dir exists */
    mkdir(config->config_dir, 0777);

    /* Build config file path */
    char cfg_path[64];
    DiagSnPrintf(cfg_path, sizeof(cfg_path), "%s/servers.json", config->config_dir);

    /* Try to read; create default if missing */
    char *cfg_text = read_file(cfg_path);
    if (!cfg_text) {
        write_default_config(cfg_path);
        cfg_text = read_file(cfg_path);
    }

    int server_count = 0;

    if (cfg_text) {
        cJSON *root = cJSON_Parse(cfg_text);
        rtos_mem_free(cfg_text);

        if (root) {
            cJSON *servers = cJSON_GetObjectItem(root, "servers");
            if (servers && cJSON_IsArray(servers)) {
                server_count = cJSON_GetArraySize(servers);
                for (int i = 0; i < server_count && i < MCP_MAX_SERVERS; i++) {
                    cJSON *srv  = cJSON_GetArrayItem(servers, i);
                    cJSON *jname    = cJSON_GetObjectItem(srv, "name");
                    cJSON *jhost    = cJSON_GetObjectItem(srv, "host");
                    cJSON *jpath    = cJSON_GetObjectItem(srv, "path");
                    cJSON *jkey     = cJSON_GetObjectItem(srv, "api_key");
                    cJSON *jbearer  = cJSON_GetObjectItem(srv, "use_bearer");

                    if (!jname || !cJSON_IsString(jname) ||
                        !jhost || !cJSON_IsString(jhost) ||
                        !jpath || !cJSON_IsString(jpath)) {
                        RTK_LOGW(TAG, "Server[%d]: missing name/host/path, skipping\n", i);
                        continue;
                    }

                    const char *api_key    = (jkey && cJSON_IsString(jkey)) ? jkey->valuestring : "";
                    int         use_bearer = (jbearer && cJSON_IsBool(jbearer)) ? cJSON_IsTrue(jbearer) : 0;

                    RTK_LOGI(TAG, "Connecting to MCP server '%s' (%s%s) ...\n",
                             jname->valuestring, jhost->valuestring, jpath->valuestring);

                    connect_and_discover(i,
                                         jname->valuestring,
                                         jhost->valuestring,
                                         jpath->valuestring,
                                         api_key,
                                         use_bearer);
                }
            }
            cJSON_Delete(root);
        } else {
            RTK_LOGW(TAG, "Failed to parse servers.json\n");
        }
    } else {
        RTK_LOGW(TAG, "Could not read servers.json\n");
    }

    RTK_LOGI(TAG, "Initialized (%d servers configured, %d tools discovered)\n",
             server_count, s_tool_count);
    return RTK_SUCCESS;
}

/* ---- Lifecycle registration (claw_cap_registry): IO phase (wifi hook) ----
 * MCP discovery runs over HTTP, so it must wait for the network and needs its
 * own task stack (not the wifi_mgr callback stack). on_io registers a wifi
 * on-connected hook that spawns a one-shot discovery task; formerly
 * mcp_client_init_task + on_wifi_connected_for_mcp_client in ameba_claw_main.c.
 * cap_mcp_client registers no group at boot — discovery happens post-connect. */
static void mcp_client_init_task(void *unused)
{
    (void)unused;
    const cap_mcp_client_config_t c = { .config_dir = "vfs:/mcp" };
    cap_mcp_client_init(&c);
    rtos_task_delete(NULL);
}

static void mcp_client_on_wifi_connected(void)
{
    if (rtos_task_create(NULL, "mcp_init", mcp_client_init_task, NULL, 8192, 1) != RTK_SUCCESS)
        RTK_LOGE(TAG, "mcp_init task create failed\n");
}

static void mcp_client_on_io(const claw_config_t *cfg)
{
    (void)cfg;
    claw_wifi_mgr_register_on_connected(mcp_client_on_wifi_connected);
}

CLAW_CAP_REGISTER(mcp_client, {
    .group = "mcp_client",
    .order = 100,
    .on_io = mcp_client_on_io,
});

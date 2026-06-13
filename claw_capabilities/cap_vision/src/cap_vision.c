/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_vision.h"
#include "claw_cap.h"
#include "claw_config.h"
#include "llm_agent_http.h"
#include "platform_stdlib.h"
#include <cJSON.h>
#include <mbedtls/base64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TAG "cap_vision"

#define VISION_DEFAULT_MAX_BYTES  (2 * 1024 * 1024)  /* 2 MB */

/* init-time overrides: non-empty strings take priority over claw_config values.
 * Empty string means "use claw_config / compile-time default".
 * max_image_bytes drives heap sizing so it is fixed at init time. */
static struct {
    char   model[64];
    char   api_key[128];
    char   base_url[128];
    char   api_path[128];
    size_t max_image_bytes;
} s_cfg;

/* ---- MIME from extension ------------------------------------------------- */

static const char *mime_from_path(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "image/jpeg";
    dot++;
    if (strcmp(dot, "jpg") == 0 || strcmp(dot, "jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, "png")  == 0) return "image/png";
    if (strcmp(dot, "gif")  == 0) return "image/gif";
    if (strcmp(dot, "webp") == 0) return "image/webp";
    return "image/jpeg"; /* safe fallback for unknown image types */
}

/* ---- Tool execute: vision_describe --------------------------------------- */

static int cap_vision_execute(const char *input_json,
                               const claw_cap_call_context_t *ctx,
                               char **output)
{
    (void)ctx;

    /* Resolve vision config once per call.
     * Three-level fallback (highest priority first):
     *   1. s_cfg init-time override (caller-supplied via cap_vision_init)
     *   2. claw_config vision section (runtime-editable via WebUI)
     *   3. compile-time defaults / llm.api_key */
    const claw_config_t *vcfg = claw_config_get();
    const char *vis_model = s_cfg.model[0]    ? s_cfg.model
                          : vcfg->vision.model[0]    ? vcfg->vision.model
                          : CLAW_CONFIG_DEFAULT_VISION_MODEL;
    const char *api_key   = s_cfg.api_key[0]  ? s_cfg.api_key
                          : vcfg->vision.api_key[0]  ? vcfg->vision.api_key
                          : vcfg->llm.api_key;
    const char *api_host  = s_cfg.base_url[0] ? s_cfg.base_url
                          : vcfg->vision.base_url[0] ? vcfg->vision.base_url
                          : CLAW_CONFIG_DEFAULT_VISION_BASE_URL;
    const char *api_path  = s_cfg.api_path[0] ? s_cfg.api_path
                          : vcfg->vision.api_path[0] ? vcfg->vision.api_path
                          : CLAW_CONFIG_DEFAULT_VISION_API_PATH;

    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        claw_cap_set_output(output, "{\"error\":\"invalid input JSON\"}");
        return RTK_FAIL;
    }

    cJSON *path_j    = cJSON_GetObjectItem(root, "path");
    cJSON *prompt_j  = cJSON_GetObjectItem(root, "prompt");

    const char *vfs_path = (path_j   && cJSON_IsString(path_j))   ? path_j->valuestring   : NULL;
    const char *prompt   = (prompt_j && cJSON_IsString(prompt_j))  ? prompt_j->valuestring : "请描述这张图片的内容";

    if (!vfs_path || !vfs_path[0]) {
        cJSON_Delete(root);
        claw_cap_set_output(output, "{\"error\":\"path is required\"}");
        return RTK_FAIL;
    }

    /* ---- Read file into heap --------------------------------------------- */
    struct stat st;
    if (stat(vfs_path, &st) != 0 || st.st_size <= 0) {
        cJSON_Delete(root);
        claw_cap_set_output(output, "{\"error\":\"file not found or empty\"}");
        return RTK_FAIL;
    }

    size_t file_sz = (size_t)st.st_size;
    if (file_sz > s_cfg.max_image_bytes) {
        char buf[128];
        DiagSnPrintf(buf, sizeof(buf),
                     "{\"error\":\"image too large (%u KB, max %u KB)\"}",
                     (unsigned)(file_sz / 1024),
                     (unsigned)(s_cfg.max_image_bytes / 1024));
        cJSON_Delete(root);
        claw_cap_set_output(output, buf);
        return RTK_FAIL;
    }

    uint8_t *img_buf = (uint8_t *)malloc(file_sz);
    if (!img_buf) {
        cJSON_Delete(root);
        claw_cap_set_output(output, "{\"error\":\"out of memory reading image\"}");
        return RTK_ERR_NOMEM;
    }

    FILE *fp = fopen(vfs_path, "rb");
    if (!fp) {
        free(img_buf);
        cJSON_Delete(root);
        claw_cap_set_output(output, "{\"error\":\"cannot open file\"}");
        return RTK_FAIL;
    }
    size_t n = fread(img_buf, 1, file_sz, fp);
    fclose(fp);

    if (n != file_sz) {
        free(img_buf);
        cJSON_Delete(root);
        claw_cap_set_output(output, "{\"error\":\"file read incomplete\"}");
        return RTK_FAIL;
    }

    /* ---- Base64 encode --------------------------------------------------- */
    /* mbedtls_base64_encode: output length = ceil(n/3)*4 + 1 */
    size_t b64_len = ((file_sz + 2) / 3) * 4 + 1;
    char  *b64_buf = (char *)malloc(b64_len);
    if (!b64_buf) {
        free(img_buf);
        cJSON_Delete(root);
        claw_cap_set_output(output, "{\"error\":\"out of memory for base64\"}");
        return RTK_ERR_NOMEM;
    }

    size_t out_len = 0;
    int b64_ret = mbedtls_base64_encode(
        (unsigned char *)b64_buf, b64_len, &out_len,
        img_buf, file_sz);
    free(img_buf);

    if (b64_ret != 0) {
        free(b64_buf);
        cJSON_Delete(root);
        claw_cap_set_output(output, "{\"error\":\"base64 encoding failed\"}");
        return RTK_FAIL;
    }
    b64_buf[out_len] = '\0';

    /* ---- Build data URI: "data:<mime>;base64,<b64>" ---------------------- */
    const char *mime = mime_from_path(vfs_path);
    size_t uri_len = strlen("data:") + strlen(mime) + strlen(";base64,") + out_len + 1;
    char  *data_uri = (char *)malloc(uri_len);
    if (!data_uri) {
        free(b64_buf);
        cJSON_Delete(root);
        claw_cap_set_output(output, "{\"error\":\"out of memory for data uri\"}");
        return RTK_ERR_NOMEM;
    }
    DiagSnPrintf(data_uri, uri_len, "data:%s;base64,%s", mime, b64_buf);
    free(b64_buf);

    /* ---- Build GLM vision request ----------------------------------------
     * OpenAI-compatible multimodal format:
     * messages: [{role:"user", content:[
     *   {type:"image_url", image_url:{url:"data:..."}},
     *   {type:"text", text:"<prompt>"}
     * ]}]
     * ----------------------------------------------------------------------- */
    cJSON *req      = cJSON_CreateObject();
    cJSON *msgs     = cJSON_CreateArray();
    cJSON *user_msg = cJSON_CreateObject();
    cJSON *content  = cJSON_CreateArray();
    cJSON *img_part = cJSON_CreateObject();
    cJSON *img_url  = cJSON_CreateObject();
    cJSON *txt_part = cJSON_CreateObject();

    if (!req || !msgs || !user_msg || !content ||
        !img_part || !img_url || !txt_part) {
        cJSON_Delete(req); free(data_uri); cJSON_Delete(root);
        claw_cap_set_output(output, "{\"error\":\"out of memory building request\"}");
        return RTK_ERR_NOMEM;
    }

    cJSON_AddStringToObject(req, "model", vis_model);
    cJSON_AddFalseToObject(req, "stream");

    /* image_url part */
    cJSON_AddStringToObject(img_url,  "url",  data_uri);
    cJSON_AddStringToObject(img_part, "type", "image_url");
    cJSON_AddItemToObject(img_part,   "image_url", img_url);
    cJSON_AddItemToArray(content, img_part);

    /* text part */
    cJSON_AddStringToObject(txt_part, "type", "text");
    cJSON_AddStringToObject(txt_part, "text", prompt);
    cJSON_AddItemToArray(content, txt_part);

    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddItemToObject(user_msg,   "content", content);
    cJSON_AddItemToArray(msgs, user_msg);
    cJSON_AddItemToObject(req, "messages", msgs);

    free(data_uri);

    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    cJSON_Delete(root);
    if (!req_str) {
        claw_cap_set_output(output, "{\"error\":\"out of memory serializing request\"}");
        return RTK_ERR_NOMEM;
    }

    /* ---- HTTP POST ------------------------------------------------------- */
    llm_http_resp_t resp;
    if (llm_http_resp_init(&resp) != 0) {
        free(req_str);
        claw_cap_set_output(output, "{\"error\":\"out of memory for http response\"}");
        return RTK_ERR_NOMEM;
    }

    RTK_LOGI(TAG, "POST vision host=%s path=%s model=%s img=%u bytes\n",
             api_host, api_path, vis_model, (unsigned)file_sz);

    int http_ret = llm_http_post_bearer(api_host, api_path,
                                         req_str, strlen(req_str),
                                         api_key, &resp);
    free(req_str);

    if (http_ret != 0) {
        llm_http_resp_free(&resp);
        claw_cap_set_output(output, "{\"error\":\"vision API request failed\"}");
        return RTK_FAIL;
    }

    /* ---- Parse response -------------------------------------------------- */
    cJSON *resp_root = cJSON_Parse(resp.buf);
    llm_http_resp_free(&resp);

    if (!resp_root) {
        claw_cap_set_output(output, "{\"error\":\"invalid vision API response\"}");
        return RTK_FAIL;
    }

    /* Check API error */
    cJSON *err_j = cJSON_GetObjectItem(resp_root, "error");
    if (err_j) {
        cJSON *msg_j = cJSON_GetObjectItem(err_j, "message");
        const char *errmsg = (msg_j && cJSON_IsString(msg_j))
                             ? msg_j->valuestring : "unknown API error";
        char errbuf[256];
        DiagSnPrintf(errbuf, sizeof(errbuf), "{\"error\":\"%s\"}", errmsg);
        claw_cap_set_output(output, errbuf);
        cJSON_Delete(resp_root);
        return RTK_FAIL;
    }

    /* Extract text from choices[0].message.content */
    const char *description = NULL;
    cJSON *choices = cJSON_GetObjectItem(resp_root, "choices");
    cJSON *ch0     = choices ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = ch0    ? cJSON_GetObjectItem(ch0, "message") : NULL;
    cJSON *content_j = message ? cJSON_GetObjectItem(message, "content") : NULL;

    if (content_j && cJSON_IsString(content_j)) {
        description = content_j->valuestring;
    }

    if (!description || !description[0]) {
        cJSON_Delete(resp_root);
        claw_cap_set_output(output, "{\"error\":\"empty vision response\"}");
        return RTK_FAIL;
    }

    /* Return as JSON: {"description": "..."} */
    cJSON *out_j = cJSON_CreateObject();
    if (!out_j) {
        cJSON_Delete(resp_root);
        claw_cap_set_output(output, "{\"error\":\"out of memory\"}");
        return RTK_ERR_NOMEM;
    }
    cJSON_AddStringToObject(out_j, "description", description);
    *output = cJSON_PrintUnformatted(out_j);
    cJSON_Delete(out_j);
    cJSON_Delete(resp_root);

    RTK_LOGI(TAG, "vision done: %u chars description\n",
             (unsigned)(*output ? strlen(*output) : 0));

    return *output ? RTK_SUCCESS : RTK_ERR_NOMEM;
}

/* ---- Cap group ----------------------------------------------------------- */

static const claw_cap_descriptor_t s_caps[] = {
    {
        .id          = "vision_describe",
        .name        = "vision_describe",
        .family      = "vision",
        .description =
            "Analyse an image file and return a natural-language description. "
            "Use when a user sends an image or asks what is in a photo. "
            "path: VFS path to the image (e.g. vfs:/inbox/wechat/.../photo.jpg). "
            "prompt: optional question about the image (default: describe contents).",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"path\":{\"type\":\"string\","
              "\"description\":\"VFS path to the image file\"},"
            "\"prompt\":{\"type\":\"string\","
              "\"description\":\"Question or instruction about the image (optional)\"}"
            "},"
            "\"required\":[\"path\"]}",
        .execute     = cap_vision_execute,
    },
};

static const claw_cap_group_t s_group = {
    .group_id         = "vision",
    .plugin_name      = "cap_vision",
    .version          = "1",
    .descriptors      = s_caps,
    .descriptor_count = 1,
};

/* ---- Public API ---------------------------------------------------------- */

int cap_vision_init(const cap_vision_config_t *cfg)
{
    /* Store caller-supplied overrides in s_cfg (NOT in claw_config — that would
     * cause unintended flash writes if claw_config_save() is called later).
     * Empty string = "not overridden; fall through to claw_config at call time". */
    s_cfg.model[0]    = '\0';
    s_cfg.api_key[0]  = '\0';
    s_cfg.base_url[0] = '\0';
    s_cfg.api_path[0] = '\0';
    if (cfg) {
        if (cfg->model    && cfg->model[0])    strlcpy(s_cfg.model,    cfg->model,    sizeof(s_cfg.model));
        if (cfg->api_key  && cfg->api_key[0])  strlcpy(s_cfg.api_key,  cfg->api_key,  sizeof(s_cfg.api_key));
        if (cfg->base_url && cfg->base_url[0]) strlcpy(s_cfg.base_url, cfg->base_url, sizeof(s_cfg.base_url));
        if (cfg->api_path && cfg->api_path[0]) strlcpy(s_cfg.api_path, cfg->api_path, sizeof(s_cfg.api_path));
    }
    s_cfg.max_image_bytes = (cfg && cfg->max_image_bytes) ? cfg->max_image_bytes : VISION_DEFAULT_MAX_BYTES;

    int err = claw_cap_register_group(&s_group);
    if (err != RTK_SUCCESS) {
        RTK_LOGE(TAG, "cap_register_group failed: %d\n", err);
        return err;
    }

    /* Log resolved values (apply fallback chain for the log message too). */
    const claw_config_t *vcfg = claw_config_get();
    const char *log_model = s_cfg.model[0]    ? s_cfg.model
                          : vcfg->vision.model[0]    ? vcfg->vision.model
                          : CLAW_CONFIG_DEFAULT_VISION_MODEL;
    const char *log_host  = s_cfg.base_url[0] ? s_cfg.base_url
                          : vcfg->vision.base_url[0] ? vcfg->vision.base_url
                          : CLAW_CONFIG_DEFAULT_VISION_BASE_URL;
    RTK_LOGI(TAG, "initialized (model=%s host=%s max=%uKB)\n",
             log_model, log_host, (unsigned)(s_cfg.max_image_bytes / 1024));
    return RTK_SUCCESS;
}

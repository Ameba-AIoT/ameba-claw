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

/* Max images per vision_describe call. 2 enables compare/diff tasks
 * (prev vs current) while bounding the peak heap: all images' base64 live in
 * the request body at once, so N is intentionally small. */
#define VISION_MAX_IMAGES  2

/* Standing coordinate protocol appended to every vision request.
 * Rationale: vision_describe otherwise returns free text with no positions, so
 * an agent that wants to draw boxes / annotate an image has to guess pixel
 * coordinates -> boxes land in the wrong place. Photo resolution, screen size
 * and the on-screen image rect all differ, so we force NORMALIZED (0..1) boxes
 * that are resolution-independent; the agent then maps them onto the actual
 * drawn image rectangle (see display.md "annotate a photo").
 * The clause is conditional ("when locating"), so pure describe calls are
 * unaffected -- the model only emits the JSON block when asked to locate.
 * Used by: cap_vision_execute (both OpenAI and Anthropic request paths). */
#define VISION_COORD_HINT \
    "\n\n[定位约定] 当被要求定位物体/画框/标注位置时，除文字描述外，" \
    "再输出一个 JSON 数组，每个目标一项：" \
    "[{\"label\":\"名称\",\"box\":[x0,y0,x1,y1]}]。" \
    "坐标必须是归一化值(0~1)：原点在图片左上角，x 向右、y 向下；" \
    "(x0,y0)=左上角，(x1,y1)=右下角。禁止输出像素值。" \
    "只做纯描述时忽略本段。"

/* init-time overrides: non-empty strings take priority over claw_config values.
 * Empty string means "use claw_config / compile-time default".
 * max_image_bytes drives heap sizing so it is fixed at init time. */
static struct {
    char   model[64];
    char   api_key[128];
    char   base_url[128];
    char   api_path[128];
    char   api_type[16]; /* "openai" or "anthropic" */
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

/* ---- URL helpers --------------------------------------------------------- */

/* Parse a full URL like "https://host/path" into host (buf_host) and
 * path (buf_path).  Strips the scheme and port-less host only.
 * Path always starts with '/'; if the URL ends at the host, path = "/". */
static void parse_url_host_path(const char *url,
                                const char *endpoint_suffix,
                                char *buf_host, size_t host_sz,
                                char *buf_path, size_t path_sz)
{
    /* Skip scheme: "https://" or "http://" */
    const char *p = url;
    const char *ss = strstr(p, "://");
    if (ss) p = ss + 3;

    /* Find end of host (first '/' after scheme) */
    const char *slash = strchr(p, '/');
    if (!slash) {
        strlcpy(buf_host, p, host_sz);
        strlcpy(buf_path, "/", path_sz);
    } else {
        size_t hlen = (size_t)(slash - p);
        if (hlen >= host_sz) hlen = host_sz - 1;
        memcpy(buf_host, p, hlen);
        buf_host[hlen] = '\0';
        strlcpy(buf_path, slash, path_sz);
    }

    /* Append endpoint_suffix unless the path already ends with it.
     * This makes the result correct regardless of whether api_url has a
     * trailing slash (e.g. "/v1" and "/v1/" both become "/v1/messages"). */
    size_t plen = strlen(buf_path);
    size_t slen = endpoint_suffix ? strlen(endpoint_suffix) : 0;
    if (slen == 0) return;
    int already = (plen >= slen &&
                   strcmp(buf_path + plen - slen, endpoint_suffix) == 0);
    if (!already) {
        if (buf_path[plen - 1] != '/') {
            strlcat(buf_path, "/", path_sz);
        }
        strlcat(buf_path, endpoint_suffix, path_sz);
    }
}

/* ---- VFS path normalization --------------------------------------------- */

/* Ensure path has "vfs:" prefix. Writes to buf (size buf_sz) and returns it. */
static const char *normalize_vfs_path(const char *path, char *buf, size_t buf_sz)
{
    if (strncmp(path, "vfs:", 4) == 0) return path;
    DiagSnPrintf(buf, buf_sz, "vfs:%s", path);
    return buf;
}

/* ---- Append one image as a multimodal content part ---------------------- */

/* Read the image at raw_path, base64-encode it, and append the backend-
 * appropriate content part to `content`:
 *   Anthropic -> {type:"image", source:{type:"base64", media_type, data}}
 *   OpenAI     -> {type:"image_url", image_url:{url:"data:<mime>;base64,..."}}
 * Returns 0 on success. On failure returns -1, writes a JSON error object into
 * `err`, appends nothing to `content`, and leaks no buffers. */
static int append_image_part(cJSON *content, const char *raw_path,
                             int is_anthropic, size_t max_bytes,
                             char *err, size_t errsz)
{
    char path_buf[256];
    const char *vfs_path = normalize_vfs_path(raw_path, path_buf, sizeof(path_buf));

    struct stat st;
    if (stat(vfs_path, &st) != 0 || st.st_size <= 0) {
        DiagSnPrintf(err, errsz, "{\"error\":\"file not found or empty: %s\"}", raw_path);
        return -1;
    }
    size_t file_sz = (size_t)st.st_size;
    if (file_sz > max_bytes) {
        DiagSnPrintf(err, errsz,
                     "{\"error\":\"image too large (%u KB, max %u KB)\"}",
                     (unsigned)(file_sz / 1024), (unsigned)(max_bytes / 1024));
        return -1;
    }

    uint8_t *img_buf = (uint8_t *)malloc(file_sz);
    if (!img_buf) {
        DiagSnPrintf(err, errsz, "{\"error\":\"out of memory reading image\"}");
        return -1;
    }
    FILE *fp = fopen(vfs_path, "rb");
    if (!fp) {
        free(img_buf);
        DiagSnPrintf(err, errsz, "{\"error\":\"cannot open file\"}");
        return -1;
    }
    size_t n = fread(img_buf, 1, file_sz, fp);
    fclose(fp);
    if (n != file_sz) {
        free(img_buf);
        DiagSnPrintf(err, errsz, "{\"error\":\"file read incomplete\"}");
        return -1;
    }

    /* mbedtls_base64_encode: output length = ceil(n/3)*4 + 1 */
    size_t b64_len = ((file_sz + 2) / 3) * 4 + 1;
    char  *b64_buf = (char *)malloc(b64_len);
    if (!b64_buf) {
        free(img_buf);
        DiagSnPrintf(err, errsz, "{\"error\":\"out of memory for base64\"}");
        return -1;
    }
    size_t out_len = 0;
    int b64_ret = mbedtls_base64_encode((unsigned char *)b64_buf, b64_len,
                                        &out_len, img_buf, file_sz);
    free(img_buf);
    if (b64_ret != 0) {
        free(b64_buf);
        DiagSnPrintf(err, errsz, "{\"error\":\"base64 encoding failed\"}");
        return -1;
    }
    b64_buf[out_len] = '\0';

    const char *mime = mime_from_path(vfs_path);

    cJSON *img_part = cJSON_CreateObject();
    if (!img_part) {
        free(b64_buf);
        DiagSnPrintf(err, errsz, "{\"error\":\"out of memory building request\"}");
        return -1;
    }

    if (is_anthropic) {
        cJSON *src = cJSON_CreateObject();
        if (!src) {
            cJSON_Delete(img_part); free(b64_buf);
            DiagSnPrintf(err, errsz, "{\"error\":\"out of memory building request\"}");
            return -1;
        }
        cJSON_AddStringToObject(src,      "type",       "base64");
        cJSON_AddStringToObject(src,      "media_type", mime);
        cJSON_AddStringToObject(src,      "data",       b64_buf);
        cJSON_AddStringToObject(img_part, "type",       "image");
        cJSON_AddItemToObject(img_part,   "source",     src);
    } else {
        size_t uri_len = strlen("data:") + strlen(mime) + strlen(";base64,") + out_len + 1;
        char  *data_uri = (char *)malloc(uri_len);
        if (!data_uri) {
            cJSON_Delete(img_part); free(b64_buf);
            DiagSnPrintf(err, errsz, "{\"error\":\"out of memory for data uri\"}");
            return -1;
        }
        DiagSnPrintf(data_uri, uri_len, "data:%s;base64,%s", mime, b64_buf);
        cJSON *img_url = cJSON_CreateObject();
        if (!img_url) {
            free(data_uri); cJSON_Delete(img_part); free(b64_buf);
            DiagSnPrintf(err, errsz, "{\"error\":\"out of memory building request\"}");
            return -1;
        }
        cJSON_AddStringToObject(img_url,  "url",       data_uri);
        cJSON_AddStringToObject(img_part, "type",      "image_url");
        cJSON_AddItemToObject(img_part,   "image_url", img_url);
        free(data_uri);
    }
    free(b64_buf);
    cJSON_AddItemToArray(content, img_part);
    return 0;
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

    /* Determine whether vision has been explicitly configured by the user.
     * If base_url is still the compile-time default AND api_key is empty,
     * the user has not configured a dedicated vision endpoint — inherit from
     * llm.api_url so requests use the same proxy as the main LLM. */
    const char *api_type  = s_cfg.api_type[0]        ? s_cfg.api_type
                          : vcfg->vision.api_type[0]  ? vcfg->vision.api_type
                          : "openai";
    int is_anthropic = (strcmp(api_type, "anthropic") == 0);

    int vision_is_default = !s_cfg.base_url[0] &&
        (strcmp(vcfg->vision.base_url, CLAW_CONFIG_DEFAULT_VISION_BASE_URL) == 0) &&
        !vcfg->vision.api_key[0] && vcfg->llm.api_url[0];
    char derived_host[128] = {0};
    char derived_path[256] = {0};
    int  host_from_llm = 0;
    if (vision_is_default) {
        parse_url_host_path(vcfg->llm.api_url,
                            is_anthropic ? "messages" : "chat/completions",
                            derived_host, sizeof(derived_host),
                            derived_path, sizeof(derived_path));
        host_from_llm = derived_host[0] != '\0';
    }

    const char *vis_model = s_cfg.model[0]     ? s_cfg.model
                          : !vision_is_default ? vcfg->vision.model
                          : vcfg->llm.model[0] ? vcfg->llm.model
                          : CLAW_CONFIG_DEFAULT_VISION_MODEL;
    const char *api_key   = s_cfg.api_key[0]         ? s_cfg.api_key
                          : vcfg->vision.api_key[0]   ? vcfg->vision.api_key
                          : vcfg->llm.api_key;
    const char *api_host  = s_cfg.base_url[0]        ? s_cfg.base_url
                          : !vision_is_default        ? vcfg->vision.base_url
                          : host_from_llm             ? derived_host
                          : CLAW_CONFIG_DEFAULT_VISION_BASE_URL;
    const char *api_path  = s_cfg.api_path[0]        ? s_cfg.api_path
                          : !vision_is_default        ? vcfg->vision.api_path
                          : (host_from_llm && derived_path[0]) ? derived_path
                          : CLAW_CONFIG_DEFAULT_VISION_API_PATH;

    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        claw_cap_set_output(output, "{\"error\":\"invalid input JSON\"}");
        return RTK_FAIL;
    }

    cJSON *prompt_j  = cJSON_GetObjectItem(root, "prompt");
    const char *prompt = (prompt_j && cJSON_IsString(prompt_j)) ? prompt_j->valuestring
                                                                : "请描述这张图片的内容";

    /* Collect 1..VISION_MAX_IMAGES image paths. Accept either `paths` (array,
     * for multi-image compare/diff) or the legacy single `path` string.
     * Pointers alias into `root`, which stays alive until the request is built.*/
    const char *raw_paths[VISION_MAX_IMAGES];
    int npaths = 0;
    cJSON *paths_j = cJSON_GetObjectItem(root, "paths");
    if (paths_j && cJSON_IsArray(paths_j)) {
        cJSON *it;
        cJSON_ArrayForEach(it, paths_j) {
            if (!cJSON_IsString(it) || !it->valuestring[0]) continue;
            if (npaths >= VISION_MAX_IMAGES) {
                cJSON_Delete(root);
                claw_cap_set_output(output, "{\"error\":\"too many images (max %d)\"}",
                                    VISION_MAX_IMAGES);
                return RTK_FAIL;
            }
            raw_paths[npaths++] = it->valuestring;
        }
    } else {
        cJSON *path_j = cJSON_GetObjectItem(root, "path");
        if (path_j && cJSON_IsString(path_j) && path_j->valuestring[0]) {
            raw_paths[npaths++] = path_j->valuestring;
        }
    }

    if (npaths == 0) {
        cJSON_Delete(root);
        claw_cap_set_output(output, "{\"error\":\"path or paths is required\"}");
        return RTK_FAIL;
    }

    /* Append the normalized-coordinate protocol so localization/annotation
     * requests yield resolution-independent boxes. Falls back to the bare
     * prompt if allocation fails (coords just won't be requested). */
    char *full_prompt = NULL;
    {
        size_t fp_len = strlen(prompt) + strlen(VISION_COORD_HINT) + 1;
        full_prompt = (char *)malloc(fp_len);
        if (full_prompt) {
            DiagSnPrintf(full_prompt, fp_len, "%s%s", prompt, VISION_COORD_HINT);
            prompt = full_prompt;
        }
    }

    /* ---- Build vision request -------------------------------------------- */
    cJSON *req      = cJSON_CreateObject();
    cJSON *msgs     = cJSON_CreateArray();
    cJSON *user_msg = cJSON_CreateObject();
    cJSON *content  = cJSON_CreateArray();
    cJSON *txt_part = cJSON_CreateObject();

    if (!req || !msgs || !user_msg || !content || !txt_part) {
        cJSON_Delete(req); cJSON_Delete(msgs); cJSON_Delete(user_msg);
        cJSON_Delete(content); cJSON_Delete(txt_part);
        free(full_prompt); cJSON_Delete(root);
        claw_cap_set_output(output, "{\"error\":\"out of memory building request\"}");
        return RTK_ERR_NOMEM;
    }

    cJSON_AddStringToObject(req, "model", vis_model);
    cJSON_AddFalseToObject(req, "stream");
    if (is_anthropic) {
        cJSON_AddNumberToObject(req, "max_tokens", 1024); /* required by Anthropic API */
    }

    /* Images first, text last — required by Anthropic, accepted by
     * OpenAI-compatible backends. Each image reads + base64s independently. */
    for (int i = 0; i < npaths; i++) {
        char err[160];
        if (append_image_part(content, raw_paths[i], is_anthropic,
                              s_cfg.max_image_bytes, err, sizeof(err)) != 0) {
            cJSON_Delete(req); cJSON_Delete(msgs); cJSON_Delete(user_msg);
            cJSON_Delete(content); cJSON_Delete(txt_part);
            free(full_prompt); cJSON_Delete(root);
            claw_cap_set_output(output, "%s", err);
            return RTK_FAIL;
        }
    }

    cJSON_AddStringToObject(txt_part, "type", "text");
    cJSON_AddStringToObject(txt_part, "text", prompt);
    cJSON_AddItemToArray(content, txt_part);
    free(full_prompt);

    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddItemToObject(user_msg,   "content", content);
    cJSON_AddItemToArray(msgs, user_msg);
    cJSON_AddItemToObject(req, "messages", msgs);

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

    RTK_LOGI(TAG, "POST vision host=%s path=%s model=%s imgs=%d type=%s\n",
             api_host, api_path, vis_model, npaths, api_type);

    int http_ret = is_anthropic
        ? llm_http_post(api_host, api_path, req_str, strlen(req_str), api_key, &resp)
        : llm_http_post_bearer(api_host, api_path, req_str, strlen(req_str), api_key, &resp);
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

    /* Extract text — handle both OpenAI and Anthropic response shapes */
    const char *description = NULL;
    if (is_anthropic) {
        /* Anthropic: {"content":[{"type":"text","text":"..."},...]} */
        cJSON *content_arr = cJSON_GetObjectItem(resp_root, "content");
        cJSON *blk;
        cJSON_ArrayForEach(blk, content_arr) {
            cJSON *btype = cJSON_GetObjectItem(blk, "type");
            cJSON *btext = cJSON_GetObjectItem(blk, "text");
            if (btype && cJSON_IsString(btype) &&
                strcmp(btype->valuestring, "text") == 0 &&
                btext && cJSON_IsString(btext)) {
                description = btext->valuestring;
                break;
            }
        }
    } else {
        /* OpenAI: {"choices":[{"message":{"content":"..."}}]} */
        cJSON *choices  = cJSON_GetObjectItem(resp_root, "choices");
        cJSON *ch0      = choices ? cJSON_GetArrayItem(choices, 0) : NULL;
        cJSON *message  = ch0    ? cJSON_GetObjectItem(ch0, "message") : NULL;
        cJSON *content_j = message ? cJSON_GetObjectItem(message, "content") : NULL;
        if (content_j && cJSON_IsString(content_j)) {
            description = content_j->valuestring;
        }
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
            "Analyse one or two image files and return a natural-language answer. "
            "Use when a user sends an image or asks what is in a photo. "
            "path: VFS path to a single image (e.g. vfs:/inbox/wechat/.../photo.jpg). "
            "paths: array of up to 2 VFS paths — pass two to COMPARE images "
            "(e.g. prev vs current: ask for added/removed/moved objects); much "
            "more reliable than describing each separately. Use either path or "
            "paths, not both. "
            "prompt: optional question about the image(s) (default: describe contents). "
            "RETURNS: JSON object {\"description\":\"<the model's text answer>\"} "
            "(or {\"error\":\"...\"}). The answer is ALWAYS in the `description` "
            "field -- read that field, not the top-level object. "
            "LOCATING OBJECTS FOR ANNOTATION: to draw boxes/labels over an image, "
            "ask (in prompt) to locate the objects; the model then embeds, INSIDE "
            "the `description` text, a JSON array "
            "[{\"label\":..,\"box\":[x0,y0,x1,y1]}] -- so parse that array out of "
            "the `description` string. Coords are NORMALIZED (0..1, origin=top-left "
            "of the IMAGE, x right / y down; x0,y0=top-left, x1,y1=bottom-right). "
            "They are resolution-independent: map each to screen pixels against the "
            "rectangle where you actually drew the image (draw_image returns its "
            "real on-screen w,h) -- e.g. px = img_x + x0*draw_w, py = img_y + "
            "y0*draw_h. Never treat the coords as pixels of the original photo.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"path\":{\"type\":\"string\","
              "\"description\":\"VFS path to a single image file\"},"
            "\"paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
              "\"maxItems\":2,"
              "\"description\":\"Up to 2 VFS image paths to compare; use instead of path\"},"
            "\"prompt\":{\"type\":\"string\","
              "\"description\":\"Question or instruction about the image(s) (optional)\"}"
            "}}",
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
    s_cfg.api_type[0] = '\0';
    if (cfg) {
        if (cfg->model    && cfg->model[0])    strlcpy(s_cfg.model,    cfg->model,    sizeof(s_cfg.model));
        if (cfg->api_key  && cfg->api_key[0])  strlcpy(s_cfg.api_key,  cfg->api_key,  sizeof(s_cfg.api_key));
        if (cfg->base_url && cfg->base_url[0]) strlcpy(s_cfg.base_url, cfg->base_url, sizeof(s_cfg.base_url));
        if (cfg->api_path && cfg->api_path[0]) strlcpy(s_cfg.api_path, cfg->api_path, sizeof(s_cfg.api_path));
        if (cfg->api_type && cfg->api_type[0]) strlcpy(s_cfg.api_type, cfg->api_type, sizeof(s_cfg.api_type));
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

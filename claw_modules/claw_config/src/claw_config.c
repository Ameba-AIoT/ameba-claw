#include "claw_config.h"
#include "claw_compat.h"
#include <cJSON.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ameba_soc.h"

#define CONFIG_FILE "vfs:claw_config.json"
#define TAG "claw_config"

static claw_config_t s_cfg;
static bool          s_initialized = false;

/* ---- On-save callbacks ---- */

#define MAX_ON_SAVE_CBS 4
static claw_config_on_save_fn_t s_on_save_cbs[MAX_ON_SAVE_CBS];
static int                      s_on_save_count = 0;

void claw_config_register_on_save(claw_config_on_save_fn_t cb)
{
    if (!cb) return;
    for (int i = 0; i < s_on_save_count; i++) {
        if (s_on_save_cbs[i] == cb) return; /* duplicate */
    }
    if (s_on_save_count < MAX_ON_SAVE_CBS)
        s_on_save_cbs[s_on_save_count++] = cb;
}

static void notify_on_save(void)
{
    for (int i = 0; i < s_on_save_count; i++)
        s_on_save_cbs[i]();
}

/* ---- Defaults ---- */

static void apply_defaults(claw_config_t *c)
{
    _memset(c, 0, sizeof(*c));

    /* wifi: not configured */
    strlcpy(c->wifi.security_type, "WPA2", sizeof(c->wifi.security_type));
    c->wifi.configured = false;

    /* softap */
    strlcpy(c->softap.ssid,     CLAW_CONFIG_DEFAULT_SOFTAP_SSID,     sizeof(c->softap.ssid));
    strlcpy(c->softap.password, CLAW_CONFIG_DEFAULT_SOFTAP_PASSWORD, sizeof(c->softap.password));
    c->softap.channel = CLAW_CONFIG_DEFAULT_SOFTAP_CHANNEL;

    /* llm */
    strlcpy(c->llm.api_key,  CLAW_CONFIG_DEFAULT_LLM_API_KEY,  sizeof(c->llm.api_key));
    strlcpy(c->llm.model,    CLAW_CONFIG_DEFAULT_LLM_MODEL,    sizeof(c->llm.model));
    strlcpy(c->llm.api_url,  CLAW_CONFIG_DEFAULT_LLM_API_URL,  sizeof(c->llm.api_url));
    c->llm.max_tokens     = CLAW_CONFIG_DEFAULT_LLM_MAX_TOKENS;
    c->llm.max_iterations = CLAW_CONFIG_DEFAULT_LLM_MAX_ITER;
    c->llm.backend        = 0; /* default: Bearer (OpenAI/DashScope standard) */
    c->llm.thinking_enabled = CLAW_CONFIG_DEFAULT_LLM_THINKING;
    c->llm.stream_enabled   = CLAW_CONFIG_DEFAULT_LLM_STREAM;
    c->llm.compact_tokens   = CLAW_CONFIG_DEFAULT_LLM_COMPACT_TOKENS;
    c->llm.window_tokens    = CLAW_CONFIG_DEFAULT_LLM_WINDOW_TOKENS;

    /* telegram, feishu: empty */
    /* web_search */
    c->web_search.max_results = CLAW_CONFIG_DEFAULT_SEARCH_MAX_RESULTS;
    /* wechat */
    strlcpy(c->wechat.base_url, CLAW_CONFIG_DEFAULT_WECHAT_BASE_URL, sizeof(c->wechat.base_url));
    strlcpy(c->wechat.app_id,   CLAW_CONFIG_DEFAULT_WECHAT_APP_ID,   sizeof(c->wechat.app_id));

    /* lua */
    c->lua.module_mask = CLAW_CONFIG_DEFAULT_LUA_MODULE_MASK;

    /* vision */
    strlcpy(c->vision.model,    CLAW_CONFIG_DEFAULT_VISION_MODEL,    sizeof(c->vision.model));
    strlcpy(c->vision.base_url, CLAW_CONFIG_DEFAULT_VISION_BASE_URL, sizeof(c->vision.base_url));
    strlcpy(c->vision.api_path, CLAW_CONFIG_DEFAULT_VISION_API_PATH, sizeof(c->vision.api_path));
    /* vision.api_key defaults to empty → falls back to llm.api_key at runtime */
}

/* ---- JSON helpers ---- */

static void load_str(cJSON *obj, const char *key, char *dst, size_t dst_sz)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsString(item) && item->valuestring) {
        strlcpy(dst, item->valuestring, dst_sz);
    }
}

static void load_bool(cJSON *obj, const char *key, bool *dst)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsBool(item)) {
        *dst = cJSON_IsTrue(item);
    }
}

static void load_int_u16(cJSON *obj, const char *key, uint16_t *dst)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsNumber(item)) {
        *dst = (uint16_t)item->valueint;
    }
}

static void load_int_u32(cJSON *obj, const char *key, uint32_t *dst)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsNumber(item)) {
        *dst = (uint32_t)item->valueint;
    }
}

static void load_int_u8(cJSON *obj, const char *key, uint8_t *dst)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item && cJSON_IsNumber(item)) {
        *dst = (uint8_t)item->valueint;
    }
}

static void parse_config(cJSON *root, claw_config_t *c)
{
    cJSON *section;

    section = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    if (section) {
        load_str(section,  "ssid",          c->wifi.ssid,          sizeof(c->wifi.ssid));
        load_str(section,  "password",      c->wifi.password,      sizeof(c->wifi.password));
        load_str(section,  "security_type", c->wifi.security_type, sizeof(c->wifi.security_type));
        load_bool(section, "configured",    &c->wifi.configured);
    }

    section = cJSON_GetObjectItemCaseSensitive(root, "softap");
    if (section) {
        load_str(section,   "ssid",     c->softap.ssid,     sizeof(c->softap.ssid));
        load_str(section,   "password", c->softap.password, sizeof(c->softap.password));
        load_int_u8(section,"channel",  &c->softap.channel);
    }

    section = cJSON_GetObjectItemCaseSensitive(root, "llm");
    if (section) {
        load_str(section,    "api_key",        c->llm.api_key,       sizeof(c->llm.api_key));
        load_str(section,    "model",          c->llm.model,         sizeof(c->llm.model));
        load_str(section,    "api_url",        c->llm.api_url,       sizeof(c->llm.api_url));
        load_int_u32(section,"max_tokens",     &c->llm.max_tokens);
        load_int_u8(section, "max_iterations", &c->llm.max_iterations);
        load_int_u8(section, "backend",        &c->llm.backend);
        load_int_u8(section, "thinking_enabled", &c->llm.thinking_enabled);
        load_int_u8(section, "stream_enabled",   &c->llm.stream_enabled);
        load_int_u32(section,"compact_tokens",   &c->llm.compact_tokens);
        load_int_u32(section,"window_tokens",    &c->llm.window_tokens);
    }

    section = cJSON_GetObjectItemCaseSensitive(root, "telegram");
    if (section) {
        load_str(section, "bot_token", c->telegram.bot_token, sizeof(c->telegram.bot_token));
    }

    section = cJSON_GetObjectItemCaseSensitive(root, "feishu");
    if (section) {
        load_str(section, "app_id",     c->feishu.app_id,     sizeof(c->feishu.app_id));
        load_str(section, "app_secret", c->feishu.app_secret, sizeof(c->feishu.app_secret));
    }

    section = cJSON_GetObjectItemCaseSensitive(root, "web_search");
    if (section) {
        load_str(section,   "api_key",     c->web_search.api_key,    sizeof(c->web_search.api_key));
        load_int_u8(section,"max_results", &c->web_search.max_results);
    }

    section = cJSON_GetObjectItemCaseSensitive(root, "wechat");
    if (section) {
        load_str(section, "base_url", c->wechat.base_url, sizeof(c->wechat.base_url));
        load_str(section, "app_id",   c->wechat.app_id,   sizeof(c->wechat.app_id));
    }

    section = cJSON_GetObjectItemCaseSensitive(root, "lua");
    if (section) {
        load_int_u16(section, "module_mask", &c->lua.module_mask);
    }

    section = cJSON_GetObjectItemCaseSensitive(root, "webui");
    if (section) {
        load_str(section, "token", c->webui.token, sizeof(c->webui.token));
    }

    section = cJSON_GetObjectItemCaseSensitive(root, "vision");
    if (section) {
        load_str(section, "model",    c->vision.model,    sizeof(c->vision.model));
        load_str(section, "api_key",  c->vision.api_key,  sizeof(c->vision.api_key));
        load_str(section, "base_url", c->vision.base_url, sizeof(c->vision.base_url));
        load_str(section, "api_path", c->vision.api_path, sizeof(c->vision.api_path));
    }

}

/* ---- Public API ---- */

int claw_config_init(void)
{
    apply_defaults(&s_cfg);

    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) {
        RTK_LOGI(TAG, "no config file, using defaults\n");
        s_initialized = true;
        return RTK_SUCCESS;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 4096) {
        RTK_LOGW(TAG, "config file size %ld unexpected, using defaults\n", size);
        fclose(f);
        s_initialized = true;
        return RTK_SUCCESS;
    }

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        s_initialized = true;
        return RTK_ERR_NOMEM;
    }

    size_t read_len = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[read_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        RTK_LOGW(TAG, "config parse error, using defaults\n");
        s_initialized = true;
        return RTK_SUCCESS;
    }

    parse_config(root, &s_cfg);
    cJSON_Delete(root);

    RTK_LOGI(TAG, "loaded: wifi.configured=%d softap.ssid=%s\n",
             s_cfg.wifi.configured, s_cfg.softap.ssid);
    s_initialized = true;
    return RTK_SUCCESS;
}

int claw_config_save(void)
{
    if (!s_initialized) return RTK_ERR_BADARG;

    cJSON *root = cJSON_CreateObject();
    if (!root) return RTK_ERR_NOMEM;

    /* wifi */
    cJSON *wifi = cJSON_CreateObject();
    cJSON_AddStringToObject(wifi, "ssid",          s_cfg.wifi.ssid);
    cJSON_AddStringToObject(wifi, "password",      s_cfg.wifi.password);
    cJSON_AddStringToObject(wifi, "security_type", s_cfg.wifi.security_type);
    cJSON_AddBoolToObject(  wifi, "configured",    s_cfg.wifi.configured);
    cJSON_AddItemToObject(root, "wifi", wifi);

    /* softap */
    cJSON *softap = cJSON_CreateObject();
    cJSON_AddStringToObject(softap, "ssid",     s_cfg.softap.ssid);
    cJSON_AddStringToObject(softap, "password", s_cfg.softap.password);
    cJSON_AddNumberToObject(softap, "channel",  s_cfg.softap.channel);
    cJSON_AddItemToObject(root, "softap", softap);

    /* llm */
    cJSON *llm = cJSON_CreateObject();
    cJSON_AddStringToObject(llm, "api_key",        s_cfg.llm.api_key);
    cJSON_AddStringToObject(llm, "model",          s_cfg.llm.model);
    cJSON_AddStringToObject(llm, "api_url",        s_cfg.llm.api_url);
    cJSON_AddNumberToObject(llm, "max_tokens",     s_cfg.llm.max_tokens);
    cJSON_AddNumberToObject(llm, "max_iterations", s_cfg.llm.max_iterations);
    cJSON_AddNumberToObject(llm, "backend",        s_cfg.llm.backend);
    cJSON_AddNumberToObject(llm, "thinking_enabled", s_cfg.llm.thinking_enabled);
    cJSON_AddNumberToObject(llm, "stream_enabled",   s_cfg.llm.stream_enabled);
    cJSON_AddNumberToObject(llm, "compact_tokens",   s_cfg.llm.compact_tokens);
    cJSON_AddNumberToObject(llm, "window_tokens",    s_cfg.llm.window_tokens);
    cJSON_AddItemToObject(root, "llm", llm);

    /* telegram */
    cJSON *tg = cJSON_CreateObject();
    cJSON_AddStringToObject(tg, "bot_token", s_cfg.telegram.bot_token);
    cJSON_AddItemToObject(root, "telegram", tg);

    /* feishu */
    cJSON *fs = cJSON_CreateObject();
    cJSON_AddStringToObject(fs, "app_id",     s_cfg.feishu.app_id);
    cJSON_AddStringToObject(fs, "app_secret", s_cfg.feishu.app_secret);
    cJSON_AddItemToObject(root, "feishu", fs);

    /* web_search */
    cJSON *ws = cJSON_CreateObject();
    cJSON_AddStringToObject(ws, "api_key",     s_cfg.web_search.api_key);
    cJSON_AddNumberToObject(ws, "max_results", s_cfg.web_search.max_results);
    cJSON_AddItemToObject(root, "web_search", ws);

    /* wechat */
    cJSON *wc = cJSON_CreateObject();
    cJSON_AddStringToObject(wc, "base_url", s_cfg.wechat.base_url);
    cJSON_AddStringToObject(wc, "app_id",   s_cfg.wechat.app_id);
    cJSON_AddItemToObject(root, "wechat", wc);

    /* lua */
    cJSON *lua = cJSON_CreateObject();
    cJSON_AddNumberToObject(lua, "module_mask", s_cfg.lua.module_mask);
    cJSON_AddItemToObject(root, "lua", lua);

    /* webui */
    cJSON *webui = cJSON_CreateObject();
    cJSON_AddStringToObject(webui, "token", s_cfg.webui.token);
    cJSON_AddItemToObject(root, "webui", webui);

    /* vision */
    cJSON *vision = cJSON_CreateObject();
    cJSON_AddStringToObject(vision, "model",    s_cfg.vision.model);
    cJSON_AddStringToObject(vision, "api_key",  s_cfg.vision.api_key);
    cJSON_AddStringToObject(vision, "base_url", s_cfg.vision.base_url);
    cJSON_AddStringToObject(vision, "api_path", s_cfg.vision.api_path);
    cJSON_AddItemToObject(root, "vision", vision);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return RTK_ERR_NOMEM;

    FILE *f = fopen(CONFIG_FILE, "w");
    if (!f) {
        free(json_str);
        RTK_LOGE(TAG, "failed to open config file for writing\n");
        return RTK_FAIL;
    }

    size_t len = strlen(json_str);
    size_t written = fwrite(json_str, 1, len, f);
    fclose(f);
    free(json_str);

    if (written != len) {
        RTK_LOGE(TAG, "write incomplete: %u/%u bytes\n", (unsigned)written, (unsigned)len);
        return RTK_FAIL;
    }

    RTK_LOGI(TAG, "saved\n");
    notify_on_save();
    return RTK_SUCCESS;
}

claw_config_t *claw_config_get(void)
{
    return &s_cfg;
}

void claw_config_update_wifi_mem(const char *ssid, const char *password,
                                 const char *security_type)
{
    if (!ssid) return;
    strlcpy(s_cfg.wifi.ssid,          ssid,                     sizeof(s_cfg.wifi.ssid));
    strlcpy(s_cfg.wifi.password,      password ? password : "", sizeof(s_cfg.wifi.password));
    strlcpy(s_cfg.wifi.security_type, security_type ? security_type : "WPA2",
            sizeof(s_cfg.wifi.security_type));
    s_cfg.wifi.configured = true;
}

int claw_config_set_wifi(const char *ssid, const char *password, const char *security_type)
{
    if (!ssid) return RTK_ERR_BADARG;
    claw_config_update_wifi_mem(ssid, password, security_type);
    return claw_config_save();
}

int claw_config_set_llm(const char *api_key, const char *model, const char *api_url,
                        uint32_t max_tokens, uint8_t max_iterations, int backend,
                        int thinking_enabled, int stream_enabled,
                        uint32_t compact_tokens, uint32_t window_tokens)
{
    if (api_key)               strlcpy(s_cfg.llm.api_key, api_key,  sizeof(s_cfg.llm.api_key));
    if (model && model[0])     strlcpy(s_cfg.llm.model,   model,    sizeof(s_cfg.llm.model));
    if (api_url && api_url[0]) strlcpy(s_cfg.llm.api_url, api_url,  sizeof(s_cfg.llm.api_url));
    if (max_tokens)            s_cfg.llm.max_tokens     = max_tokens;
    if (max_iterations)        s_cfg.llm.max_iterations = max_iterations;
    if (backend >= 0)          s_cfg.llm.backend        = (uint8_t)backend;
    if (thinking_enabled >= 0) s_cfg.llm.thinking_enabled = thinking_enabled ? 1 : 0;
    if (stream_enabled >= 0)   s_cfg.llm.stream_enabled   = stream_enabled ? 1 : 0;
    if (compact_tokens)        s_cfg.llm.compact_tokens   = compact_tokens;
    if (window_tokens)         s_cfg.llm.window_tokens    = window_tokens;
    return claw_config_save();
}

int claw_config_set_telegram(const char *bot_token)
{
    strlcpy(s_cfg.telegram.bot_token,
            bot_token ? bot_token : "",
            sizeof(s_cfg.telegram.bot_token));
    return claw_config_save();
}

int claw_config_set_feishu(const char *app_id, const char *app_secret)
{
    strlcpy(s_cfg.feishu.app_id,     app_id     ? app_id     : "", sizeof(s_cfg.feishu.app_id));
    strlcpy(s_cfg.feishu.app_secret, app_secret ? app_secret : "", sizeof(s_cfg.feishu.app_secret));
    return claw_config_save();
}

int claw_config_set_wechat(const char *base_url, const char *app_id)
{
    if (base_url && base_url[0])
        strlcpy(s_cfg.wechat.base_url, base_url, sizeof(s_cfg.wechat.base_url));
    if (app_id && app_id[0])
        strlcpy(s_cfg.wechat.app_id, app_id, sizeof(s_cfg.wechat.app_id));
    return claw_config_save();
}

int claw_config_set_imbot(const char *wechat_base_url,
                          const char *feishu_app_id, const char *feishu_app_secret,
                          const char *tg_bot_token)
{
    if (wechat_base_url && wechat_base_url[0])
        strlcpy(s_cfg.wechat.base_url, wechat_base_url, sizeof(s_cfg.wechat.base_url));
    /* wechat app_id not exposed in UI — always preserved */
    strlcpy(s_cfg.feishu.app_id,      feishu_app_id     ? feishu_app_id     : "", sizeof(s_cfg.feishu.app_id));
    strlcpy(s_cfg.feishu.app_secret,  feishu_app_secret ? feishu_app_secret : "", sizeof(s_cfg.feishu.app_secret));
    strlcpy(s_cfg.telegram.bot_token, tg_bot_token      ? tg_bot_token      : "", sizeof(s_cfg.telegram.bot_token));
    return claw_config_save();
}

int claw_config_set_search(const char *api_key, uint8_t max_results)
{
    strlcpy(s_cfg.web_search.api_key, api_key ? api_key : "", sizeof(s_cfg.web_search.api_key));
    if (max_results > 0) s_cfg.web_search.max_results = max_results;
    return claw_config_save();
}

int claw_config_set_lua_modules(uint16_t mask)
{
    s_cfg.lua.module_mask = mask | CLAW_LUA_MOD_LOCKED_MASK;
    return claw_config_save();
}

int claw_config_set_vision(const char *model, const char *api_key,
                           const char *base_url, const char *api_path)
{
    if (model    && model[0])    strlcpy(s_cfg.vision.model,    model,    sizeof(s_cfg.vision.model));
    if (api_key)                 strlcpy(s_cfg.vision.api_key,  api_key,  sizeof(s_cfg.vision.api_key));
    if (base_url && base_url[0]) strlcpy(s_cfg.vision.base_url, base_url, sizeof(s_cfg.vision.base_url));
    if (api_path && api_path[0]) strlcpy(s_cfg.vision.api_path, api_path, sizeof(s_cfg.vision.api_path));
    return claw_config_save();
}

int claw_config_ensure_api_token(void)
{
    if (!s_initialized) return RTK_ERR_BADARG;

    /* Already have a token — nothing to do. */
    if (s_cfg.webui.token[0] != '\0') return RTK_SUCCESS;

    /* Generate 16 random bytes via hardware TRNG and hex-encode them. */
    uint8_t rnd[16];
    if (TRNG_get_random_bytes(rnd, sizeof(rnd)) != 0) {
        RTK_LOGE(TAG, "TRNG failed, cannot generate API token\n");
        return RTK_FAIL;
    }

    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        s_cfg.webui.token[i * 2]     = hex[rnd[i] >> 4];
        s_cfg.webui.token[i * 2 + 1] = hex[rnd[i] & 0x0F];
    }
    s_cfg.webui.token[32] = '\0';

    RTK_LOGI(TAG, "generated new WebUI API token\n");
    return claw_config_save();
}

void claw_config_clear_wifi_mem(void)
{
    _memset(s_cfg.wifi.ssid,     0, sizeof(s_cfg.wifi.ssid));
    _memset(s_cfg.wifi.password, 0, sizeof(s_cfg.wifi.password));
    strlcpy(s_cfg.wifi.security_type, "WPA2", sizeof(s_cfg.wifi.security_type));
    s_cfg.wifi.configured = false;
    RTK_LOGI(TAG, "wifi config cleared (mem only)\n");
}

int claw_config_clear_wifi(void)
{
    claw_config_clear_wifi_mem();
    return claw_config_save();
}

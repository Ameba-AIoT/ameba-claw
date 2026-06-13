#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Compile-time defaults (override via vfs:claw_config.json at runtime) ---- */
#define CLAW_CONFIG_DEFAULT_LLM_API_KEY       ""
#define CLAW_CONFIG_DEFAULT_LLM_MODEL         "glm-5.1"
#define CLAW_CONFIG_DEFAULT_LLM_API_URL       ""
#define CLAW_CONFIG_DEFAULT_LLM_MAX_TOKENS    16384
#define CLAW_CONFIG_DEFAULT_LLM_MAX_ITER      50
#define CLAW_CONFIG_DEFAULT_LLM_THINKING      0   /* 0 = reasoning off (GLM "thinking":disabled) */
#define CLAW_CONFIG_DEFAULT_LLM_STREAM        1   /* 1 = request SSE streaming ("stream":true) */
/* Token-budget compaction: let the conversation grow until prompt_tokens nears
 * the model window, then summarize older turns. 0 keeps the built-in default. */
#define CLAW_CONFIG_DEFAULT_LLM_COMPACT_TOKENS  110000  /* trigger compaction at/above this */
#define CLAW_CONFIG_DEFAULT_LLM_WINDOW_TOKENS   128000  /* hard ceiling (model context window) */

#define CLAW_CONFIG_DEFAULT_SOFTAP_SSID       "AmebaClaw"
#define CLAW_CONFIG_DEFAULT_SOFTAP_PASSWORD   ""
#define CLAW_CONFIG_DEFAULT_SOFTAP_CHANNEL    6

#define CLAW_CONFIG_DEFAULT_SEARCH_MAX_RESULTS 3
#define CLAW_CONFIG_DEFAULT_WECHAT_BASE_URL    "https://ilinkai.weixin.qq.com"
#define CLAW_CONFIG_DEFAULT_WECHAT_APP_ID      "bot"

/* Bit positions: audio=0, uart=1, i2c=2, spi=3, rtc=4,
 *                timer=5, file=6, wifi=7, gpio=8, sys=9, event=10, cap=11 */
#define CLAW_CONFIG_DEFAULT_LUA_MODULE_MASK    0x0FFFu /* all 12 modules enabled */

#define CLAW_CONFIG_DEFAULT_VISION_MODEL    "glm-5v-turbo"
#define CLAW_CONFIG_DEFAULT_VISION_BASE_URL "open.bigmodel.cn"
#define CLAW_CONFIG_DEFAULT_VISION_API_PATH "/api/paas/v4/chat/completions"

/* sys/event/cap are core infrastructure — always enabled, cannot be turned off via UI */
#define CLAW_LUA_MOD_LOCKED_MASK  ((uint16_t)((1u<<9)|(1u<<10)|(1u<<11)))

/* ---- Config structs ---- */

typedef struct {
    char    ssid[64];
    char    password[64];
    char    security_type[16]; /* "WPA2", "WPA", "OPEN" */
    bool    configured;        /* true = credentials are valid, skip SoftAP on next boot */
} claw_wifi_config_t;

typedef struct {
    char    ssid[32];
    char    password[64];   /* empty = open network */
    uint8_t channel;
} claw_softap_config_t;

typedef struct {
    char     api_key[128];
    char     model[32];
    char     api_url[256];
    uint32_t max_tokens;
    uint8_t  max_iterations;
    uint8_t  backend; /* 0=Bearer(OpenAI/DashScope), 1=x-api-key(RealGPT), 2=Anthropic */
    uint8_t  thinking_enabled; /* 0 = send GLM "thinking":{"type":"disabled"}; 1 = leave default */
    uint8_t  stream_enabled;   /* 1 = request SSE streaming ("stream":true); ignored for Anthropic backend */
    uint32_t compact_tokens;   /* compaction trigger: real prompt_tokens at/above this → summarize older turns */
    uint32_t window_tokens;    /* hard context ceiling; synchronous trim guards against exceeding it */
} claw_llm_config_t;

typedef struct {
    char bot_token[128];
} claw_telegram_config_t;

typedef struct {
    char app_id[64];
    char app_secret[64];
} claw_feishu_config_t;

typedef struct {
    char    api_key[128];
    uint8_t max_results;
} claw_web_search_config_t;

typedef struct {
    char base_url[256];  /* WeChat iLink API base URL */
    char app_id[64];     /* iLink App ID, default "bot" */
} claw_wechat_config_t;

typedef struct {
    uint16_t module_mask; /* bitmask: bit N=1 means module N is enabled */
} claw_lua_config_t;

typedef struct {
    char model[64];     /* vision model, e.g. "glm-5v-turbo" */
    char api_key[128];  /* dedicated key; empty = fall back to llm.api_key */
    char base_url[128]; /* API host, e.g. "open.bigmodel.cn" */
    char api_path[128]; /* API path, e.g. "/api/paas/v4/chat/completions" */
} claw_vision_config_t;

typedef struct {
    /* Token required in X-API-Token header for all /api/ routes.
     * Generated automatically on first boot via TRNG; empty = not yet set
     * (should only happen before claw_config_ensure_api_token() runs). */
    char token[33];  /* 16 bytes hex-encoded = 32 chars + NUL */
} claw_webui_config_t;

/* Master config — all user-visible settings in one place */
typedef struct {
    claw_wifi_config_t       wifi;
    claw_softap_config_t     softap;
    claw_llm_config_t        llm;
    claw_telegram_config_t   telegram;
    claw_feishu_config_t     feishu;
    claw_web_search_config_t web_search;
    claw_wechat_config_t     wechat;
    claw_lua_config_t        lua;
    claw_webui_config_t      webui;
    claw_vision_config_t     vision;
} claw_config_t;

/* ---- API ---- */

/* Load config from vfs:claw_config.json; fall back to defaults if missing. */
int claw_config_init(void);

/* Persist current config to vfs:claw_config.json. */
int claw_config_save(void);

/* Return pointer to the live config instance. Never NULL after claw_config_init(). */
claw_config_t *claw_config_get(void);

/* Save WiFi credentials and mark wifi.configured = true, then persist.
 * NOTE: calls claw_config_save() which uses cJSON+VFS; needs ~3.5 KB stack.
 * Prefer claw_config_update_wifi_mem() + claw_config_save() from a deep-stack
 * task when the caller's stack is limited. */
int claw_config_set_wifi(const char *ssid, const char *password, const char *security_type);

/* Update WiFi credentials in memory ONLY — does NOT write to flash.
 * Use this from shallow-stack tasks; follow with claw_config_save() on a
 * task with sufficient stack (>= 6 KB). */
void claw_config_update_wifi_mem(const char *ssid, const char *password,
                                 const char *security_type);

/* Save LLM settings and persist. Pass NULL/0 for fields to keep current value.
 * thinking_enabled / stream_enabled / backend: non-negative=set, negative=keep current. */
int claw_config_set_llm(const char *api_key, const char *model, const char *api_url,
                        uint32_t max_tokens, uint8_t max_iterations, int backend,
                        int thinking_enabled, int stream_enabled,
                        uint32_t compact_tokens, uint32_t window_tokens);

/* Save Telegram bot token and persist. */
int claw_config_set_telegram(const char *bot_token);

/* Save Feishu credentials and persist. */
int claw_config_set_feishu(const char *app_id, const char *app_secret);

/* Save WeChat config and persist. Pass NULL or empty to keep current value. */
int claw_config_set_wechat(const char *base_url, const char *app_id);

/* Save web search config and persist. */
int claw_config_set_search(const char *api_key, uint8_t max_results);

/* Save all IM bot configs (wechat base_url, feishu, telegram) in one persist.
 * wechat app_id is not exposed in UI and is always preserved.
 * Empty string for feishu/telegram fields disables that platform. */
int claw_config_set_imbot(const char *wechat_base_url,
                          const char *feishu_app_id, const char *feishu_app_secret,
                          const char *tg_bot_token);

/* Save Lua module mask and persist. */
int claw_config_set_lua_modules(uint16_t mask);

/* Save vision config fields and persist. Pass NULL/empty to keep current value. */
int claw_config_set_vision(const char *model, const char *api_key,
                           const char *base_url, const char *api_path);

/* Generate a random WebUI API token via TRNG if none exists yet, then persist.
 * Safe to call on every boot: no-op when a token is already present. */
int claw_config_ensure_api_token(void);

/* Clear WiFi credentials and set configured=false, then persist.
 * NOTE: calls claw_config_save() which uses cJSON+VFS; needs ~3.5 KB stack.
 * Prefer claw_config_clear_wifi_mem() + claw_config_save() from a deep-stack task. */
int claw_config_clear_wifi(void);

/* Clear WiFi credentials in memory ONLY — does NOT write to flash. */
void claw_config_clear_wifi_mem(void);

/* Register a callback invoked after every successful claw_config_save().
 * Used by IM modules to detect credential changes and start tasks on demand.
 * Maximum 4 callbacks. Duplicate registrations are silently ignored. */
typedef void (*claw_config_on_save_fn_t)(void);
void claw_config_register_on_save(claw_config_on_save_fn_t cb);

#ifdef __cplusplus
}
#endif

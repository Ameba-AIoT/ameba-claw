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

/* Comma-separated module names that are disabled at runtime, e.g. "uart,spi".
 * Empty string = all compiled-in modules enabled (default). */
#define CLAW_CONFIG_DEFAULT_LUA_DISABLED_MODULES ""
#define CLAW_LUA_DISABLED_MODULES_SIZE 256

#define CLAW_CONFIG_DEFAULT_VISION_MODEL    "glm-5v-turbo"
#define CLAW_CONFIG_DEFAULT_VISION_BASE_URL "open.bigmodel.cn"
#define CLAW_CONFIG_DEFAULT_VISION_API_PATH "/api/paas/v4/chat/completions"

/* Locked modules (sys/event/cap/wifi/udp) are enforced in the registry regardless
 * of what disabled_modules contains; no config-layer constant needed. */

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
    uint8_t  backend; /* 0=Bearer/OpenAI-compatible, 1=Anthropic (x-api-key + anthropic-version) */
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
    char   app_id[64];      /* QQ bot AppID */
    char   app_secret[128]; /* QQ bot clientSecret */
    int8_t msg_type;        /* 0=text, 2=markdown; default 0 */
} claw_qq_config_t;

typedef struct {
    char    api_key[128];
    uint8_t max_results;
} claw_web_search_config_t;

typedef struct {
    char base_url[256];  /* WeChat iLink API base URL */
    char app_id[64];     /* iLink App ID, default "bot" */
} claw_wechat_config_t;

typedef struct {
    char disabled_modules[CLAW_LUA_DISABLED_MODULES_SIZE]; /* "" = all enabled */
} claw_lua_config_t;

/* Local timezone. The system clock is UTC (SNTP); this offset is applied
 * MANUALLY (utc + offset) to derive local time — do NOT rely on localtime_r,
 * which returns UTC here (no TZ env). `set` distinguishes "user configured a
 * timezone" from "never set": when false, local-time features refuse to guess
 * and prompt the user to set it (see cap_time set_timezone cap). */
typedef struct {
    int32_t offset_min;   /* minutes east of UTC, e.g. 480 = UTC+8, -300 = UTC-5 */
    bool    set;          /* true once the user has explicitly configured it */
} claw_time_config_t;

/* Max bytes for allowlist text (newline-separated rules, ~20 entries comfortably) */
#define CLAW_HTTP_REQUEST_ALLOWLIST_MAX 512

typedef struct {
    /* '\n'-separated allowlist rules.
     * Empty = deny all.  "*" alone = allow all.
     * Otherwise: any matching rule permits the host; no match = denied. */
    char allowlist[CLAW_HTTP_REQUEST_ALLOWLIST_MAX];
} claw_http_request_config_t;

typedef struct {
    char model[64];     /* vision model, e.g. "glm-5v-turbo" */
    char api_key[128];  /* dedicated key; empty = fall back to llm.api_key */
    char base_url[128]; /* API host, e.g. "open.bigmodel.cn" */
    char api_path[128]; /* API path, e.g. "/api/paas/v4/chat/completions" */
    char api_type[16];  /* "openai" (default) or "anthropic" */
} claw_vision_config_t;

#define CLAW_CAP_HIDDEN_MAX   24  /* matches CAP_VIS_MAX; covers all possible groups */
#define CLAW_CAP_GROUP_ID_LEN 64

typedef struct {
    char    hidden[CLAW_CAP_HIDDEN_MAX][CLAW_CAP_GROUP_ID_LEN];
    uint8_t hidden_count;
} claw_cap_visibility_config_t;

/* Runtime enable/disable per group. Deny-list: absent = enabled (default on).
 * Changes take effect on next boot; a disabled group's lifecycle hooks are skipped. */
#define CLAW_CAP_RUNTIME_DISABLED_MAX 24

typedef struct {
    char    disabled[CLAW_CAP_RUNTIME_DISABLED_MAX][CLAW_CAP_GROUP_ID_LEN];
    uint8_t disabled_count;
} claw_cap_runtime_config_t;

/* Master config — all user-visible settings in one place */
typedef struct {
    claw_wifi_config_t            wifi;
    claw_softap_config_t          softap;
    claw_llm_config_t             llm;
    claw_telegram_config_t        telegram;
    claw_feishu_config_t          feishu;
    claw_qq_config_t              qq;
    claw_web_search_config_t      web_search;
    claw_wechat_config_t          wechat;
    claw_lua_config_t             lua;
    claw_time_config_t            time;
    claw_vision_config_t          vision;
    claw_http_request_config_t    http_request;
    claw_cap_visibility_config_t  cap_visibility;
    claw_cap_runtime_config_t     cap_runtime;
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

/* Save all IM bot configs (wechat base_url, feishu, telegram, qq) in one persist.
 * wechat app_id is not exposed in UI and is always preserved.
 * Empty string for feishu/telegram/qq fields disables that platform. */
int claw_config_set_imbot(const char *wechat_base_url,
                          const char *feishu_app_id, const char *feishu_app_secret,
                          const char *tg_bot_token,
                          const char *qq_app_id, const char *qq_app_secret);

/* Save QQ bot credentials and persist. */
int claw_config_set_qq(const char *app_id, const char *app_secret, int msg_type);

/* Set the local timezone offset (minutes east of UTC) and mark it configured,
 * then persist. This is the single writer for the `time` config section; all
 * local-time consumers (cap_time, cron) read it back via cap_time's unified API. */
int claw_config_set_timezone(int32_t offset_min);

/* Save Lua disabled-modules list and persist.
 * disabled_csv: comma-separated module names to disable, e.g. "uart,spi".
 * Empty string or NULL re-enables all. Locked modules are enforced by the
 * registry at install time; this layer stores the value as-is. */
int claw_config_set_lua_modules(const char *disabled_csv);

/* Save vision config fields and persist. Pass NULL/empty to keep current value. */
int claw_config_set_vision(const char *model, const char *api_key,
                           const char *base_url, const char *api_path);

/* Save HTTP request allowlist and persist.
 * allowlist: newline-separated rules (e.g. "ip-api.com\napi.bilibili.com\n").
 *            Empty or NULL = deny all (no hosts permitted). Use "*" to allow all. */
int claw_config_set_http_request(const char *allowlist);

/* Set which cap groups are hidden from the LLM tools list and persist.
 * hidden_groups: array of group_id strings; count: number of entries.
 * Pass count=0 to make all groups visible. */
int claw_config_set_cap_visibility(const char *const *hidden_groups, uint8_t count);
int claw_config_set_cap_runtime_disabled(const char *const *groups, uint8_t count);

/* Clear WiFi credentials and set configured=false, then persist.
 * NOTE: calls claw_config_save() which uses cJSON+VFS; needs ~3.5 KB stack.
 * Prefer claw_config_clear_wifi_mem() + claw_config_save() from a deep-stack task. */
int claw_config_clear_wifi(void);

/* Clear WiFi credentials in memory ONLY — does NOT write to flash. */
void claw_config_clear_wifi_mem(void);

/* Register a callback invoked after every successful claw_config_save().
 * Used by IM modules to detect credential changes and start tasks on demand.
 * Maximum 6 callbacks. Duplicate registrations are silently ignored. */
typedef void (*claw_config_on_save_fn_t)(void);
void claw_config_register_on_save(claw_config_on_save_fn_t cb);

#ifdef __cplusplus
}
#endif

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Register WebUI HTTP routes with the claw_http_server.
 * Must be called after claw_http_server_init() and before claw_http_server_start().
 *
 * Routes registered:
 *   GET  /status             — JSON device status
 *   GET  /setup              — HTML WiFi provisioning page
 *   POST /api/wifi/connect   — JSON body {"ssid":"...","password":"..."} → connect + save
 *   GET  /api/wifi/scan      — JSON list of nearby APs (stub: returns empty array)
 */
int cap_webui_init(void);

#ifdef __cplusplus
}
#endif

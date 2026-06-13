#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLAW_WIFI_STATE_IDLE,
    CLAW_WIFI_STATE_PROVISIONING,  /* SoftAP running, waiting for user to configure */
    CLAW_WIFI_STATE_CONNECTING,    /* STA connect in progress */
    CLAW_WIFI_STATE_CONNECTED,     /* STA connected and has IP */
    CLAW_WIFI_STATE_DISCONNECTED,  /* configured but not connected; driver handling reconnect */
} claw_wifi_state_t;

/* Initialize the WiFi manager (call after claw_config_init). */
int claw_wifi_mgr_init(void);

/*
 * Start WiFi:
 *  - If config.wifi.configured: connect STA with saved credentials.
 *  - Otherwise: start SoftAP provisioning mode (AmebaSoftAP, open network).
 * Non-blocking in provisioning mode; blocks until connected (or fails) in normal mode.
 */
int claw_wifi_mgr_start(void);

/*
 * Connect STA to the given network and obtain a DHCP address.
 * Blocks up to ~15 s. Intended for use by the WebUI provisioning handler.
 * On success, call claw_config_set_wifi() separately to persist credentials.
 */
int claw_wifi_mgr_connect_sta(const char *ssid, const char *password);

claw_wifi_state_t claw_wifi_mgr_get_state(void);

/* Return current STA IP as a dotted string, or "0.0.0.0" if not connected. */
const char *claw_wifi_mgr_get_sta_ip(void);

/* Return true if SoftAP is currently running. */
bool claw_wifi_mgr_is_softap_running(void);

/* Return last connect error string, or "" if no error. Cleared on next connect attempt. */
const char *claw_wifi_mgr_get_connect_error(void);

/*
 * FreeRTOS task entry point that calls claw_wifi_mgr_init() + claw_wifi_mgr_start().
 * Must be created via rtos_task_create so it runs after the scheduler starts.
 */
void claw_wifi_mgr_task_entry(void *param);

/*
 * Register a callback invoked once WiFi STA obtains an IP address.
 * Also fires on reconnect after a drop. Maximum 8 callbacks.
 * Safe to call before claw_wifi_mgr_start().
 */
typedef void (*claw_wifi_on_connected_fn_t)(void);
void claw_wifi_mgr_register_on_connected(claw_wifi_on_connected_fn_t cb);

#ifdef __cplusplus
}
#endif

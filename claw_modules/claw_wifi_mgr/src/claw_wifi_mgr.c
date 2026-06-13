#include "ameba_soc.h"
#include "claw_wifi_mgr.h"
#include "claw_config.h"
#include "claw_compat.h"

#include "wifi_api.h"
#include "wifi_api_ext.h"
#include "wifi_fast_connect.h"
#include "wifi_auto_reconnect.h"
#include "lwip_netconf.h"
#include "basic_types.h"

#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

#include "os_wrapper.h"
#include <stdio.h>
#include <string.h>

#define TAG "claw_wifi_mgr"

/* ---- On-connected callbacks ---- */

#define MAX_WIFI_ON_CONN_CBS 8
static claw_wifi_on_connected_fn_t s_on_conn_cbs[MAX_WIFI_ON_CONN_CBS];
static int                         s_on_conn_count = 0;

void claw_wifi_mgr_register_on_connected(claw_wifi_on_connected_fn_t cb)
{
    if (!cb) return;
    for (int i = 0; i < s_on_conn_count; i++) {
        if (s_on_conn_cbs[i] == cb) return;
    }
    if (s_on_conn_count < MAX_WIFI_ON_CONN_CBS)
        s_on_conn_cbs[s_on_conn_count++] = cb;
}

static void notify_on_connected(void)
{
    for (int i = 0; i < s_on_conn_count; i++)
        s_on_conn_cbs[i]();
}

extern struct netif xnetif[];
extern void *dhcps_init(struct netif *pnetif);
extern int dhcps_start(struct netif *pnetif);
extern void dhcps_deinit(struct netif *pnetif);

/* ---- State ---- */

static volatile claw_wifi_state_t s_state     = CLAW_WIFI_STATE_IDLE;
static volatile bool              s_softap_up = false;
static bool              s_wifi_on_done  = false;
static char              s_sta_ip[16]    = "0.0.0.0";
static char              s_softap_ssid[32] = "";
static char              s_connect_error[64] = "";
static struct rtw_softap_info s_softap_ap_info;

/* ---- Channel pre-alignment (scan + CSA) ---- */
static rtos_sema_t s_csa_done_sema = NULL;

/* ---- Internal helpers ---- */

static void ensure_wifi_on(void)
{
    if (!s_wifi_on_done) {
        wifi_on(RTW_MODE_STA);
        s_wifi_on_done = true;
    }
}

static int start_softap(void)
{
    const claw_config_t *cfg = claw_config_get();
    uint32_t ip_addr, netmask, gw;
    s32 ret;

    /* Per RTK docs, the device is already in STA mode after wifi_on() at boot.
     * Concurrent AP+STA mode is entered by calling wifi_start_ap() while STA
     * mode is active — no extra wifi_on() call is needed here. */
    wifi_fast_connect_enable(0);
    wifi_set_autoreconnect(0);
    wifi_disconnect();
    rtos_time_delay_ms(500);  /* let disconnect settle */
    dhcps_deinit(&xnetif[NETIF_WLAN_AP_INDEX]);
    s_wifi_on_done = true;  /* WiFi is already up in STA mode from boot */

    ip_addr = CONCAT_TO_UINT32(192, 168, 1, 1);
    netmask = CONCAT_TO_UINT32(255, 255, 255, 0);
    gw      = CONCAT_TO_UINT32(192, 168, 1, 1);
    lwip_set_ip(NETIF_WLAN_AP_INDEX, ip_addr, netmask, gw);

    wifi_set_autoreconnect(0);

    /* Build device-specific SSID: AmebaClaw-XXXX from last 2 MAC bytes */
    struct rtw_mac mac_addr;
    if (wifi_get_mac_address(0, &mac_addr, 1) == RTK_SUCCESS) {
        snprintf(s_softap_ssid, sizeof(s_softap_ssid), "AmebaClaw-%02X%02X",
                 mac_addr.octet[4], mac_addr.octet[5]);
    } else {
        strlcpy(s_softap_ssid, cfg->softap.ssid, sizeof(s_softap_ssid));
    }

    _memset(&s_softap_ap_info, 0, sizeof(s_softap_ap_info));
    s_softap_ap_info.ssid.len = (uint8_t)strlen(s_softap_ssid);
    _memcpy(s_softap_ap_info.ssid.val, s_softap_ssid, s_softap_ap_info.ssid.len);
    s_softap_ap_info.hidden_ssid = 0;
    s_softap_ap_info.channel     = cfg->softap.channel;

    if (cfg->softap.password[0] != '\0') {
        s_softap_ap_info.security_type = RTW_SECURITY_WPA2_AES_PSK;
        s_softap_ap_info.password      = (u8 *)cfg->softap.password;
        s_softap_ap_info.password_len  = (u8)strlen(cfg->softap.password);
    } else {
        s_softap_ap_info.security_type = RTW_SECURITY_OPEN;
        s_softap_ap_info.password      = NULL;
        s_softap_ap_info.password_len  = 0;
    }

    /* wifi_set_user_config() (called inside wifi_on) leaves en_mcc=DISABLE by
     * default. Override here, before wifi_start_ap() reads these values. */
    wifi_user_config.en_mcc = (u8)ENABLE;
    wifi_user_config.no_beacon_disconnect_time = 20; /* unit 2s = 40s, MCC TDMA tolerance */

    /* Retry wifi_start_ap up to 5 times if the first attempt fails.
     * On each retry, explicitly stop any partially-initialised AP first.
     * Without this, wifi_start_ap() hits the "already an AP running" early-
     * return path and returns 0 without actually configuring the SSID. */
    for (int retry = 0; retry < 5; retry++) {
        if (retry > 0) {
            wifi_stop_ap();
            rtos_time_delay_ms(500);
        }
        ret = wifi_start_ap(&s_softap_ap_info);
        if (ret == RTK_SUCCESS) break;
        RTK_LOGW(TAG, "wifi_start_ap failed (%d), retry %d/5\n", ret, retry + 1);
        rtos_time_delay_ms(2000);
    }
    if (ret != RTK_SUCCESS) {
        RTK_LOGE(TAG, "wifi_start_ap failed after retries: %d\n", ret);
        return RTK_FAIL;
    }

    dhcps_init(&xnetif[NETIF_WLAN_AP_INDEX]);
    dhcps_start(&xnetif[NETIF_WLAN_AP_INDEX]);
    s_softap_up = true;

    RTK_LOGI(TAG, "SoftAP started: SSID=%s channel=%u IP=192.168.1.1\n",
             s_softap_ssid, cfg->softap.channel);
    return RTK_SUCCESS;
}


static void update_sta_ip(void)
{
    const ip4_addr_t *ip = netif_ip4_addr(&xnetif[NETIF_WLAN_STA_INDEX]);
    if (ip) {
        DiagSnPrintf(s_sta_ip, sizeof(s_sta_ip), "%u.%u.%u.%u",
                 ip4_addr1(ip), ip4_addr2(ip), ip4_addr3(ip), ip4_addr4(ip));
    } else {
        strlcpy(s_sta_ip, "0.0.0.0", sizeof(s_sta_ip));
    }
}

/* Silent connectivity check — same logic as lwip_check_connectivity but without log output. */
static bool is_sta_connected(void)
{
    u8 join_status = RTW_JOINSTATUS_UNKNOWN;
    if (wifi_get_join_status(&join_status) != RTK_SUCCESS) return false;
    if (join_status != RTW_JOINSTATUS_SUCCESS) return false;
    return (*(u32 *)lwip_get_ip(NETIF_WLAN_STA_INDEX) != IP_ADDR_INVALID);
}

typedef struct { const char *ssid; u8 channel; } scan_ctx_t;

static s32 scan_for_channel_cb(struct rtw_scan_result *ap, void *user_data, u8 *ies, u32 ie_len)
{
    (void)ies; (void)ie_len;
    scan_ctx_t *ctx = (scan_ctx_t *)user_data;
    if (ctx->channel != 0 || ap->ssid.len == 0) return RTK_SUCCESS;
    size_t tlen = strlen(ctx->ssid);
    if (ap->ssid.len == (u8)tlen && _memcmp(ap->ssid.val, ctx->ssid, tlen) == 0) {
        ctx->channel = (u8)ap->channel;
        RTK_LOGI(TAG, "scan: '%s' on ch%u\n", ctx->ssid, (unsigned)ctx->channel);
    }
    return RTK_SUCCESS;
}

static u8 scan_get_ap_channel(const char *ssid)
{
    scan_ctx_t ctx = { .ssid = ssid, .channel = 0 };
    struct rtw_scan_param sp;
    _memset(&sp, 0, sizeof(sp));
    sp.options = RTW_SCAN_ACTIVE | RTW_SCAN_REPORT_EACH;
    sp.scan_report_each_mode_user_callback = scan_for_channel_cb;
    sp.scan_user_data = &ctx;
    s32 rc = wifi_scan_networks(&sp, 1 /* blocking */);
    if (rc != RTK_SUCCESS)
        RTK_LOGW(TAG, "scan_get_ap_channel: failed %d\n", rc);
    return ctx.channel;
}

static void csa_align_done_cb(u8 channel, s8 ret)
{
    (void)channel; (void)ret;
    if (s_csa_done_sema) rtos_sema_give(s_csa_done_sema);
}

static void align_softap_channel(u8 target_ch)
{
    if (s_softap_ap_info.channel == target_ch) {
        RTK_LOGI(TAG, "SoftAP already on ch%u, no CSA needed\n", (unsigned)target_ch);
        return;
    }
    RTK_LOGI(TAG, "CSA: SoftAP ch%u -> ch%u\n",
             (unsigned)s_softap_ap_info.channel, (unsigned)target_ch);
    rtos_sema_create_binary(&s_csa_done_sema);
    struct rtw_csa_parm csa = {
        .new_chl         = target_ch,
        .chl_switch_cnt  = 5,
        .action_type     = 1,
        .bc_action_cnt   = 3,
        .chl_switch_mode = 0,
        .callback        = csa_align_done_cb,
    };
    s32 rc = wifi_ap_switch_chl_and_inform(&csa);
    if (rc != RTK_SUCCESS) {
        RTK_LOGW(TAG, "CSA start failed: %d, connecting without alignment\n", rc);
        rtos_sema_delete(s_csa_done_sema);
        s_csa_done_sema = NULL;
        return;
    }
    rtos_sema_take(s_csa_done_sema, 2000);
    rtos_sema_delete(s_csa_done_sema);
    s_csa_done_sema = NULL;
    s_softap_ap_info.channel = target_ch;
    RTK_LOGI(TAG, "SoftAP aligned to ch%u\n", (unsigned)target_ch);
}

/* ---- Public API ---- */

int claw_wifi_mgr_init(void)
{
    s_state        = CLAW_WIFI_STATE_IDLE;
    s_softap_up    = false;
    s_wifi_on_done = false;
    strlcpy(s_sta_ip, "0.0.0.0", sizeof(s_sta_ip));
    RTK_LOGI(TAG, "init\n");
    return RTK_SUCCESS;
}

int claw_wifi_mgr_start(void)
{
    const claw_config_t *cfg = claw_config_get();

    DiagPrintf("[wifi_mgr] start: configured=%d ssid='%s'\n",
           (int)cfg->wifi.configured, cfg->wifi.ssid);

    if (!cfg->wifi.configured) {
        s_state = CLAW_WIFI_STATE_PROVISIONING;
        return start_softap();
    }

    /* Normal mode: connect STA with saved credentials.
     * autoreconnect must be enabled BEFORE wifi_connect() so that the driver's
     * rtw_reconn_new_conn() saves the credentials (it checks b_enable==1). */
    RTK_LOGI(TAG, "Connecting to '%s'\n", cfg->wifi.ssid);
    wifi_set_autoreconnect(1);
    RTK_LOGI(TAG, "auto-reconnect enabled\n");
    return claw_wifi_mgr_connect_sta(cfg->wifi.ssid, cfg->wifi.password);
}

int claw_wifi_mgr_connect_sta(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') return RTK_ERR_BADARG;

    ensure_wifi_on();

    struct rtw_network_info connect_param;
    _memset(&connect_param, 0, sizeof(connect_param));

    size_t raw_len = strlen(ssid);
    if (raw_len > RTW_ESSID_MAX_SIZE) {
        RTK_LOGW(TAG, "wifi_connect: ssid too long (%zu), truncating to %d\n",
                 raw_len, RTW_ESSID_MAX_SIZE);
        raw_len = RTW_ESSID_MAX_SIZE;
    }
    connect_param.ssid.len = (uint8_t)raw_len;
    _memcpy(connect_param.ssid.val, ssid, connect_param.ssid.len);
    connect_param.ssid.val[connect_param.ssid.len] = '\0';

    if (password && password[0] != '\0') {
        connect_param.security_type = RTW_SECURITY_WPA_WPA2_MIXED_PSK;
        connect_param.password      = (u8 *)password;
        connect_param.password_len  = (int)strlen(password);
    } else {
        connect_param.security_type = RTW_SECURITY_OPEN;
        connect_param.password      = NULL;
        connect_param.password_len  = 0;
    }

    /* Pre-align SoftAP to target AP's channel to eliminate MCC TDMA interference. */
    if (s_softap_up) {
        RTK_LOGI(TAG, "scanning for '%s' channel...\n", ssid);
        u8 target_ch = scan_get_ap_channel(ssid);
        if (target_ch != 0) {
            align_softap_channel(target_ch);
        } else {
            RTK_LOGW(TAG, "scan: '%s' not found, connecting without pre-alignment\n", ssid);
        }
    }

#define CONNECT_MAX_ATTEMPTS 3
    for (int attempt = 0; attempt < CONNECT_MAX_ATTEMPTS; attempt++) {
        s_connect_error[0] = '\0';
        s_state = CLAW_WIFI_STATE_CONNECTING;

        RTK_LOGI(TAG, "wifi_connect ssid=%s attempt=%d/%d\n", ssid, attempt + 1, CONNECT_MAX_ATTEMPTS);
        s32 ret = wifi_connect(&connect_param, 1 /* blocking */);

        if (ret == RTK_SUCCESS)
            break;  /* L2 connected — proceed to DHCP */

        RTK_LOGE(TAG, "wifi_connect attempt %d/%d failed: %d\n", attempt + 1, CONNECT_MAX_ATTEMPTS, ret);
        const char *hint;
        bool no_retry = false;
        switch (-(int)ret) {
        case RTK_ERR_WIFI_CONN_SCAN_FAIL:
            hint = (password && password[0] != '\0')
                   ? "未找到该WiFi（若为开放网络请留空密码）"
                   : "未找到该WiFi（若有密码请填写）";
            break;
        case RTK_ERR_WIFI_CONN_AUTH_FAIL:            hint = "认证失败"; break;
        case RTK_ERR_WIFI_CONN_AUTH_PASSWORD_WRONG:  hint = "密码错误";      no_retry = true; break;
        case RTK_ERR_WIFI_CONN_ASSOC_FAIL:           hint = "关联失败"; break;
        case RTK_ERR_WIFI_CONN_4WAY_HANDSHAKE_FAIL:  hint = "握手失败"; break;
        case RTK_ERR_WIFI_CONN_4WAY_PASSWORD_WRONG:  hint = "密码错误";      no_retry = true; break;
        case RTK_ERR_WIFI_CONN_INVALID_KEY:          hint = "密码格式错误";  no_retry = true; break;
        case RTK_ERR_BUSY:                           hint = "WiFi忙"; break;
        case RTK_ERR_TIMEOUT:                        hint = "连接超时"; break;
        default:                                     hint = "连接失败"; break;
        }

        bool is_last = no_retry || (attempt == CONNECT_MAX_ATTEMPTS - 1);
        if (is_last) {
            DiagSnPrintf(s_connect_error, sizeof(s_connect_error), "%s (%d)", hint, ret);
            s_state = s_softap_up ? CLAW_WIFI_STATE_PROVISIONING : CLAW_WIFI_STATE_DISCONNECTED;
            return RTK_FAIL;
        }
        /* SoftAP stays running; brief pause before next attempt */
        rtos_time_delay_ms(2000);
    }
#undef CONNECT_MAX_ATTEMPTS

    /* L2 connected — request DHCP. SoftAP was pre-aligned to STA channel via CSA,
     * so both are on the same channel and DHCP runs without MCC TDMA interference. */
    if (s_softap_up) {
        struct rtw_wifi_setting sta_setting;
        _memset(&sta_setting, 0, sizeof(sta_setting));
        if (wifi_get_setting(STA_WLAN_INDEX, &sta_setting) == RTK_SUCCESS) {
            RTK_LOGI(TAG, "L2 ok ch%u, SoftAP ch%u — requesting DHCP\n",
                     sta_setting.channel, s_softap_ap_info.channel);
        }
    }

    lwip_request_ip(NETIF_WLAN_STA_INDEX);

    /* Wait for IP assignment (up to 30 s) */
    for (int i = 0; i < 150; i++) {
        rtos_time_delay_ms(200);
        if (is_sta_connected()) {
            update_sta_ip();
            s_state = CLAW_WIFI_STATE_CONNECTED;
            RTK_LOGI(TAG, "STA connected, IP=%s\n", s_sta_ip);
            notify_on_connected();
            return RTK_SUCCESS;
        }
    }

    RTK_LOGE(TAG, "DHCP timeout\n");
    DiagSnPrintf(s_connect_error, sizeof(s_connect_error), "已连接但无法获取 IP，请重试");
    wifi_disconnect();
    s_state = s_softap_up ? CLAW_WIFI_STATE_PROVISIONING : CLAW_WIFI_STATE_DISCONNECTED;
    return RTK_FAIL;
}

const char *claw_wifi_mgr_get_connect_error(void)
{
    return s_connect_error;
}

claw_wifi_state_t claw_wifi_mgr_get_state(void)
{
    /* In concurrent AP+STA mode, probe STA connectivity even when SoftAP is up */
    if (is_sta_connected()) {
        s_state = CLAW_WIFI_STATE_CONNECTED;
        update_sta_ip();
    } else if (s_state == CLAW_WIFI_STATE_CONNECTED) {
        s_state = s_softap_up ? CLAW_WIFI_STATE_PROVISIONING : CLAW_WIFI_STATE_DISCONNECTED;
    }

    if (s_softap_up && s_state != CLAW_WIFI_STATE_CONNECTED) {
        return CLAW_WIFI_STATE_PROVISIONING;
    }
    return s_state;
}

const char *claw_wifi_mgr_get_sta_ip(void)
{
    if (is_sta_connected()) {
        update_sta_ip();
    }
    return s_sta_ip;
}

bool claw_wifi_mgr_is_softap_running(void)
{
    return s_softap_up;
}

/* Polling loop for provisioning mode (8 KB stack — sufficient for claw_config_save).
 * Handles two paths without rebooting:
 *   1. WebUI (/api/wifi/connect): prov_connect_task calls connect_sta() which sets
 *      s_state=CONNECTING then CONNECTED; we wait until configured is also saved.
 *   2. AT+WLCONN: user connects STA externally; we detect it, save credentials,
 *      fire on-connected callbacks, and return — device stays in concurrent AP+STA.
 *
 * Race guard: the AT path is skipped while s_state==CONNECTING or CONNECTED to
 * avoid racing with prov_connect_task (connect_sta sets CONNECTING before calling
 * wifi_connect, so join_status can't be SUCCESS before that window opens). */
static void claw_wifi_poll_provisioning(void)
{
    DiagPrintf("[wifi_mgr] provisioning poll started\n");
    for (;;) {
        rtos_time_delay_ms(2000);

        /* Both paths complete here: STA connected and credentials saved */
        if (is_sta_connected() && claw_config_get()->wifi.configured) {
            DiagPrintf("[wifi_mgr] provisioning: done (connected + configured)\n");
            return;
        }

        /* Skip AT path while WebUI connect is in progress or just completed */
        if (s_state == CLAW_WIFI_STATE_CONNECTING ||
            s_state == CLAW_WIFI_STATE_CONNECTED) {
            continue;
        }

        /* AT+WLCONN path: user connected STA externally via serial command */
        u8 join_status = RTW_JOINSTATUS_UNKNOWN;
        if (wifi_get_join_status(&join_status) != RTK_SUCCESS) continue;
        if (join_status != RTW_JOINSTATUS_SUCCESS) continue;
        if (*(u32 *)lwip_get_ip(NETIF_WLAN_STA_INDEX) == IP_ADDR_INVALID) continue;

        struct rtw_wifi_setting setting;
        if (wifi_get_setting(STA_WLAN_INDEX, &setting) != RTK_SUCCESS) continue;
        if (setting.ssid[0] == '\0') continue;

        update_sta_ip();
        s_state = CLAW_WIFI_STATE_CONNECTED;
        const char *sec_str = (setting.security_type == RTW_SECURITY_OPEN) ? "OPEN" : "WPA2";
        claw_config_set_wifi((const char *)setting.ssid,
                             (const char *)setting.password,
                             sec_str);
        notify_on_connected();
        DiagPrintf("[wifi_mgr] provisioning: AT ssid='%s' saved\n", (char *)setting.ssid);
        return;
    }
}

void claw_wifi_mgr_task_entry(void *param)
{
    (void)param;

    /* Cancel both fast-connect and auto-reconnect immediately.
     * The platform driver triggers fast_connect during wifi_on(STA) which
     * replays the last saved AP from flash — this is independent of
     * auto_reconnect and must be disabled separately.  Stop any in-progress
     * STA scan before we decide whether to start SoftAP or connect. */
    wifi_fast_connect_enable(0);
    wifi_set_autoreconnect(0);
    wifi_disconnect();
    rtos_time_delay_ms(1500);  /* let the in-progress STA scan abort cleanly */

    claw_wifi_mgr_init();
    claw_wifi_mgr_start();
    DiagPrintf("[wifi_mgr] started, state=%d\n", (int)s_state);

    if (s_softap_up) {
        claw_wifi_poll_provisioning();
        /* Provisioning done: enable autoreconnect and register credentials.
         * autoreconnect was kept off during provisioning (SoftAP+STA concurrent);
         * wifi_connect() ran with b_enable==0 so credentials weren't saved then.
         * Call rtw_reconn_new_conn() explicitly to populate rtw_reconn.conn_param. */
        const claw_config_t *prov_cfg = claw_config_get();
        if (prov_cfg->wifi.configured) {
#if CONFIG_AUTO_RECONNECT
            struct rtw_network_info rp;
            _memset(&rp, 0, sizeof(rp));
            size_t sl = strlen(prov_cfg->wifi.ssid);
            if (sl > RTW_ESSID_MAX_SIZE) sl = RTW_ESSID_MAX_SIZE;
            rp.ssid.len = (uint8_t)sl;
            _memcpy(rp.ssid.val, prov_cfg->wifi.ssid, sl);
            if (prov_cfg->wifi.password[0] != '\0') {
                rp.security_type = RTW_SECURITY_WPA_WPA2_MIXED_PSK;
                rp.password      = (u8 *)prov_cfg->wifi.password;
                rp.password_len  = (int)strlen(prov_cfg->wifi.password);
            } else {
                rp.security_type = RTW_SECURITY_OPEN;
            }
            wifi_set_autoreconnect(1);
            rtw_reconn_new_conn(&rp);
#else
            wifi_set_autoreconnect(1);
#endif
            RTK_LOGI(TAG, "auto-reconnect enabled after provisioning\n");
        }
    }

    rtos_task_delete(NULL);
}

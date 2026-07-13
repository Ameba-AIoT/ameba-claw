#include "ameba_soc.h"
#include "claw_wifi_mgr.h"
#include "claw_config.h"
#include "claw_compat.h"

#include "wifi_api.h"
#include "wifi_api_ext.h"
#include "wifi_api_event.h"
#include "wifi_fast_connect.h"
#include "wifi_auto_reconnect.h"
#include "lwip_netconf.h"
#include "basic_types.h"

#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/dhcp.h"
#include "lwip/prot/dhcp.h"
#include "os_wrapper.h"
#include "os_wrapper_critical.h"
#include <stdio.h>
#include <string.h>

#define TAG "claw_wifi_mgr"

/* Returns true when the SDK auto-reconnect mechanism is active — either
 * waiting for its retry timer (b_waiting) or currently executing wifi_connect()
 * (b_ongoing).  Checking both fields closes the race window between timer fire
 * and task start where wifi_is_autoreconnect_ongoing() (which only reads b_ongoing)
 * would falsely return false.
 *
 * The three fields must be read atomically: rtw_reconn_timer_hdl (which runs in
 * the FreeRTOS timer daemon at max priority) clears b_waiting and spawns the
 * reconnect task; the reconnect task then sets b_ongoing.  Between those two
 * writes the timer daemon can preempt this task.  A critical section prevents
 * that preemption so all three reads are seen consistently. */
static bool claw_autoreconn_active(void)
{
#if CONFIG_AUTO_RECONNECT
    rtos_critical_enter(RTOS_CRITICAL_WIFI);
    bool active = rtw_reconn.b_enable &&
                  (rtw_reconn.b_waiting || rtw_reconn.b_ongoing);
    rtos_critical_exit(RTOS_CRITICAL_WIFI);
    return active;
#else
    return false;
#endif
}

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

static volatile claw_wifi_state_t s_state          = CLAW_WIFI_STATE_IDLE;
static volatile bool              s_softap_up       = false;
static bool              s_wifi_on_done   = false;
static char              s_sta_ip[16]     = "0.0.0.0";
static char              s_softap_ssid[32] = "";
static char              s_connect_error[64] = "";
static struct rtw_softap_info s_softap_ap_info;
/* Tick at which s_state last entered DISCONNECTED; 0 = never.
 * Used by the watchdog to decide when to override auto-reconnect. */
static volatile uint32_t s_disconnected_at_ms = 0;
/* Tick at which the watchdog first observed DHCP RENEWING/REBINDING; 0 = healthy. */
static volatile uint32_t s_dhcp_renew_since_ms = 0;

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

/* ---- WiFi platform event handlers ----------------------------------------
 * Registered via event_external_hdl (weak-symbol override in ameba_wificfg.c).
 * These fire in the WiFi driver task — keep them short, no blocking calls.
 * -------------------------------------------------------------------------*/

/* RTW_EVENT_DHCP_STATUS — fires when DHCP assigns an IP.
 * This is the single authoritative "WiFi ready" signal for ALL connection
 * paths: fast-connect at boot, manual wifi_connect(), auto-reconnect.
 * No other code path needs to poll is_sta_connected() for the normal case. */
static void claw_on_dhcp_status(u8 *evt_info)
{
    struct rtw_event_dhcp_status *d = (struct rtw_event_dhcp_status *)evt_info;
    if (d->dhcp_status != DHCP_ADDRESS_ASSIGNED) return;

    update_sta_ip();
    s_state = CLAW_WIFI_STATE_CONNECTED;
    s_dhcp_renew_since_ms = 0;
    RTK_LOGI(TAG, "STA connected (DHCP), IP=%s\n", s_sta_ip);
    notify_on_connected();
}

/* RTW_EVENT_JOIN_STATUS — tracks connection state and persists credentials
 * for AT+WLCONN external connections. */
static void claw_on_join_status(u8 *evt_info)
{
    struct rtw_event_join_status_info *info =
        (struct rtw_event_join_status_info *)evt_info;

    if (info->status == RTW_JOINSTATUS_DISCONNECT) {
        /* The SDK already calls lwip_dhcp_stop() + lwip_netif_set_link_down()
         * in rtw_event.c before this handler runs.  Mirror that in our state
         * so callers see DISCONNECTED rather than a stale CONNECTED. */
        s_state = s_softap_up ? CLAW_WIFI_STATE_PROVISIONING
                               : CLAW_WIFI_STATE_DISCONNECTED;
        strlcpy(s_sta_ip, "0.0.0.0", sizeof(s_sta_ip));
        s_disconnected_at_ms = rtos_time_get_current_system_time_ms();
        RTK_LOGW(TAG, "disconnected (reason=%d)\n",
                 info->priv.disconnect.disconn_reason);
        return;
    }

    if (info->status != RTW_JOINSTATUS_SUCCESS) return;

    struct rtw_wifi_setting setting;
    if (wifi_get_setting(STA_WLAN_INDEX, &setting) != RTK_SUCCESS) return;
    if (setting.ssid[0] == '\0') return;

    /* Only persist when SSID actually changed (skip normal auto-reconnects). */
    const claw_config_t *cfg = claw_config_get();
    if (cfg->wifi.configured &&
            strcmp(cfg->wifi.ssid, (const char *)setting.ssid) == 0)
        return;

    const char *sec = (setting.security_type == RTW_SECURITY_OPEN) ? "OPEN" : "WPA2";
    claw_config_set_wifi((const char *)setting.ssid,
                         (const char *)setting.password, sec);
    RTK_LOGI(TAG, "AT+WLCONN: saved ssid='%s'\n", (char *)setting.ssid);
}

/* Override the __weak default in ameba_wificfg.c. */
struct rtw_event_hdl_func_t event_external_hdl[] = {
    {RTW_EVENT_DHCP_STATUS,  claw_on_dhcp_status},
    {RTW_EVENT_JOIN_STATUS,  claw_on_join_status},
};
u16 array_len_of_event_external_hdl =
    sizeof(event_external_hdl) / sizeof(struct rtw_event_hdl_func_t);

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
    /* Do NOT reset s_state here: claw_on_dhcp_status() (registered via
     * event_external_hdl before wifi_on()) may already have set it to
     * CONNECTED if fast_connect succeeded before this function runs. */
    s_softap_up    = false;
    s_wifi_on_done = true;  /* wifi_on(STA) was called by wifi_init_thread */

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

    /* fast_connect runs via wifi_init_thread (before this task starts).
     * claw_on_dhcp_status() fires on DHCP_ADDRESS_ASSIGNED and sets s_state=CONNECTED.
     *
     * We don't inspect intermediate WiFi states — we only care about the final
     * result.  Wait up to 5 s for fast-connect to deliver an IP.  Do NOT call
     * wifi_disconnect() while fast-connect is still in progress; that would abort
     * a scan/auth/assoc that is about to succeed.  Only if the 5 s window expires
     * do we conclude fast-connect failed and connect manually. */
    if (s_state != CLAW_WIFI_STATE_CONNECTED) {
        RTK_LOGI(TAG, "waiting for fast-connect (up to 5s)\n");
        for (int i = 0; i < 50 && s_state != CLAW_WIFI_STATE_CONNECTED; i++)
            rtos_time_delay_ms(100);
    }

    if (s_state == CLAW_WIFI_STATE_CONNECTED) {
        wifi_set_autoreconnect(1);
        RTK_LOGI(TAG, "fast-connect reused: ip=%s\n", s_sta_ip);
        return RTK_SUCCESS;
    }

    RTK_LOGW(TAG, "fast-connect timed out, connecting manually\n");
    wifi_set_autoreconnect(0);
    wifi_disconnect();
    rtos_time_delay_ms(500);

    RTK_LOGI(TAG, "Connecting to '%s'\n", cfg->wifi.ssid);
    /* Keep fast-connect ENABLED for the manual connect.  The driver persists a
     * fast-connect profile to flash on RTW_JOINSTATUS_SUCCESS only while
     * p_store_fast_connect_info != NULL (set by wifi_fast_connect_enable(1)).
     * The previous code disabled it here, so the successful manual connection
     * never wrote a profile — every reboot then found "Fast connect profile is
     * not exist", waited the full 5s for a fast-connect that could never
     * happen, and fell back to this slow manual path again.  Re-enabling it
     * lets the first manual connect after a credential change seed the profile,
     * so subsequent boots fast-connect and skip the 5s stall. */
    wifi_fast_connect_enable(1);
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
            break;  /* L2 connected — DHCP event may already have fired */

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
            /* Stamp the disconnect time so the watchdog's grace-period timer starts
             * from now, even when no RTW_JOINSTATUS_DISCONNECT event fires (e.g.
             * scan/auth failures never generate a DISCONNECT event). */
            s_disconnected_at_ms = rtos_time_get_current_system_time_ms();
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

    /* DHCP may have already completed inside wifi_connect() — the fast_connect
     * task (or the driver's internal post-connect flow) can trigger the DHCP
     * event before wifi_connect() even returns.  claw_on_dhcp_status() sets
     * s_state = CONNECTED when that happens, so check first.
     *
     * Only call lwip_request_ip() if DHCP has not yet completed — it is the
     * trigger for a manual DHCP request.  Calling it when IP is already
     * assigned is a no-op per platform documentation, but avoiding it keeps
     * the flow clean.
     *
     * Polling s_state is used instead of a semaphore because the RTK OS wrapper
     * forces rtos_sema_take() into non-blocking mode during early-boot FreeRTOS
     * scheduler state checks (pmu_yield_os_check), making the semaphore path
     * unreliable at this stage. */
    if (s_state == CLAW_WIFI_STATE_CONNECTED)
        return RTK_SUCCESS;

    lwip_request_ip(NETIF_WLAN_STA_INDEX);

    for (int i = 0; i < 150; i++) {
        if (s_state == CLAW_WIFI_STATE_CONNECTED) return RTK_SUCCESS;
        rtos_time_delay_ms(200);
    }

    RTK_LOGE(TAG, "DHCP timeout\n");
    DiagSnPrintf(s_connect_error, sizeof(s_connect_error),
                 "已连接但无法获取 IP，请重试");
    wifi_disconnect();
    s_state = s_softap_up ? CLAW_WIFI_STATE_PROVISIONING
                           : CLAW_WIFI_STATE_DISCONNECTED;
    s_disconnected_at_ms = rtos_time_get_current_system_time_ms();
    return RTK_FAIL;
}

const char *claw_wifi_mgr_get_connect_error(void)
{
    return s_connect_error;
}

claw_wifi_state_t claw_wifi_mgr_get_state(void)
{
    if (s_softap_up && s_state != CLAW_WIFI_STATE_CONNECTED)
        return CLAW_WIFI_STATE_PROVISIONING;
    return s_state;
}

const char *claw_wifi_mgr_get_sta_ip(void)
{
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
    /* Block until DHCP fires (via event semaphore) or credentials are saved.
     * claw_on_dhcp_status() handles state + callbacks; claw_on_join_status()
     * persists credentials for AT+WLCONN.  Poll credentials until both are done. */
    DiagPrintf("[wifi_mgr] provisioning poll started\n");
    for (;;) {
        rtos_time_delay_ms(1000);
        if (s_state == CLAW_WIFI_STATE_CONNECTED &&
                claw_config_get()->wifi.configured) {
            DiagPrintf("[wifi_mgr] provisioning: done\n");
            return;
        }
    }
}

void claw_wifi_mgr_task_entry(void *param)
{
    (void)param;

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

    /* ---- Connectivity watchdog ----
     * Handles two failure modes:
     *
     * A) True WiFi disconnect (JOINSTATUS_DISCONNECT fired): claw_on_join_status
     *    sets s_state = DISCONNECTED and records s_disconnected_at_ms.  The SDK's
     *    auto-reconnect handles L2+DHCP recovery.  If auto-reconnect exhausts its
     *    retries and stops, the watchdog takes over after RECONNECT_RETRY_MS.
     *
     * B) AP restarts within no_beacon_disconnect_time (18 s): driver never fires
     *    DISCONNECT.  lwip's T1 renewal (at 50 % of lease, ~30 min) transparently
     *    restores L3.  If T1 and T2 both fail the IP goes to 0.0.0.0 — the
     *    watchdog detects that and re-requests DHCP, falling back to full reconnect.
     *
     * Concurrency: every branch that calls lwip_request_ip() or wifi_connect()
     * checks wifi_is_autoreconnect_ongoing() first.  dhcp_start() is not
     * re-entrant; two concurrent callers on the same netif corrupt struct dhcp. */
#define DHCP_WATCHDOG_INTERVAL_MS  30000u
#define RECONNECT_RETRY_MS         60000u
/* How long the watchdog tolerates DHCP RENEWING state before forcing a restart.
 * T1 fires at 50% of the lease (typically 30 min for a 1-h Android lease).
 * A healthy T1 unicast renewal completes in < 1 s and lwip transitions back to
 * BOUND immediately — the watchdog will see BOUND on the next 30-s poll.
 * If still in RENEWING after 5 min the AP's DHCP server is not responding to
 * unicast; restart DHCP now rather than waiting ~22 more min for T2 to also
 * fail and the IP to go to 0 (which Case B catches). */
#define DHCP_STALL_MS              (5u * 60u * 1000u)

    for (;;) {
        rtos_time_delay_ms(DHCP_WATCHDOG_INTERVAL_MS);

        /* ---- Case A: STA disconnected — SDK auto-reconnect may still be running ----
         * Covers both DISCONNECTED (no SoftAP) and PROVISIONING (SoftAP up but STA
         * dropped) when credentials are already configured.  The latter was previously
         * invisible to Case A, leaving the device stuck after auto-reconnect gave up. */
        {
            bool sta_disconnected =
                (s_state == CLAW_WIFI_STATE_DISCONNECTED) ||
                (s_state == CLAW_WIFI_STATE_PROVISIONING &&
                 s_softap_up && claw_config_get()->wifi.configured);

            if (sta_disconnected) {
                /* Give auto-reconnect its grace period before overriding. */
                if (RTOS_TIME_GET_PASSING_TIME_MS(s_disconnected_at_ms) < RECONNECT_RETRY_MS)
                    continue;
                if (claw_autoreconn_active())
                    continue;
                RTK_LOGW(TAG, "watchdog: auto-reconnect stopped, retrying\n");
                /* Copy credentials before the blocking connect call: claw_config_set_wifi()
                 * can overwrite the static char arrays in s_cfg while wifi_connect() is
                 * reading through the raw pointer, corrupting the auth handshake. */
                char ssid_copy[64], pw_copy[64];
                {
                    const claw_config_t *rc = claw_config_get();
                    strlcpy(ssid_copy, rc->wifi.ssid, sizeof(ssid_copy));
                    strlcpy(pw_copy, rc->wifi.password, sizeof(pw_copy));
                }
                claw_wifi_mgr_connect_sta(ssid_copy, pw_copy);
                /* Stamp after the attempt so RECONNECT_RETRY_MS is measured from
                 * completion, not from before the (potentially long) connect call. */
                s_disconnected_at_ms = rtos_time_get_current_system_time_ms();
                continue;
            }
        }

        if (s_state != CLAW_WIFI_STATE_CONNECTED) continue;

        /* ---- Cases B & C: DHCP health checks ----
         * Read IP and re-check s_state atomically to close the TOCTOU window
         * where a DISCONNECT fires between the CONNECTED check above and here. */
        const ip4_addr_t *cur_ip = netif_ip4_addr(&xnetif[NETIF_WLAN_STA_INDEX]);
        if (s_state != CLAW_WIFI_STATE_CONNECTED) continue;

        if (cur_ip && !ip4_addr_isany(cur_ip)) {
            /* ---- Case C: IP valid but DHCP renewal stuck ----
             * Read lwip's DHCP state machine (dhcp->state is u8_t; single-byte
             * read is atomic on Cortex-M33).
             *
             * RENEWING  (state=5): T1 fired, lwip sending unicast REQUEST.
             *   A healthy renewal completes in < 1s (AP responds to unicast).
             *   If still RENEWING after DHCP_STALL_MS (5 min) the AP is not
             *   responding → force a full DHCP restart now instead of waiting
             *   ~22 more min for T2 to give up and clear the IP.
             *
             * REBINDING (state=4): T2 fired, lwip broadcasting.  T1 already
             *   failed — restart immediately; no grace period needed. */
            struct dhcp *dp = netif_dhcp_data(&xnetif[NETIF_WLAN_STA_INDEX]);
            if (dp) {
                dhcp_state_enum_t dstate = (dhcp_state_enum_t)dp->state;

                if (dstate == DHCP_STATE_RENEWING || dstate == DHCP_STATE_REBINDING) {
                    /* Guard the check-then-set with a critical section: claw_on_dhcp_status()
                     * (wifi event task) writes s_dhcp_renew_since_ms=0 concurrently.
                     * Without the lock, the callback could clear it between our ==0 test
                     * and the store, leaving a stale timestamp that causes a spurious
                     * DHCP restart 5 minutes later on a healthy connection. */
                    rtos_critical_enter(RTOS_CRITICAL_WIFI);
                    if (s_dhcp_renew_since_ms == 0)
                        s_dhcp_renew_since_ms = rtos_time_get_current_system_time_ms();
                    rtos_critical_exit(RTOS_CRITICAL_WIFI);
                } else {
                    rtos_critical_enter(RTOS_CRITICAL_WIFI);
                    s_dhcp_renew_since_ms = 0;
                    rtos_critical_exit(RTOS_CRITICAL_WIFI);
                }

                uint32_t renew_elapsed_ms = RTOS_TIME_GET_PASSING_TIME_MS(s_dhcp_renew_since_ms);
                bool stalled =
                    (dstate == DHCP_STATE_REBINDING) ||
                    (dstate == DHCP_STATE_RENEWING && renew_elapsed_ms >= DHCP_STALL_MS);

                if (stalled && !claw_autoreconn_active()) {
                    RTK_LOGW(TAG, "watchdog: DHCP stalled (state=%u, %u min), restarting\n",
                             (unsigned)dstate,
                             (unsigned)(renew_elapsed_ms / 60000u));
                    s_dhcp_renew_since_ms = 0;
                    uint8_t dhcp_ret = lwip_request_ip(NETIF_WLAN_STA_INDEX);
                    if (dhcp_ret != DHCP_ADDRESS_ASSIGNED) {
                        if (!claw_autoreconn_active()) {
                            RTK_LOGE(TAG, "watchdog: DHCP restart failed, reconnecting\n");
                            s_state = s_softap_up ? CLAW_WIFI_STATE_PROVISIONING
                                                  : CLAW_WIFI_STATE_DISCONNECTED;
                            strlcpy(s_sta_ip, "0.0.0.0", sizeof(s_sta_ip));
                            char ssid_c[64], pw_c[64];
                            {
                                const claw_config_t *cfg = claw_config_get();
                                strlcpy(ssid_c, cfg->wifi.ssid, sizeof(ssid_c));
                                strlcpy(pw_c, cfg->wifi.password, sizeof(pw_c));
                            }
                            claw_wifi_mgr_connect_sta(ssid_c, pw_c);
                            s_disconnected_at_ms = rtos_time_get_current_system_time_ms();
                        }
                    }
                }
            }
            continue;  /* IP still valid — skip Case B */
        }

        /* ---- Case B: connected but IP gone (lwip DHCP lease expired) ---- */

        /* dhcp_start() (called by lwip_request_ip) is not re-entrant: two
         * concurrent callers on the same netif corrupt struct dhcp. */
        if (claw_autoreconn_active()) continue;

        RTK_LOGW(TAG, "watchdog: IP lost, requesting DHCP\n");
        uint8_t ret = lwip_request_ip(NETIF_WLAN_STA_INDEX);
        if (ret != DHCP_ADDRESS_ASSIGNED) {
            if (claw_autoreconn_active()) continue;
            RTK_LOGE(TAG, "watchdog: DHCP failed, reconnecting\n");
            s_state = s_softap_up ? CLAW_WIFI_STATE_PROVISIONING
                                  : CLAW_WIFI_STATE_DISCONNECTED;
            strlcpy(s_sta_ip, "0.0.0.0", sizeof(s_sta_ip));
            char ssid_b[64], pw_b[64];
            {
                const claw_config_t *cfg = claw_config_get();
                strlcpy(ssid_b, cfg->wifi.ssid, sizeof(ssid_b));
                strlcpy(pw_b, cfg->wifi.password, sizeof(pw_b));
            }
            claw_wifi_mgr_connect_sta(ssid_b, pw_b);
            s_disconnected_at_ms = rtos_time_get_current_system_time_ms();
        }
    }
}

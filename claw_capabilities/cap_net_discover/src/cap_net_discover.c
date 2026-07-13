/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * cap_net_discover — UDP broadcast peer discovery.
 *
 * Two modes:
 *
 *   One-shot  (net_discover_peer):
 *     Blocks until one peer found or timeout.  Returns {"peer_ip":"..."}.
 *     Suitable for LLM direct queries and simple one-off scripts.
 *
 *   Persistent service  (net_discover_start / net_discover_stop):
 *     Starts a background FreeRTOS task that continuously broadcasts and
 *     listens.  On peer appearance / disappearance it both:
 *       1) Publishes system events (for C-layer subscribers):
 *            "peer_discovered"  payload: {"peer_ip":"x.x.x.x"}
 *            "peer_lost"        payload: {"peer_ip":"x.x.x.x"}
 *       2) Calls optional caps directly (for Lua scripts, no evr.on needed):
 *            on_found_cap  — called with {"peer_ip":"x.x.x.x", <on_found_args>}
 *            on_lost_cap   — called with {"peer_ip":"x.x.x.x"}
 *
 *     Lua usage (minimal):
 *       local cap   = require("cap")
 *       local cjson = require("cjson")
 *       cap.call("net_discover_start", cjson.encode({
 *         port        = 9002,
 *         keepalive_s = 8,
 *         on_found_cap  = "<cap_to_call_when_peer_appears>",
 *         on_found_args = { ... base args, peer_ip injected at call time ... },
 *         on_lost_cap   = "<cap_to_call_when_peer_leaves>",
 *       }))
 *       -- returns immediately; background service handles the rest
 */

#include "cap_net_discover.h"
#include "claw_cap.h"
#include "claw_event_publisher.h"
#include <cJSON.h>
#include "ameba_soc.h"
#include "os_wrapper.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "claw_wifi_mgr.h"
#include <string.h>
#include <stdlib.h>

#define TAG                     "cap_disc"
#define DISC_DEFAULT_PORT       9002
#define DISC_MAGIC              "AMEBA_WALKIE"
#define DISC_MAGIC_LEN          12
#define DISC_INTERVAL_MS        2000    /* broadcast cadence */
#define DISC_RECV_TIMEOUT_MS    300     /* recvfrom poll interval */
#define DISC_DEFAULT_TIMEOUT    600     /* one-shot default timeout (s) */
#define DISC_EXTRA_BC           5       /* extra broadcasts after one-shot find */
#define DISC_EXTRA_BC_MS        400
#define DISC_DEFAULT_KEEPALIVE  8       /* persistent: seconds without hear → lost */
#define DISC_CAP_ID_MAX         48
#define DISC_CAP_ARGS_MAX       256

/* One-shot timeout tiers */
#define DISC_LLM_MAX_TIMEOUT_S  60
#define DISC_MAX_TIMEOUT        3600

/* ---- Persistent-service state ------------------------------------------ */

static struct {
    rtos_mutex_t    mutex;
    rtos_task_t     task;               /* NULL when service not running */
    volatile int    running;            /* task loop condition */
    volatile int    wifi_reconnected;   /* set by wifi_on_connected callback */
    uint16_t        port;
} s_svc;

static void disc_on_wifi_connected(void)
{
    s_svc.wifi_reconnected = 1;
}

/* ---- Shared helper: wait for a valid STA IP (up to 30 s) --------------- */

static int wait_for_ip(char *buf, size_t bufsz)
{
    for (int i = 0; i < 60; i++) {
        const char *ip = claw_wifi_mgr_get_sta_ip();
        if (ip && ip[0] && strcmp(ip, "0.0.0.0") != 0) {
            strlcpy(buf, ip, bufsz);
            return 0;
        }
        rtos_time_delay_ms(500);
    }
    return -1;
}

/* ---- Helper: merge peer_ip into a base-args JSON ----------------------- */

/* Builds {"peer_ip":"<ip>", ...base_args_fields...} using cJSON so the
 * injection is always structurally correct.
 * buf must be DISC_CAP_ARGS_MAX bytes.  Falls back to snprintf on OOM. */
static void build_peer_args(char *buf, size_t bufsz,
                             const char *peer_ip, const char *base_args)
{
    cJSON *out = NULL;

    /* Parse base_args if non-empty — accept both {} and "" */
    if (base_args && base_args[0] != '\0' && strcmp(base_args, "{}") != 0) {
        out = cJSON_Parse(base_args);
    }
    if (!out) {
        out = cJSON_CreateObject();
    }
    if (!out) {
        /* OOM fallback — peer_ip is always a dotted-decimal string, safe */
        DiagSnPrintf(buf, bufsz, "{\"peer_ip\":\"%s\"}", peer_ip);
        return;
    }

    /* Inject peer_ip as the first field (replace if already present) */
    cJSON_DeleteItemFromObject(out, "peer_ip");
    cJSON_AddStringToObject(out, "peer_ip", peer_ip);

    char *s = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (s) {
        strlcpy(buf, s, bufsz);
        free(s);
    } else {
        DiagSnPrintf(buf, bufsz, "{\"peer_ip\":\"%s\"}", peer_ip);
    }
}

/* ---- Persistent-service background task -------------------------------- */

typedef struct {
    uint16_t port;
    uint32_t keepalive_ms;
    char     on_found_cap[DISC_CAP_ID_MAX];
    char     on_found_args[DISC_CAP_ARGS_MAX];  /* base args; peer_ip added at runtime */
    char     on_lost_cap[DISC_CAP_ID_MAX];
} svc_args_t;

static void fire_cap(const char *cap_id, const char *args_json)
{
    if (!cap_id || !cap_id[0]) return;
    char *out = NULL;
    claw_cap_call_context_t ctx = {0};
    claw_cap_call(cap_id, args_json && args_json[0] ? args_json : "{}", &ctx, &out);
    free(out);
}

/* Open broadcast + listen sockets bound to the current IP.
 * Returns 0 on success; caller must lwip_close() both on failure path. */
static int svc_open_sockets(uint16_t port, int *bc_out, int *ls_out)
{
    int bc = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (bc < 0) return -1;
    int yes = 1;
    lwip_setsockopt(bc, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    int ls = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (ls < 0) { lwip_close(bc); return -1; }
    int reuse = 1;
    lwip_setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in la = {0};
    la.sin_family      = AF_INET;
    la.sin_port        = htons(port);
    la.sin_addr.s_addr = htonl(INADDR_ANY);
    if (lwip_bind(ls, (struct sockaddr *)&la, sizeof(la)) < 0) {
        lwip_close(bc); lwip_close(ls); return -1;
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = DISC_RECV_TIMEOUT_MS * 1000};
    lwip_setsockopt(ls, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    *bc_out = bc;
    *ls_out = ls;
    return 0;
}

static void discover_svc_task(void *arg)
{
    svc_args_t *a = (svc_args_t *)arg;
    uint16_t port         = a->port;
    uint32_t keepalive_ms = a->keepalive_ms;
    char on_found_cap[DISC_CAP_ID_MAX];
    char on_found_args[DISC_CAP_ARGS_MAX];
    char on_lost_cap[DISC_CAP_ID_MAX];
    strncpy(on_found_cap,  a->on_found_cap,  sizeof(on_found_cap)  - 1);
    strncpy(on_found_args, a->on_found_args, sizeof(on_found_args) - 1);
    strncpy(on_lost_cap,   a->on_lost_cap,   sizeof(on_lost_cap)   - 1);
    on_found_cap[sizeof(on_found_cap)-1]   = '\0';
    on_found_args[sizeof(on_found_args)-1] = '\0';
    on_lost_cap[sizeof(on_lost_cap)-1]     = '\0';
    free(a);

    char my_ip[16] = "";
    char peer_ip[16] = "";
    uint32_t last_seen = 0;
    uint32_t last_bc   = 0;
    int bc = -1, ls = -1;

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(port);
    inet_aton("255.255.255.255", &dst.sin_addr);

    struct sockaddr_in uc_dst = {0};
    uc_dst.sin_family = AF_INET;
    uc_dst.sin_port   = htons(port);

    /* Clear any pending reconnect flag accumulated before this task started.
     * disc_on_wifi_connected() fires (and sets the flag) before the scheduler
     * triggers net_discover_start, so by the time we reach here the flag is
     * already 1.  We handle the initial open below; the flag is only meaningful
     * for reconnects that happen AFTER the task is running. */
    s_svc.wifi_reconnected = 0;

    /* Initial socket open: if WiFi already has an IP (common case: scheduler
     * fires after wifi_connected), open immediately.  Otherwise the
     * wifi_reconnected flag will be set by disc_on_wifi_connected() and the
     * loop below will handle it — no blocking wait needed. */
    {
        const char *cur_ip = claw_wifi_mgr_get_sta_ip();
        if (cur_ip && cur_ip[0] && strcmp(cur_ip, "0.0.0.0") != 0) {
            strlcpy(my_ip, cur_ip, sizeof(my_ip));
            if (svc_open_sockets(port, &bc, &ls) == 0) {
                last_bc = 0;
                RTK_LOGI(TAG, "svc: started my_ip=%s port=%u keepalive=%ums"
                         " on_found=%s on_lost=%s\n",
                         my_ip, (unsigned)port, (unsigned)keepalive_ms,
                         on_found_cap[0] ? on_found_cap : "(event-only)",
                         on_lost_cap[0]  ? on_lost_cap  : "(event-only)");
            } else {
                RTK_LOGE(TAG, "svc: socket open failed, will retry on reconnect\n");
                my_ip[0] = '\0';
            }
        } else {
            RTK_LOGI(TAG, "svc: no IP yet, waiting for wifi_connected\n");
        }
    }

    while (s_svc.running) {

        /* ---- WiFi reconnect event ---------------------------------------- *
         * disc_on_wifi_connected() fires whenever WiFi gets a new IP.
         * We only rebuild sockets when this flag is set — no polling. *
         * ------------------------------------------------------------------ */
        if (s_svc.wifi_reconnected) {
            s_svc.wifi_reconnected = 0;

            const char *cur_ip = claw_wifi_mgr_get_sta_ip();
            int ip_valid = cur_ip && cur_ip[0] &&
                           strcmp(cur_ip, "0.0.0.0") != 0;

            /* Close stale sockets regardless */
            if (bc >= 0) { lwip_close(bc); bc = -1; }
            if (ls >= 0) { lwip_close(ls); ls = -1; }

            /* Fire on_lost_cap if a peer was active */
            if (peer_ip[0] != '\0') {
                RTK_LOGI(TAG, "svc: peer_lost %s (wifi reconnect)\n", peer_ip);
                char ev_payload[48];
                DiagSnPrintf(ev_payload, sizeof(ev_payload),
                             "{\"peer_ip\":\"%s\"}", peer_ip);
                claw_event_dispatcher_publish_trigger(
                    "cap_net_discover", "peer_lost", peer_ip, ev_payload);
                if (on_lost_cap[0]) {
                    char call_args[DISC_CAP_ARGS_MAX];
                    DiagSnPrintf(call_args, sizeof(call_args),
                                 "{\"peer_ip\":\"%s\"}", peer_ip);
                    fire_cap(on_lost_cap, call_args);
                }
                peer_ip[0] = '\0';
                last_seen  = 0;
                memset(&uc_dst.sin_addr, 0, sizeof(uc_dst.sin_addr));
            }

            if (ip_valid) {
                strlcpy(my_ip, cur_ip, sizeof(my_ip));
                if (svc_open_sockets(port, &bc, &ls) == 0) {
                    last_bc = 0;
                    RTK_LOGI(TAG, "svc: reconnected my_ip=%s\n", my_ip);
                } else {
                    RTK_LOGE(TAG, "svc: socket reopen failed\n");
                    my_ip[0] = '\0';
                }
            }
        }

        /* No sockets yet — sleep until next reconnect event */
        if (bc < 0 || ls < 0) {
            rtos_time_delay_ms(200);
            continue;
        }

        uint32_t now = rtos_time_get_current_system_time_ms();

        /* Broadcast (discovery phase) or unicast (keepalive phase) */
        if (now - last_bc >= (uint32_t)DISC_INTERVAL_MS) {
            if (peer_ip[0] != '\0') {
                lwip_sendto(bc, DISC_MAGIC, DISC_MAGIC_LEN, 0,
                            (struct sockaddr *)&uc_dst, sizeof(uc_dst));
            } else {
                lwip_sendto(bc, DISC_MAGIC, DISC_MAGIC_LEN, 0,
                            (struct sockaddr *)&dst, sizeof(dst));
            }
            last_bc = now;
        }

        /* Receive */
        char             buf[32];
        struct sockaddr_in from;
        socklen_t        flen = sizeof(from);
        int n = lwip_recvfrom(ls, buf, sizeof(buf), 0,
                              (struct sockaddr *)&from, &flen);
        if (n >= DISC_MAGIC_LEN && memcmp(buf, DISC_MAGIC, DISC_MAGIC_LEN) == 0) {
            char sender[16];
            inet_ntoa_r(from.sin_addr, sender, sizeof(sender));
            if (strcmp(sender, my_ip) != 0) {
                if (peer_ip[0] == '\0') {
                    /* New peer appeared */
                    strlcpy(peer_ip, sender, sizeof(peer_ip));
                    last_seen = now;
                    inet_aton(peer_ip, &uc_dst.sin_addr);
                    RTK_LOGI(TAG, "svc: peer_discovered %s\n", peer_ip);

                    char ev_payload[48];
                    DiagSnPrintf(ev_payload, sizeof(ev_payload),
                                 "{\"peer_ip\":\"%s\"}", peer_ip);
                    claw_event_dispatcher_publish_trigger(
                        "cap_net_discover", "peer_discovered", peer_ip, ev_payload);

                    if (on_found_cap[0]) {
                        char call_args[DISC_CAP_ARGS_MAX];
                        build_peer_args(call_args, sizeof(call_args),
                                        peer_ip, on_found_args);
                        fire_cap(on_found_cap, call_args);
                    }
                } else if (strcmp(peer_ip, sender) == 0) {
                    last_seen = now;
                }
            }
        }

        /* Keepalive check */
        if (peer_ip[0] != '\0' &&
            (int32_t)(now - last_seen) > (int32_t)keepalive_ms) {
            RTK_LOGI(TAG, "svc: peer_lost %s\n", peer_ip);

            char ev_payload[48];
            DiagSnPrintf(ev_payload, sizeof(ev_payload),
                         "{\"peer_ip\":\"%s\"}", peer_ip);
            claw_event_dispatcher_publish_trigger(
                "cap_net_discover", "peer_lost", peer_ip, ev_payload);

            if (on_lost_cap[0]) {
                char call_args[DISC_CAP_ARGS_MAX];
                DiagSnPrintf(call_args, sizeof(call_args),
                             "{\"peer_ip\":\"%s\"}", peer_ip);
                fire_cap(on_lost_cap, call_args);
            }

            peer_ip[0] = '\0';
            last_seen  = 0;
            memset(&uc_dst.sin_addr, 0, sizeof(uc_dst.sin_addr));
        }
    }

    if (bc >= 0) lwip_close(bc);
    if (ls >= 0) lwip_close(ls);
    RTK_LOGI(TAG, "svc: stopped\n");

    rtos_mutex_take(s_svc.mutex, 0xFFFFFFFFUL);
    s_svc.task    = NULL;
    s_svc.running = 0;
    rtos_mutex_give(s_svc.mutex);
    rtos_task_delete(NULL);
}

/* ---- Cap: net_discover_start ------------------------------------------- */

static int execute_discover_start(const char *input_json,
                                  const claw_cap_call_context_t *ctx,
                                  char **output)
{
    (void)ctx;

    uint16_t port        = DISC_DEFAULT_PORT;
    uint32_t keepalive_s = DISC_DEFAULT_KEEPALIVE;
    const char *on_found_cap  = "";
    const char *on_found_args = "";
    const char *on_lost_cap   = "";
    char *on_found_args_heap  = NULL;  /* non-NULL if we serialised an object */

    cJSON *j = cJSON_Parse(input_json ? input_json : "{}");
    if (j) {
        cJSON *jp  = cJSON_GetObjectItem(j, "port");
        cJSON *jk  = cJSON_GetObjectItem(j, "keepalive_s");
        cJSON *jfc = cJSON_GetObjectItem(j, "on_found_cap");
        cJSON *jfa = cJSON_GetObjectItem(j, "on_found_args");
        cJSON *jlc = cJSON_GetObjectItem(j, "on_lost_cap");
        if (jp)  port        = (uint16_t)cJSON_GetNumberValue(jp);
        if (jk)  keepalive_s = (uint32_t)cJSON_GetNumberValue(jk);
        if (jfc && cJSON_IsString(jfc)) on_found_cap  = jfc->valuestring;
        /* on_found_args: accept both string and object.
         * String: caller already serialised it — use as-is.
         * Object: serialise it here so the service stores a JSON string. */
        if (jfa) {
            if (cJSON_IsString(jfa)) {
                on_found_args = jfa->valuestring;
            } else if (cJSON_IsObject(jfa)) {
                on_found_args_heap = cJSON_PrintUnformatted(jfa);
                if (on_found_args_heap) on_found_args = on_found_args_heap;
            }
        }
        if (jlc && cJSON_IsString(jlc)) on_lost_cap   = jlc->valuestring;
        /* Note: cJSON_Delete(j) after we copy strings below */
    }
    if (keepalive_s < 2)   keepalive_s = 2;
    if (keepalive_s > 300) keepalive_s = 300;

    /* Validate: discovery port must differ from any port in on_found_args.
     * Both bind their own UDP socket on the same device; sharing a port splits
     * incoming packets unpredictably and silently breaks both services. */
    if (on_found_args && on_found_args[0]) {
        cJSON *jargs = cJSON_Parse(on_found_args);
        if (jargs) {
            cJSON *jp2 = cJSON_GetObjectItem(jargs, "port");
            if (jp2 && (uint16_t)cJSON_GetNumberValue(jp2) == port) {
                cJSON_Delete(jargs);
                free(on_found_args_heap);
                if (j) cJSON_Delete(j);
                return claw_cap_set_output(output,
                    "{\"error\":\"discovery port must differ from audio port in on_found_args — "
                    "both bind their own UDP socket and sharing a port breaks both services. "
                    "Use a different port for net_discover_start (e.g. 9002) and keep the "
                    "audio port in on_found_args (e.g. 9000).\"}");
            }
            cJSON_Delete(jargs);
        }
    }

    rtos_mutex_take(s_svc.mutex, 0xFFFFFFFFUL);

    if (s_svc.task != NULL) {
        if (s_svc.port == port) {
            rtos_mutex_give(s_svc.mutex);
            free(on_found_args_heap);
            if (j) cJSON_Delete(j);
            return claw_cap_set_output(output,
                "{\"ok\":true,\"already_running\":true}");
        }
        rtos_mutex_give(s_svc.mutex);
        free(on_found_args_heap);
        if (j) cJSON_Delete(j);
        return claw_cap_set_output(output,
            "{\"error\":\"already running on a different port — call net_discover_stop first\"}");
    }

    svc_args_t *a = calloc(1, sizeof(svc_args_t));
    if (!a) {
        rtos_mutex_give(s_svc.mutex);
        free(on_found_args_heap);
        if (j) cJSON_Delete(j);
        return claw_cap_set_output(output, "{\"error\":\"OOM\"}");
    }
    a->port        = port;
    a->keepalive_ms = keepalive_s * 1000u;
    strncpy(a->on_found_cap,  on_found_cap,  sizeof(a->on_found_cap)  - 1);
    strncpy(a->on_found_args, on_found_args, sizeof(a->on_found_args) - 1);
    strncpy(a->on_lost_cap,   on_lost_cap,   sizeof(a->on_lost_cap)   - 1);
    free(on_found_args_heap);  /* NULL-safe; frees serialised object if allocated */
    if (j) cJSON_Delete(j);   /* done with JSON strings */

    s_svc.port    = port;
    s_svc.running = 1;

    int ret = rtos_task_create(&s_svc.task, "disc_svc", discover_svc_task,
                               a, 4096, 1);
    if (ret != RTK_SUCCESS) {
        s_svc.running = 0;
        free(a);
        rtos_mutex_give(s_svc.mutex);
        return claw_cap_set_output(output, "{\"error\":\"task create failed\"}");
    }

    rtos_mutex_give(s_svc.mutex);
    RTK_LOGI(TAG, "svc: start requested port=%u keepalive=%us\n",
             (unsigned)port, (unsigned)keepalive_s);
    return claw_cap_set_output(output,
        "{\"ok\":true,\"port\":%u,\"keepalive_s\":%u}",
        (unsigned)port, (unsigned)keepalive_s);
}

/* ---- Cap: net_discover_stop -------------------------------------------- */

static int execute_discover_stop(const char *input_json,
                                 const claw_cap_call_context_t *ctx,
                                 char **output)
{
    (void)input_json;
    (void)ctx;

    rtos_mutex_take(s_svc.mutex, 0xFFFFFFFFUL);
    if (s_svc.task == NULL) {
        rtos_mutex_give(s_svc.mutex);
        return claw_cap_set_output(output,
            "{\"ok\":true,\"was_running\":false}");
    }
    s_svc.running = 0;
    rtos_mutex_give(s_svc.mutex);

    /* Wait up to 2 s for the task to exit */
    for (int i = 0; i < 20; i++) {
        rtos_time_delay_ms(100);
        rtos_mutex_take(s_svc.mutex, 0xFFFFFFFFUL);
        int done = (s_svc.task == NULL);
        rtos_mutex_give(s_svc.mutex);
        if (done) break;
    }

    RTK_LOGI(TAG, "svc: stop requested\n");
    return claw_cap_set_output(output, "{\"ok\":true,\"was_running\":true}");
}

/* ---- Cap: net_discover_peer (one-shot, kept for backward compat) -------- */

static int execute_discover_peer(const char *input_json,
                                 const claw_cap_call_context_t *ctx,
                                 char **output)
{
    uint16_t disc_port = DISC_DEFAULT_PORT;
    int      timeout_s = DISC_DEFAULT_TIMEOUT;

    cJSON *j = cJSON_Parse(input_json ? input_json : "{}");
    if (j) {
        cJSON *jp = cJSON_GetObjectItem(j, "port");
        cJSON *jt = cJSON_GetObjectItem(j, "timeout_s");
        if (jp) disc_port = (uint16_t)cJSON_GetNumberValue(jp);
        if (jt) timeout_s = (int)cJSON_GetNumberValue(jt);
        cJSON_Delete(j);
    }
    if (timeout_s < 1)              timeout_s = 1;
    if (timeout_s > DISC_MAX_TIMEOUT) timeout_s = DISC_MAX_TIMEOUT;
    if (ctx && ctx->caller == CLAW_CAP_CALLER_LLM &&
            timeout_s > DISC_LLM_MAX_TIMEOUT_S) {
        timeout_s = DISC_LLM_MAX_TIMEOUT_S;
    }

    /* Refuse if the persistent service already owns this port */
    rtos_mutex_take(s_svc.mutex, 0xFFFFFFFFUL);
    int svc_owns_port = (s_svc.task != NULL && s_svc.port == disc_port);
    rtos_mutex_give(s_svc.mutex);
    if (svc_owns_port) {
        return claw_cap_set_output(output,
            "{\"error\":\"persistent discovery service is running on this port — "
            "use net_discover_start with on_found_cap instead\"}");
    }

    char my_ip[16] = "";
    if (wait_for_ip(my_ip, sizeof(my_ip)) != 0)
        return claw_cap_set_output(output,
            "{\"error\":\"no_ip\",\"msg\":\"WiFi not connected\"}");

    int bc = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (bc < 0) return claw_cap_set_output(output, "{\"error\":\"socket_failed\"}");
    int yes = 1;
    lwip_setsockopt(bc, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    int ls = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (ls < 0) { lwip_close(bc); return claw_cap_set_output(output, "{\"error\":\"socket_failed\"}"); }
    int reuse = 1;
    lwip_setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in la = {0};
    la.sin_family      = AF_INET;
    la.sin_port        = htons(disc_port);
    la.sin_addr.s_addr = htonl(INADDR_ANY);
    if (lwip_bind(ls, (struct sockaddr *)&la, sizeof(la)) < 0) {
        RTK_LOGE(TAG, "bind port %u failed\n", (unsigned)disc_port);
        lwip_close(bc); lwip_close(ls);
        return claw_cap_set_output(output,
            "{\"error\":\"bind_failed\",\"port\":%u}", (unsigned)disc_port);
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = DISC_RECV_TIMEOUT_MS * 1000};
    lwip_setsockopt(ls, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst = {0};
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(disc_port);
    inet_aton("255.255.255.255", &dst.sin_addr);

    RTK_LOGI(TAG, "Discovery: my_ip=%s port=%u timeout=%ds\n",
             my_ip, (unsigned)disc_port, timeout_s);

    char     peer_ip[16] = "";
    uint32_t last_bc     = 0;
    int      max_iter    = (timeout_s * 1000) / DISC_INTERVAL_MS;
    int      iter        = 0;

    while (!peer_ip[0] && iter < max_iter) {
        uint32_t now = rtos_time_get_current_system_time_ms();
        if (now - last_bc >= (uint32_t)DISC_INTERVAL_MS) {
            lwip_sendto(bc, DISC_MAGIC, DISC_MAGIC_LEN, 0,
                        (struct sockaddr *)&dst, sizeof(dst));
            last_bc = now;
            iter++;
        }
        char buf[32]; struct sockaddr_in from; socklen_t flen = sizeof(from);
        int n = lwip_recvfrom(ls, buf, sizeof(buf), 0,
                              (struct sockaddr *)&from, &flen);
        if (n >= DISC_MAGIC_LEN && memcmp(buf, DISC_MAGIC, DISC_MAGIC_LEN) == 0) {
            char sender[16];
            inet_ntoa_r(from.sin_addr, sender, sizeof(sender));
            if (strcmp(sender, my_ip) != 0)
                strlcpy(peer_ip, sender, sizeof(peer_ip));
        }
    }

    if (peer_ip[0]) {
        RTK_LOGI(TAG, "Discovery: found peer %s\n", peer_ip);
        for (int k = 0; k < DISC_EXTRA_BC; k++) {
            lwip_sendto(bc, DISC_MAGIC, DISC_MAGIC_LEN, 0,
                        (struct sockaddr *)&dst, sizeof(dst));
            rtos_time_delay_ms(DISC_EXTRA_BC_MS);
        }
    }

    lwip_close(bc);
    lwip_close(ls);

    if (!peer_ip[0]) {
        RTK_LOGW(TAG, "Discovery: timed out after %d broadcasts\n", iter);
        return claw_cap_set_output(output,
            "{\"error\":\"timeout\",\"msg\":\"no peer found after %d broadcasts\"}", iter);
    }
    return claw_cap_set_output(output,
        "{\"peer_ip\":\"%s\",\"msg\":\"peer discovered via UDP broadcast\"}", peer_ip);
}

/* ---- Capability descriptors -------------------------------------------- */

static const claw_cap_descriptor_t s_caps[] = {
    {
        .id          = "net_discover_start",
        .name        = "net_discover_start",
        .family      = "network",
        .description =
            "Start a persistent UDP broadcast/listen background service. "
            "Continuously broadcasts and listens until net_discover_stop is called. "
            "When a peer appears, calls on_found_cap with peer_ip injected into on_found_args. "
            "When peer disappears (no broadcast for keepalive_s seconds), calls on_lost_cap. "
            "Returns immediately — service runs in background. "
            "Idempotent: calling again on the same port returns already_running=true. "
            "IMPORTANT: the discovery port must differ from any port used inside on_found_args "
            "(e.g. audio port). Both bind their own UDP socket on the same device; sharing a "
            "port splits incoming packets unpredictably and breaks both services. "
            "Use this (not net_discover_peer) whenever you need to react to peer appearance or disappearance over time.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"port\":{\"type\":\"integer\","
                      "\"description\":\"UDP broadcast port (default 9002)\"},"
            "\"keepalive_s\":{\"type\":\"integer\","
                             "\"description\":\"Seconds without a broadcast before peer is considered lost (default 8)\"},"
            "\"on_found_cap\":{\"type\":\"string\","
                              "\"description\":\"Cap to call when a peer is discovered. peer_ip is merged into args automatically.\"},"
            "\"on_found_args\":{\"description\":\"Base args for on_found_cap. Pass as object or JSON string. peer_ip is injected at call time.\"},"
            "\"on_lost_cap\":{\"type\":\"string\","
                             "\"description\":\"Cap to call when peer is lost. Called with {\\\"peer_ip\\\":\\\"...\\\"}.\"}  "
            "}}",
        .execute = execute_discover_start,
    },
    {
        .id          = "net_discover_stop",
        .name        = "net_discover_stop",
        .family      = "network",
        .description =
            "Stop the persistent peer-discovery background service. "
            "No-op if service is not running.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = execute_discover_stop,
    },
    {
        .id          = "net_discover_peer",
        .name        = "net_discover_peer",
        .family      = "network",
        .description =
            "One-shot peer discovery: broadcasts and listens until one peer is found or timeout. "
            "Returns {\"peer_ip\":\"x.x.x.x\"} on success or {\"error\":\"timeout\"} and then stops — "
            "the local device stops broadcasting as soon as this call returns. "
            "Constraint: BOTH sides must be broadcasting at the same time to find each other. "
            "If the remote device boots later or reboots, this device is no longer broadcasting "
            "and they will never find each other. "
            "Use net_discover_start instead when either side may come online at any time.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"port\":{\"type\":\"integer\","
                      "\"description\":\"UDP broadcast port (default 9002)\"},"
            "\"timeout_s\":{\"type\":\"integer\","
                           "\"description\":\"Max wait time in seconds (default 600, max 3600). "
                             "Capped at 60 s when called as a direct LLM tool.\"}"
            "}}",
        .execute = execute_discover_peer,
    },
};

static const claw_cap_group_t s_group = {
    .group_id         = "net_discover",
    .plugin_name      = "cap_net_discover",
    .version          = "2",
    .descriptors      = s_caps,
    .descriptor_count = 3,
};

/* ---- Public API --------------------------------------------------------- */

int cap_net_discover_init(void)
{
    memset(&s_svc, 0, sizeof(s_svc));
    int err = rtos_mutex_create(&s_svc.mutex);
    if (err != RTK_SUCCESS) {
        RTK_LOGE(TAG, "mutex create failed\n");
        return RTK_FAIL;
    }
    /* Register once at init — fires on every WiFi reconnect, including the
     * first boot connect.  The service task checks wifi_reconnected flag. */
    claw_wifi_mgr_register_on_connected(disc_on_wifi_connected);
    return claw_cap_register_group(&s_group);
}

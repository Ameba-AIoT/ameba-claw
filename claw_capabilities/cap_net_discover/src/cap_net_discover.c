/*
 * SPDX-FileCopyrightText: 2026 Realtek Semiconductor Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * cap_net_discover — UDP broadcast peer discovery as a standalone capability.
 *
 * Registers one capability:
 *   net_discover_peer
 *     Input:  {"port": 9002, "timeout_s": 60}
 *     Output: {"peer_ip": "192.168.x.x"} | {"error": "timeout"|"no_ip"|"socket_failed"}
 *
 * The execute function blocks in the caller's task until a peer is found or
 * timeout_s seconds elapse.  Broadcasts "AMEBA_WALKIE" every 2 s and listens
 * for the same magic from a different IP on the same subnet.
 */

#include "cap_net_discover.h"
#include "claw_cap.h"
#include <cJSON.h>
#include "ameba_soc.h"
#include "os_wrapper.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "claw_wifi_mgr.h"
#include <string.h>
#include <stdlib.h>

#define TAG                  "cap_disc"
#define DISC_DEFAULT_PORT    9002
#define DISC_MAGIC           "AMEBA_WALKIE"
#define DISC_MAGIC_LEN       12
#define DISC_INTERVAL_MS     2000
#define DISC_DEFAULT_TIMEOUT 600  /* seconds (10 minutes) */
#define DISC_EXTRA_BC        5    /* extra broadcasts after peer found (helps symmetry) */
#define DISC_EXTRA_BC_MS     400

/* Three-tier timeout policy:
 *   DISC_LLM_MAX_TIMEOUT_S  — ceiling when LLM calls the cap directly as a tool;
 *                             keeps engine_task unblocked (engine budget = 120 s).
 *   DISC_DEFAULT_TIMEOUT    — used when caller does not supply timeout_s.
 *   DISC_MAX_TIMEOUT        — hard ceiling for all callers (e.g. Lua scripts). */
#define DISC_LLM_MAX_TIMEOUT_S  60
#define DISC_MAX_TIMEOUT        3600  /* seconds (1 hour) */

static int execute_discover_peer(const char *input_json,
                                 const claw_cap_call_context_t *ctx,
                                 char **output)
{
    uint16_t disc_port = DISC_DEFAULT_PORT;
    int      timeout_s = DISC_DEFAULT_TIMEOUT;

    cJSON *j = cJSON_Parse(input_json ? input_json : "{}");
    if (j) {
        cJSON *jp = cJSON_GetObjectItem(j, "port");
        if (jp) disc_port = (uint16_t)cJSON_GetNumberValue(jp);
        cJSON *jt = cJSON_GetObjectItem(j, "timeout_s");
        if (jt) timeout_s = (int)cJSON_GetNumberValue(jt);
        cJSON_Delete(j);
    }
    if (timeout_s < 1)            timeout_s = 1;
    if (timeout_s > DISC_MAX_TIMEOUT) timeout_s = DISC_MAX_TIMEOUT;

    /* Direct LLM tool call: enforce a shorter ceiling to protect the engine. */
    if (ctx && ctx->caller == CLAW_CAP_CALLER_LLM &&
            timeout_s > DISC_LLM_MAX_TIMEOUT_S) {
        timeout_s = DISC_LLM_MAX_TIMEOUT_S;
        RTK_LOGD(TAG, "LLM caller: timeout_s capped to %d\n", DISC_LLM_MAX_TIMEOUT_S);
    }

    /* Wait for a valid STA IP (up to 30 s) */
    char my_ip[16] = "";
    for (int i = 0; i < 60 && !my_ip[0]; i++) {
        const char *ip = claw_wifi_mgr_get_sta_ip();
        if (ip && ip[0] && strcmp(ip, "0.0.0.0") != 0)
            strlcpy(my_ip, ip, sizeof(my_ip));
        else
            rtos_time_delay_ms(500);
    }
    if (!my_ip[0])
        return claw_cap_set_output(output, "{\"error\":\"no_ip\",\"msg\":\"WiFi not connected\"}");

    /* Broadcast socket */
    int bc = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (bc < 0)
        return claw_cap_set_output(output, "{\"error\":\"socket_failed\"}");
    int yes = 1;
    lwip_setsockopt(bc, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    /* Listen socket — SO_REUSEADDR lets us re-bind quickly after a previous
     * call closed the socket (avoids TIME_WAIT / "bind_failed" on retry). */
    int ls = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (ls < 0) {
        lwip_close(bc);
        return claw_cap_set_output(output, "{\"error\":\"socket_failed\"}");
    }
    int reuse = 1;
    lwip_setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in la;
    memset(&la, 0, sizeof(la));
    la.sin_family      = AF_INET;
    la.sin_port        = htons(disc_port);
    la.sin_addr.s_addr = htonl(INADDR_ANY);
    if (lwip_bind(ls, (struct sockaddr *)&la, sizeof(la)) < 0) {
        RTK_LOGE(TAG, "bind port %u failed\n", (unsigned)disc_port);
        lwip_close(bc);
        lwip_close(ls);
        return claw_cap_set_output(output, "{\"error\":\"bind_failed\",\"port\":%u}",
                                   (unsigned)disc_port);
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = 300000};  /* 300 ms recv timeout */
    lwip_setsockopt(ls, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
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
            RTK_LOGD(TAG, "Discovery: broadcast #%d\n", iter);
        }

        char             buf[32];
        struct sockaddr_in from;
        socklen_t        flen = sizeof(from);
        int n = lwip_recvfrom(ls, buf, sizeof(buf), 0,
                              (struct sockaddr *)&from, &flen);
        if (n >= DISC_MAGIC_LEN && memcmp(buf, DISC_MAGIC, DISC_MAGIC_LEN) == 0) {
            char sender[16];
            inet_ntoa_r(from.sin_addr, sender, sizeof(sender));
            if (strcmp(sender, my_ip) != 0) {
                strlcpy(peer_ip, sender, sizeof(peer_ip));
                RTK_LOGI(TAG, "Discovery: found peer %s\n", peer_ip);
            }
        }
    }

    /* A few extra broadcasts so the peer can complete its own discovery loop */
    if (peer_ip[0]) {
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

/* ---- capability registration ---------------------------------------------- */

static const claw_cap_descriptor_t s_caps[] = {
    {
        .id          = "net_discover_peer",
        .name        = "net_discover_peer",
        .family      = "network",
        .description =
            "Discover a peer Ameba board on the local WiFi via UDP broadcast. "
            "Uses the built-in AMEBA_WALKIE protocol — all Ameba boards speak it. "
            "NEVER implement peer discovery yourself in Lua (custom UDP loops are "
            "incompatible with this protocol and with other boards). "
            "Always use this cap for any peer discovery need. "
            "Returns {\"peer_ip\":\"x.x.x.x\"} on success or {\"error\":\"timeout\"} on failure. "
            "When called as a direct tool, timeout_s is capped at 60 s. "
            "From inside a lua_run_async script via cap.call(), timeout_s up to 3600 s is allowed.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"port\":{\"type\":\"integer\","
                      "\"description\":\"UDP broadcast port (default 9002)\"},"
            "\"timeout_s\":{\"type\":\"integer\","
                           "\"description\":\"Max wait time in seconds (default 600, max 3600)\"}"
            "}}",
        .execute     = execute_discover_peer,
    },
};

static const claw_cap_group_t s_group = {
    .group_id         = "net_discover",
    .plugin_name      = "cap_net_discover",
    .version          = "1",
    .descriptors      = s_caps,
    .descriptor_count = 1,
};

int cap_net_discover_init(void)
{
    return claw_cap_register_group(&s_group);
}

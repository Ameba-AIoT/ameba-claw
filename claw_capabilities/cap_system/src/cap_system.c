/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_system.h"
#include "claw_cap.h"
#include "claw_cap_registry.h"
#include <cJSON.h>
#include "platform_stdlib.h"
#include "FreeRTOS.h"
#include "task.h"
#include "os_wrapper.h"
#include "sys_api.h"
#include "wifi_api.h"
#include "wifi_api_ext.h"
#include "lwip_netconf.h"
#include <stdio.h>
#include <string.h>

#define TAG "cap_system"
#define MAX_TASKS 48

static const char *task_state_str(eTaskState s)
{
    switch (s) {
    case eRunning:   return "running";
    case eReady:     return "ready";
    case eBlocked:   return "blocked";
    case eSuspended: return "suspended";
    case eDeleted:   return "deleted";
    default:         return "unknown";
    }
}

static int execute_get_heap_info(const char *input_json,
                                 const claw_cap_call_context_t *ctx,
                                 char **output)
{
    (void)input_json;
    (void)ctx;

    uint32_t free_now = rtos_mem_get_free_heap_size();
    uint32_t free_min = rtos_mem_get_minimum_ever_free_heap_size();

    return claw_cap_set_output(output,
             "{\"free_heap_bytes\":%lu,\"min_free_ever_bytes\":%lu}",
             (unsigned long)free_now, (unsigned long)free_min);
}

static int execute_get_task_list(const char *input_json,
                                 const claw_cap_call_context_t *ctx,
                                 char **output)
{
    (void)input_json;
    (void)ctx;

    UBaseType_t count = uxTaskGetNumberOfTasks();
    if (count > MAX_TASKS) count = MAX_TASKS;

    TaskStatus_t *arr = rtos_mem_malloc(count * sizeof(TaskStatus_t));
    if (!arr) {
        claw_cap_set_output(output, "{\"error\":\"out of memory\"}");
        return RTK_ERR_NOMEM;
    }

    UBaseType_t filled = uxTaskGetSystemState(arr, count, NULL);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "total_tasks", (double)filled);
    cJSON *tasks = cJSON_CreateArray();
    cJSON_AddItemToObject(resp, "tasks", tasks);

    for (UBaseType_t i = 0; i < filled; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name",     arr[i].pcTaskName ? arr[i].pcTaskName : "?");
        cJSON_AddStringToObject(t, "state",    task_state_str(arr[i].eCurrentState));
        cJSON_AddNumberToObject(t, "priority", (double)arr[i].uxCurrentPriority);
        cJSON_AddNumberToObject(t, "stack_watermark_words", (double)arr[i].usStackHighWaterMark);
        cJSON_AddItemToArray(tasks, t);
    }

    rtos_mem_free(arr);

    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!out) {
        *output = NULL;
        return RTK_ERR_NOMEM;
    }
    *output = out;
    return RTK_SUCCESS;
}

static int execute_get_info(const char *input_json,
                            const claw_cap_call_context_t *ctx,
                            char **output)
{
    (void)input_json;
    (void)ctx;

    uint32_t uptime_ms = rtos_time_get_current_system_time_ms();
    uint32_t uptime_s  = uptime_ms / 1000;

    return claw_cap_set_output(output,
             "{\"chip\":\"RTL8721F\",\"rtos\":\"FreeRTOS v10.4.3\","
             "\"uptime_s\":%lu,\"uptime_ms\":%lu}",
             (unsigned long)uptime_s, (unsigned long)uptime_ms);
}

static int execute_get_wifi(const char *input_json,
                            const claw_cap_call_context_t *ctx,
                            char **output)
{
    (void)input_json;
    (void)ctx;

    struct rtw_wifi_setting setting = {0};
    int ret = wifi_get_setting(0, &setting);
    if (ret != 0) {
        claw_cap_set_output(output, "{\"error\":\"wifi_get_setting failed\",\"code\":%d}", ret);
        return RTK_SUCCESS;
    }

    /* Get RSSI via phy stats */
    union rtw_phy_stats phy = {0};
    int8_t rssi = -127;
    if (wifi_get_phy_stats(0, NULL, &phy) == 0) {
        rssi = phy.sta.rssi;
    }

    /* Get IP from LwIP */
    char ip_str[16] = "0.0.0.0";
    uint8_t *ip = lwip_get_ip(NETIF_WLAN_STA_INDEX);
    if (ip) {
        DiagSnPrintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "ssid", (const char *)setting.ssid);
    cJSON_AddNumberToObject(resp, "channel", setting.channel);
    cJSON_AddNumberToObject(resp, "rssi_dbm", rssi);
    cJSON_AddStringToObject(resp, "ip", ip_str);

    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!out) {
        *output = NULL;
        return RTK_ERR_NOMEM;
    }
    *output = out;
    return RTK_SUCCESS;
}

static int execute_get_ip(const char *input_json,
                          const claw_cap_call_context_t *ctx,
                          char **output)
{
    (void)input_json;
    (void)ctx;

    char ip_str[16] = "0.0.0.0";
    char gw_str[16] = "0.0.0.0";
    uint8_t *ip = lwip_get_ip(NETIF_WLAN_STA_INDEX);
    uint8_t *gw = lwip_get_gw(NETIF_WLAN_STA_INDEX);

    if (ip) DiagSnPrintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    if (gw) DiagSnPrintf(gw_str, sizeof(gw_str), "%d.%d.%d.%d", gw[0], gw[1], gw[2], gw[3]);

    return claw_cap_set_output(output,
             "{\"ip\":\"%s\",\"gateway\":\"%s\"}", ip_str, gw_str);
}

static void restart_task(void *param)
{
    (void)param;
    rtos_time_delay_ms(1000);
    sys_reset();
    rtos_task_delete(NULL);
}

static int execute_restart(const char *input_json,
                           const claw_cap_call_context_t *ctx,
                           char **output)
{
    (void)input_json;
    (void)ctx;

    int rc = claw_cap_set_output(output, "{\"status\":\"restarting\",\"delay_ms\":1000}");
    /* Spawn a detached task to reset after reply is sent */
    rtos_task_create(NULL, "sys_restart", restart_task, NULL, 512, 5);
    return rc;
}

static const claw_cap_descriptor_t s_caps[] = {
    {
        .id          = "get_heap_info",
        .name        = "get_heap_info",
        .family      = "system",
        .description = "Get current heap memory usage: free heap size and historical minimum free heap.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute     = execute_get_heap_info,
    },
    {
        .id          = "get_task_list",
        .name        = "get_task_list",
        .family      = "system",
        .description = "List all FreeRTOS tasks: name, state, priority, and stack watermark.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute     = execute_get_task_list,
    },
    {
        .id          = "get_info",
        .name        = "get_info",
        .family      = "system",
        .description = "Get basic system info: chip model, RTOS version, uptime.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute     = execute_get_info,
    },
    {
        .id          = "get_wifi",
        .name        = "get_wifi",
        .family      = "system",
        .description = "Get current WiFi connection info: SSID, channel, RSSI (dBm), IP address.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute     = execute_get_wifi,
    },
    {
        .id          = "get_ip",
        .name        = "get_ip",
        .family      = "system",
        .description = "Get current IP address and gateway.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute     = execute_get_ip,
    },
    {
        .id          = "restart",
        .name        = "restart",
        .family      = "system",
        .description = "Restart the device with a soft reset after ~1 second.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute     = execute_restart,
    },
};

static const claw_cap_group_t s_group = {
    .group_id         = "system",
    .plugin_name      = "cap_system",
    .version          = "1",
    .descriptors      = s_caps,
    .descriptor_count = 6,
};

int cap_system_init(void)
{
    claw_cap_register_group(&s_group);
    return RTK_SUCCESS;
}

/* ---- Lifecycle registration (claw_cap_registry): pure INIT phase ---- */
static void system_on_init(const claw_config_t *cfg)
{
    (void)cfg;
    cap_system_init();
}
CLAW_CAP_REGISTER(system, {
    .group   = "system",
    .order   = 55,
    .on_init = system_on_init,
});

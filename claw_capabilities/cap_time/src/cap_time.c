/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_time.h"
#include "claw_cap.h"
#include "claw_agent.h"
#include <cJSON.h>
#include "platform_stdlib.h"
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lwip/apps/sntp.h"

#define TAG "cap_time"

static int s_timezone_hrs = 8;

static int execute_get_current_time(const char *input_json,
                                    const claw_cap_call_context_t *ctx,
                                    char **output)
{
    (void)input_json;
    (void)ctx;

    time_t now = time(NULL);
    if (now <= 0) {
        *output = strdup("{\"error\":\"time not synced\",\"unix_timestamp\":0}");
        return RTK_SUCCESS;
    }

    /* Apply timezone offset */
    time_t local_time = now + (time_t)s_timezone_hrs * 3600;
    struct tm t;
    gmtime_r(&local_time, &t);

    char dt_buf[32];
    DiagSnPrintf(dt_buf, sizeof(dt_buf), "%04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);

    char tz_buf[12];
    if (s_timezone_hrs >= 0) {
        DiagSnPrintf(tz_buf, sizeof(tz_buf), "UTC+%d", s_timezone_hrs);
    } else {
        DiagSnPrintf(tz_buf, sizeof(tz_buf), "UTC%d", s_timezone_hrs);
    }

    const char *fmt = "{\"datetime\":\"%s\",\"timezone\":\"%s\",\"unix_timestamp\":%ld}";
    int n = DiagSnPrintf(NULL, 0, fmt, dt_buf, tz_buf, (long)now);
    *output = malloc((size_t)n + 1);
    if (!*output) return RTK_ERR_NOMEM;
    DiagSnPrintf(*output, (size_t)n + 1, fmt, dt_buf, tz_buf, (long)now);
    return RTK_SUCCESS;
}

static int execute_sync_time(const char *input_json,
                             const claw_cap_call_context_t *ctx,
                             char **output)
{
    (void)input_json;
    (void)ctx;

    sntp_init();
    *output = strdup("{\"status\":\"sync requested\"}");
    return *output ? RTK_SUCCESS : RTK_ERR_NOMEM;
}

static const claw_cap_descriptor_t s_caps[] = {
    {
        .id          = "get_current_time",
        .name        = "get_current_time",
        .family      = "time",
        .description = "Get current date and time (UTC+8), returns a formatted time string and Unix timestamp.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute     = execute_get_current_time,
    },
    {
        .id          = "sync_time",
        .name        = "sync_time",
        .family      = "time",
        .description = "Trigger SNTP time sync, fetches the latest time from NTP server.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute     = execute_sync_time,
    },
};

static const claw_cap_group_t s_group = {
    .group_id         = "time",
    .plugin_name      = "cap_time",
    .version          = "1",
    .descriptors      = s_caps,
    .descriptor_count = 2,
};

static int collect_time_context(const claw_agent_request_t *request,
                                claw_agent_context_t *out_context,
                                void *user_ctx)
{
    (void)request;
    (void)user_ctx;

    time_t now = time(NULL);
    if (now <= 0) return RTK_FAIL;

    time_t local_time = now + (time_t)s_timezone_hrs * 3600;
    struct tm t;
    gmtime_r(&local_time, &t);

    char buf[64];
    int n = DiagSnPrintf(buf, sizeof(buf),
                     "Current datetime: %04d-%02d-%02d %02d:%02d:%02d (UTC%+d)",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min, t.tm_sec, s_timezone_hrs);
    if (n <= 0) return RTK_FAIL;

    /* claw_agent frees out_context->content with libc free(), so use libc
     * malloc here — NOT rtos_mem_malloc, which sits on a separate heap and
     * would silently corrupt heap metadata when libc free() touched it. */
    char *content = malloc((size_t)n + 1);
    if (!content) return RTK_FAIL;
    _memcpy(content, buf, (size_t)n + 1);

    out_context->kind    = CLAW_AGENT_CONTEXT_KIND_SYSTEM_PROMPT;
    out_context->content = content;
    return RTK_SUCCESS;
}

claw_agent_context_provider_t cap_time_context_provider = {
    .name     = "time",
    .collect  = collect_time_context,
    .user_ctx = NULL,
};

int cap_time_init(const cap_time_config_t *cfg)
{
    const char *server = "pool.ntp.org";
    if (cfg && cfg->ntp_server && cfg->ntp_server[0]) {
        server = cfg->ntp_server;
    }
    if (cfg) {
        s_timezone_hrs = cfg->timezone_hrs;
    }

    sntp_setservername(0, server);
    sntp_init();

    claw_cap_register_group(&s_group);
    return RTK_SUCCESS;
}

/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_time.h"
#include "claw_cap.h"
#include "claw_cap_registry.h"
#include "claw_agent.h"
#include "claw_wifi_mgr.h"
#include <cJSON.h>
#include "platform_stdlib.h"
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lwip/apps/sntp.h"
#include "ameba_claw_defs.h"
#include "ameba_soc.h"           /* RTC_TimeTypeDef, RTC_SetTime, RTC_TimeStructInit */
#include "os_wrapper_time.h"     /* rtos_time_delay_ms */
#include "os_wrapper_timer.h"    /* rtos_timer_t, rtos_timer_create/start/stop */

#define TAG "cap_time"

static int s_timezone_hrs = 8;
static volatile uint8_t s_rtc_synced = 0;  /* written once after first valid NTP sync */
static rtos_timer_t s_rtc_poll_timer = NULL;

/* Write system clock (already NTP-synced) to RTC hardware. UTC time only —
 * callers must NOT apply timezone before calling. */
static void cap_time_sync_rtc_if_needed(void)
{
    if (s_rtc_synced) return;
    time_t now = time(NULL);
    if (now < CLAW_TIME_MIN_VALID_UNIX) return;

    struct tm t;
    gmtime_r(&now, &t);

    RTC_TimeTypeDef rtc;
    RTC_TimeStructInit(&rtc);
    rtc.RTC_Year     = (u16)(t.tm_year + 1900);
    rtc.RTC_Days     = (u16)t.tm_yday;   /* 0-based day-of-year, matches tm_yday */
    rtc.RTC_Hours    = (u8)t.tm_hour;
    rtc.RTC_Minutes  = (u8)t.tm_min;
    rtc.RTC_Seconds  = (u8)t.tm_sec;
    rtc.RTC_H12_PMAM = RTC_H12_AM;
    RTC_SetTime(RTC_Format_BIN, &rtc);
    s_rtc_synced = 1;
    RTK_LOGI(TAG, "RTC synced from NTP\n");
}

static int execute_get_local_time(const char *input_json,
                                    const claw_cap_call_context_t *ctx,
                                    char **output)
{
    (void)input_json;
    (void)ctx;

    time_t now = time(NULL);
    if (now < CLAW_TIME_MIN_VALID_UNIX) {
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

    sntp_stop();   /* safe no-op if pcb==NULL; tears down existing pcb so
                    * sntp_init() below always creates a fresh request — same
                    * stop+init pattern used in cap_time_kick_sntp(). */
    sntp_init();

    /* Poll up to 10 s for SNTP to update the system clock, then write RTC so
     * the caller doesn't need a separate rtc.set_time() call. */
    for (int i = 0; i < 10; i++) {
        rtos_time_delay_ms(1000);
        if (time(NULL) >= CLAW_TIME_MIN_VALID_UNIX) {
            cap_time_sync_rtc_if_needed();
            *output = strdup("{\"status\":\"synced\",\"rtc_updated\":true}");
            return *output ? RTK_SUCCESS : RTK_ERR_NOMEM;
        }
    }

    *output = strdup("{\"status\":\"sync requested\"}");
    return *output ? RTK_SUCCESS : RTK_ERR_NOMEM;
}

static const claw_cap_descriptor_t s_caps[] = {
    {
        .id          = "get_local_time",
        .name        = "get_local_time",
        .family      = "time",
        .description = "Get the current LOCAL date/time, formatted with the configured timezone (currently UTC+8). Use this for any time shown to a user. The unix_timestamp field it returns is raw UTC epoch; for just a UTC epoch number in Lua, sys.time() is simpler.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute     = execute_get_local_time,
    },
    {
        .id          = "sync_time",
        .name        = "sync_time",
        .family      = "time",
        .description = "Trigger SNTP time sync and wait up to 10s. On success writes both system clock and RTC hardware; returns {\"status\":\"synced\",\"rtc_updated\":true}. Falls back to {\"status\":\"sync requested\"} if NTP unreachable. Timezone note: RTC hardware stores UTC. In Lua use rtc.get_local_time(8) to get UTC+8 local time (handles day/month rollover); rtc.get_time() returns raw UTC.",
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
    if (now < CLAW_TIME_MIN_VALID_UNIX) return RTK_FAIL;

    /* Opportunistic: sync RTC the first time we see a valid system clock,
     * even if the LLM never explicitly calls sync_time. */
    cap_time_sync_rtc_if_needed();

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
    .name       = "time",
    .collect    = collect_time_context,
    .user_ctx   = NULL,
    .quiet_skip = true,  /* skips when RTC not yet synced — expected */
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

    /* NOTE: do NOT sntp_init() here.  cap_time_init() runs long before the
     * wifi_mgr task is created, so the first SNTP request would always fail
     * (and lwip would only retry an hour later).  SNTP is kicked from the
     * WiFi on-connected callback instead — see cap_time_kick_sntp(). */
    sntp_setservername(0, server);

    claw_cap_register_group(&s_group);
    return RTK_SUCCESS;
}

/* Timer callback: poll every 5 s until system clock becomes valid, then
 * write RTC and stop.  Runs in the timer-service task (small stack) so only
 * call functions safe from timer context — cap_time_sync_rtc_if_needed calls
 * time(), gmtime_r(), RTC_TimeStructInit(), RTC_SetTime(), and RTK_LOGI(),
 * all of which are safe here (RTK_LOGI wraps DiagPrintf: direct UART write,
 * no mutex, no FreeRTOS blocking).  rtos_timer_stop() is called below in
 * this callback, not inside cap_time_sync_rtc_if_needed. */
static void rtc_poll_cb(void *arg)
{
    (void)arg;
    cap_time_sync_rtc_if_needed();
    if (s_rtc_synced && s_rtc_poll_timer) {
        rtos_timer_stop(s_rtc_poll_timer, 0);
    }
}

void cap_time_kick_sntp(void)
{
    /* stop+init pair so repeated WiFi reconnects don't leak/duplicate the
     * SNTP pcb; sntp_init() alone is not safe to call twice.  The first call
     * here runs sntp_stop() before any sntp_init(): verified safe in this
     * lwIP port (lwip_v2.1.2) — sntp_stop() is fully guarded by
     * `if (sntp_pcb != NULL)` and sntp_pcb starts NULL, so it is a no-op
     * until SNTP has actually been started. */
    RTK_LOGI("cap_time", "kick SNTP (server=%s)\n", sntp_getservername(0));
    sntp_stop();
    sntp_init();

    /* Start a 5-second repeating timer to write RTC once SNTP completes.
     * The timer stops itself after the first successful write. */
    s_rtc_synced = 0;
    if (!s_rtc_poll_timer) {
        rtos_timer_create(&s_rtc_poll_timer, "rtc_sync", 0, 5000, 1, rtc_poll_cb);
    }
    if (s_rtc_poll_timer) {
        rtos_timer_start(s_rtc_poll_timer, 0);
    }
}

/* ---- Lifecycle registration (claw_cap_registry) ----
 * cap_time spans all three phases. Each hook mirrors the wiring formerly in
 * ameba_claw_main.c:
 *   INIT  — cap_time_init (register group, set NTP server).
 *   AGENT — add the time context provider.
 *   IO    — register the SNTP kick on the wifi on-connected callback (the
 *           network is down at init time, so SNTP must start once wifi is up).
 */
static void time_on_init(const claw_config_t *cfg)
{
    (void)cfg;
    const cap_time_config_t c = { .ntp_server = "pool.ntp.org", .timezone_hrs = 8 };
    cap_time_init(&c);
}

static void time_on_agent(const claw_config_t *cfg)
{
    (void)cfg;
    claw_agent_add_context_provider(&cap_time_context_provider);
}

static void time_on_io(const claw_config_t *cfg)
{
    (void)cfg;
    /* cap_time_kick_sntp matches the void(void) on-connected signature. */
    claw_wifi_mgr_register_on_connected(cap_time_kick_sntp);
}

CLAW_CAP_REGISTER(time, {
    .group    = "time",
    .order    = 10,
    .on_init  = time_on_init,
    .on_agent = time_on_agent,
    .on_io    = time_on_io,
});

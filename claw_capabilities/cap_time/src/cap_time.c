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
#include "claw_config.h"
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

/* Timezone: offset in minutes east of UTC, mirrored from claw_config. s_tz_set
 * is false until the user configures a timezone — local-time features refuse to
 * guess until then (see cap_time_local_now / execute_get_local_time). */
static int  s_tz_offset_min = CLAW_TIME_DEFAULT_TZ_OFFSET_MIN;
static bool s_tz_set        = false;
static volatile uint8_t s_rtc_synced = 0;  /* written once after first valid NTP sync */
static rtos_timer_t s_rtc_poll_timer = NULL;

/* Pull the timezone from the live config into our cached mirror. Called at init
 * and after every config save (registered as an on-save callback), so a runtime
 * set_timezone / WebUI edit takes effect without a reboot. */
static void tz_refresh_from_config(void)
{
    claw_config_t *cfg = claw_config_get();
    if (cfg) {
        s_tz_offset_min = cfg->time.offset_min;
        s_tz_set        = cfg->time.set;
    }
}

/* ---- Unified time API (see cap_time.h) ---- */

bool cap_time_is_synced(void)
{
    return time(NULL) >= CLAW_TIME_MIN_VALID_UNIX;
}

bool cap_time_get_tz_offset_sec(long *out_offset_sec)
{
    if (out_offset_sec) *out_offset_sec = (long)s_tz_offset_min * 60;
    return s_tz_set;
}

bool cap_time_local_now(struct tm *out)
{
    if (!out) return false;
    time_t now = time(NULL);
    if (now < CLAW_TIME_MIN_VALID_UNIX) return false;   /* clock not synced */
    if (!s_tz_set) return false;                        /* timezone not configured */
    time_t local = now + (time_t)s_tz_offset_min * 60;
    gmtime_r(&local, out);
    return true;
}

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

/* Format the configured offset as "UTC+8" / "UTC+5:30" / "UTC-5". */
static void tz_label(char *buf, size_t sz)
{
    int off = s_tz_offset_min;
    char sign = (off < 0) ? '-' : '+';
    int a = off < 0 ? -off : off;
    int h = a / 60, m = a % 60;
    if (m)
        DiagSnPrintf(buf, sz, "UTC%c%d:%02d", sign, h, m);
    else
        DiagSnPrintf(buf, sz, "UTC%c%d", sign, h);
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
    if (!s_tz_set) {
        /* Do NOT guess a timezone — ask the user, then persist via set_timezone. */
        *output = strdup(
            "{\"error\":\"timezone not configured\",\"unix_timestamp\":0,"
            "\"hint\":\"Ask the user which timezone (UTC offset) they are in, "
            "then call set_timezone. Do not guess.\"}");
        return RTK_SUCCESS;
    }

    struct tm t;
    (void)cap_time_local_now(&t);   /* true here: synced + tz set */

    char dt_buf[32];
    DiagSnPrintf(dt_buf, sizeof(dt_buf), "%04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);

    char tz_buf[16];
    tz_label(tz_buf, sizeof(tz_buf));

    const char *fmt = "{\"datetime\":\"%s\",\"timezone\":\"%s\",\"unix_timestamp\":%ld}";
    int n = DiagSnPrintf(NULL, 0, fmt, dt_buf, tz_buf, (long)now);
    *output = malloc((size_t)n + 1);
    if (!*output) return RTK_ERR_NOMEM;
    DiagSnPrintf(*output, (size_t)n + 1, fmt, dt_buf, tz_buf, (long)now);
    return RTK_SUCCESS;
}

static int execute_set_timezone(const char *input_json,
                                const claw_cap_call_context_t *ctx,
                                char **output)
{
    (void)ctx;

    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        *output = strdup("{\"error\":\"invalid json\"}");
        return RTK_FAIL;
    }

    /* Accept offset_min (preferred, supports :30/:45 zones) or offset_hours. */
    cJSON *jmin = cJSON_GetObjectItem(root, "offset_min");
    cJSON *jhrs = cJSON_GetObjectItem(root, "offset_hours");
    int offset_min;
    if (jmin && cJSON_IsNumber(jmin)) {
        offset_min = (int)jmin->valuedouble;
    } else if (jhrs && cJSON_IsNumber(jhrs)) {
        offset_min = (int)(jhrs->valuedouble * 60.0);
    } else {
        cJSON_Delete(root);
        *output = strdup("{\"error\":\"provide offset_min or offset_hours\"}");
        return RTK_FAIL;
    }
    cJSON_Delete(root);

    if (offset_min < -720 || offset_min > 840) {   /* UTC-12 .. UTC+14 */
        *output = strdup("{\"error\":\"offset out of range (UTC-12..UTC+14)\"}");
        return RTK_FAIL;
    }

    /* Persist; claw_config_save() fires on-save → tz_refresh_from_config(). */
    int rc = claw_config_set_timezone((int32_t)offset_min);
    if (rc != RTK_SUCCESS) {
        *output = strdup("{\"error\":\"failed to persist timezone\"}");
        return RTK_FAIL;
    }

    char tz_buf[16];
    tz_label(tz_buf, sizeof(tz_buf));
    const char *fmt = "{\"ok\":true,\"timezone\":\"%s\"}";
    int n = DiagSnPrintf(NULL, 0, fmt, tz_buf);
    *output = malloc((size_t)n + 1);
    if (!*output) return RTK_ERR_NOMEM;
    DiagSnPrintf(*output, (size_t)n + 1, fmt, tz_buf);
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
        .description = "Get the current LOCAL date/time, formatted with the user's configured timezone. Use this for any time shown to a user. If it returns error \"timezone not configured\", ask the user their timezone and call set_timezone first. The unix_timestamp field is raw UTC epoch; for just a UTC epoch number in Lua, sys.time() is simpler.",
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
    {
        .id          = "set_timezone",
        .name        = "set_timezone",
        .family      = "time",
        .description = "Set the device's LOCAL timezone (persisted). ONLY call this when the USER explicitly asks to set or correct their timezone — never guess. If get_local_time reports \"timezone not configured\", first ask the user which timezone / UTC offset they are in, then call this. Pass offset_hours (e.g. 8 for UTC+8, -5 for UTC-5) or offset_min for half-hour zones (e.g. 330 for UTC+5:30). Scheduled cron/alarm tasks stay suspended until a timezone is set.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"offset_hours\":{\"type\":\"number\",\"description\":\"UTC offset in hours, e.g. 8 = UTC+8, -5 = UTC-5\"},"
            "\"offset_min\":{\"type\":\"number\",\"description\":\"UTC offset in minutes (use for :30/:45 zones, e.g. 330 = UTC+5:30). Takes precedence over offset_hours.\"}"
            "}}",
        .execute     = execute_set_timezone,
    },
};

static const claw_cap_group_t s_group = {
    .group_id         = "time",
    .plugin_name      = "cap_time",
    .version          = "1",
    .descriptors      = s_caps,
    .descriptor_count = 3,
};

static int collect_time_context(const claw_agent_request_t *request,
                                claw_agent_context_t *out_context,
                                void *user_ctx)
{
    (void)request;
    (void)user_ctx;

    if (time(NULL) < CLAW_TIME_MIN_VALID_UNIX) return RTK_FAIL;

    /* Opportunistic: sync RTC the first time we see a valid system clock,
     * even if the LLM never explicitly calls sync_time. */
    cap_time_sync_rtc_if_needed();

    /* Unified local time; skips (quiet) when timezone is not yet configured. */
    struct tm t;
    if (!cap_time_local_now(&t)) return RTK_FAIL;

    char tz_buf[16];
    tz_label(tz_buf, sizeof(tz_buf));

    char buf[72];
    int n = DiagSnPrintf(buf, sizeof(buf),
                     "Current datetime: %04d-%02d-%02d %02d:%02d:%02d (%s)",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min, t.tm_sec, tz_buf);
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

    /* Timezone comes from claw_config now (not the passed struct). Seed the
     * cached mirror and refresh it after every config save. */
    tz_refresh_from_config();
    claw_config_register_on_save(tz_refresh_from_config);

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

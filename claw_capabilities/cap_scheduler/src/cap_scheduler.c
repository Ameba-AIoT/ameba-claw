/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * cap_scheduler — wall-clock scheduler.
 *
 * Two orthogonal axes per entry:
 *   trigger (WHEN):  once | interval | cron | on_event
 *   action  (WHAT):  agent | cap | emit
 * The action is self-contained in the entry (no separate router rule needed).
 *
 * once/interval/cron use real epoch time and are gated on a synced clock AND a
 * configured timezone (cap_time). on_event fires on a system event
 * (e.g. wifi_connected) and does NOT depend on the wall clock, so the classic
 * "run a Lua script on boot" use keeps working before SNTP.
 *
 * Definitions and runtime state persist to two files (definitions survive a
 * reload without losing run counts): schedules.json + state.json.
 */
#include "cap_scheduler.h"
#include "ameba_claw_defs.h"
#include "cron_expr.h"
#include "claw_cap.h"
#include "claw_cap_registry.h"
#include "claw_wifi_mgr.h"
#include "claw_event_publisher.h"
#include "claw_event.h"
#include "cap_time.h"
#include <cJSON.h>
#include "platform_stdlib.h"
#include "os_wrapper.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define TAG "cap_scheduler"
#define MAX_JOBS CLAW_SCHEDULER_MAX_JOBS

/* next_fire sentinels */
#define NF_ARM      ((time_t)0)    /* needs (re)computation */
#define NF_DONE     ((time_t)-1)   /* no future fire (completed / impossible) */
#define NF_PENDING  ((time_t)-2)   /* snapshotted, awaiting off-lock write-back */

/* ---- Trigger / action kinds ---- */

typedef enum { KIND_ONCE = 0, KIND_INTERVAL, KIND_CRON, KIND_ON_EVENT } sched_kind_t;
typedef enum { ACT_AGENT = 0, ACT_CAP, ACT_EMIT } sched_action_t;

static const char *kind_str(sched_kind_t k)
{
    switch (k) {
    case KIND_ONCE:     return "once";
    case KIND_INTERVAL: return "interval";
    case KIND_CRON:     return "cron";
    case KIND_ON_EVENT: return "on_event";
    default:            return "once";
    }
}
static int kind_from_str(const char *s, sched_kind_t *out)
{
    if (!s) return 0;
    if      (!strcmp(s, "once"))     *out = KIND_ONCE;
    else if (!strcmp(s, "interval")) *out = KIND_INTERVAL;
    else if (!strcmp(s, "cron"))     *out = KIND_CRON;
    else if (!strcmp(s, "on_event")) *out = KIND_ON_EVENT;
    else return 0;
    return 1;
}
static const char *action_str(sched_action_t a)
{
    switch (a) {
    case ACT_AGENT: return "agent";
    case ACT_CAP:   return "cap";
    case ACT_EMIT:  return "emit";
    default:        return "agent";
    }
}
static int action_from_str(const char *s, sched_action_t *out)
{
    if (!s) return 0;
    if      (!strcmp(s, "agent")) *out = ACT_AGENT;
    else if (!strcmp(s, "cap"))   *out = ACT_CAP;
    else if (!strcmp(s, "emit"))  *out = ACT_EMIT;
    else return 0;
    return 1;
}

/* ---- Known system events (for on_event triggers) ----
 * An on_event task rendezvous with its producer purely by matching this string;
 * a trigger_event that no component ever fires would sit forever as a silent
 * dead task. This table is the single source of truth for the events an
 * on_event task may subscribe to. When you wire a new cap_scheduler_fire_event()
 * producer, add a row here AND mirror the name in the scheduler_add_job
 * description so the LLM knows it exists. */
static const struct {
    const char *name;
    const char *desc;
} k_known_events[] = {
    { "wifi_connected",
      "device came online (fires once per boot right after Wi-Fi connects, "
      "before clock sync) — the trigger for run-on-boot tasks" },
};
#define KNOWN_EVENT_COUNT (sizeof(k_known_events) / sizeof(k_known_events[0]))

static bool event_is_known(const char *name)
{
    for (size_t i = 0; i < KNOWN_EVENT_COUNT; i++)
        if (strcmp(k_known_events[i].name, name) == 0) return true;
    return false;
}

/* Build a JSON error that names the offending event and lists the valid set,
 * so an on_event task pointed at a non-existent event is rejected loudly
 * instead of created as a task that can never fire. */
static void known_events_error(char *buf, size_t sz, const char *bad)
{
    int n = DiagSnPrintf(buf, sz,
        "{\"error\":\"unknown trigger_event '%s' — no component fires it. Supported: ",
        bad);
    for (size_t i = 0; i < KNOWN_EVENT_COUNT && n > 0 && n < (int)sz; i++)
        n += DiagSnPrintf(buf + n, sz - n, "%s'%s'", i ? ", " : "",
                          k_known_events[i].name);
    if (n > 0 && n < (int)sz) DiagSnPrintf(buf + n, sz - n, "\"}");
}

/* ---- Entry ---- */

typedef struct {
    char id[32];
    int  enabled;
    int  paused;
    sched_kind_t   kind;
    sched_action_t action;

    /* trigger */
    time_t   start_at;         /* once: absolute epoch; interval: optional anchor */
    uint32_t interval_sec;     /* interval */
    char     cron_expr[48];    /* cron */
    char     trigger_event[32];/* on_event */
    uint32_t max_runs;         /* 0 = unlimited */
    time_t   end_at;           /* 0 = none */

    /* action */
    char prompt[192];          /* agent */
    char cap_id[48];           /* cap  */
    char cap_args[192];        /* cap  */
    char event_type[32];       /* emit */
    char payload_json[160];    /* emit */

    /* target session */
    char channel[32];
    char chat_id[64];
    int  session_policy;       /* claw_event_session_policy_t */

    /* runtime state */
    time_t   next_fire;        /* epoch; NF_ARM/NF_DONE/NF_PENDING sentinels */
    time_t   last_fire;
    uint32_t run_count;
    uint32_t missed_count;
    int      late;             /* last fire was within-grace late */
} sched_entry_t;

/* ---- State ---- */

static struct {
    sched_entry_t entries[MAX_JOBS];
    int           count;
    rtos_mutex_t  mutex;
    rtos_task_t   task;
    int           running;
    int           max_jobs;
    int           clock_was_valid;
    uint32_t      last_warn_ms;
    char          def_file[128];
    char          state_file[128];
} s;

/* ---- Small time helpers ---- */

/* Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm). */
static long days_from_civil(int y, int m, int d)
{
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + (long)doe - 719468L;
}
/* Interpret a broken-down time as UTC → epoch seconds. */
static time_t tm_to_epoch_utc(int year, int mon, int day, int hh, int mm, int ss)
{
    long days = days_from_civil(year, mon, day);
    return (time_t)(days * 86400L + hh * 3600L + mm * 60L + ss);
}

/* Format a UTC epoch as a local "YYYY-MM-DD HH:MM:SS" string using cap_time's
 * offset. Writes "n/a" if the epoch is a sentinel or timezone is unset. */
static void epoch_to_local_str(time_t epoch, char *buf, size_t sz)
{
    long off = 0;
    if (epoch <= 0 || !cap_time_get_tz_offset_sec(&off)) {
        strlcpy(buf, "n/a", sz);
        return;
    }
    time_t local = epoch + off;
    struct tm t;
    gmtime_r(&local, &t);
    DiagSnPrintf(buf, sz, "%04d-%02d-%02d %02d:%02d:%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
}

/* ---- Forward decls ---- */
static void save_defs(void);
static void save_state(void);

/* Returns index of entry with id, or -1. Caller holds mutex. */
static int find_idx(const char *id)
{
    for (int i = 0; i < s.count; i++)
        if (strcmp(s.entries[i].id, id) == 0) return i;
    return -1;
}

/* Compute the armed next_fire (UTC epoch) for a timer entry, or NF_DONE.
 * Pure w.r.t. the entry — safe to call OFF-LOCK (cron scan is heavy). */
static time_t compute_arm(sched_kind_t kind, time_t now, long off_sec,
                          uint32_t interval, const char *cron, time_t start_at,
                          const char *id_for_log)
{
    switch (kind) {
    case KIND_ONCE:
        return start_at > 0 ? start_at : NF_DONE;
    case KIND_INTERVAL:
        if (start_at > now) return start_at;
        return now + (time_t)interval;
    case KIND_CRON: {
        cron_expr_t c;
        if (!cron_parse(cron, &c)) {
            RTK_LOGW(TAG, "cron parse failed id=%s expr='%s'\n",
                     id_for_log ? id_for_log : "?", cron ? cron : "");
            return NF_DONE;
        }
        time_t nl = cron_next_after_local(&c, now + (time_t)off_sec);
        if (nl == (time_t)-1) {
            RTK_LOGW(TAG, "cron no match within scan window id=%s expr='%s'\n",
                     id_for_log ? id_for_log : "?", cron ? cron : "");
            return NF_DONE;
        }
        return nl - (time_t)off_sec;
    }
    case KIND_ON_EVENT:
    default:
        return NF_DONE;
    }
}

/* ---- Action execution (agent | cap | emit) ---- */

static void execute_action(const sched_entry_t *e)
{
    switch (e->action) {
    case ACT_AGENT: {
        /* Wake the agent in the target session; dispatcher's built-in
         * sched_agent_wake route handles it without a router rule. F3: frame the
         * prompt so the woken agent delivers it once and does NOT re-schedule
         * (an open prompt like "remind me to drink water" otherwise tempts the
         * agent to create a brand-new recurring task). */
        char wake_text[352];
        DiagSnPrintf(wake_text, sizeof(wake_text),
            "[Scheduled task fired] Carry out the following once for the user now, then stop. "
            "Do NOT create, update, or reschedule any scheduled task. Task: %s",
            e->prompt);
        claw_event_t evt;
        _memset(&evt, 0, sizeof(evt));
        strlcpy(evt.source_cap,     "cap_scheduler",             sizeof(evt.source_cap));
        strlcpy(evt.event_type,     CLAW_SCHED_AGENT_WAKE_EVENT, sizeof(evt.event_type));
        strlcpy(evt.source_channel, e->channel,                  sizeof(evt.source_channel));
        strlcpy(evt.chat_id,        e->chat_id,                  sizeof(evt.chat_id));
        strlcpy(evt.correlation_id, e->id,                       sizeof(evt.correlation_id));
        evt.session_policy = (claw_event_session_policy_t)e->session_policy;
        evt.text = wake_text;   /* publish_event strdups it */
        /* Fires used to leave NO serial trace: cap_lua's own START log is
         * filtered out and a scheduled script's print() is captured then
         * discarded, so "ran fine", "ran and failed", and "never fired" all
         * looked identical. Log each fire here (cap_scheduler's INFO logs DO
         * print) with id + what it drives, so a task is observable end-to-end. */
        RTK_LOGI(TAG, "fire id=%s action=agent -> wake session (ch='%s')\n",
                 e->id, e->channel);
        if (claw_event_dispatcher_publish_event(&evt) != RTK_SUCCESS)
            RTK_LOGE(TAG, "agent-wake publish failed id=%s\n", e->id);
        break;
    }
    case ACT_CAP: {
        char *out = NULL;
        claw_cap_call_context_t ctx = {0};
        if (e->channel[0]) ctx.channel = e->channel;
        if (e->chat_id[0]) ctx.chat_id = e->chat_id;
        ctx.source_cap = "cap_scheduler";
        const char *args = e->cap_args[0] ? e->cap_args : "{}";
        RTK_LOGI(TAG, "fire id=%s action=cap -> %s args=%s\n", e->id, e->cap_id, args);
        int rc = claw_cap_call(e->cap_id, args, &ctx, &out);
        RTK_LOGI(TAG, "fire id=%s cap %s rc=%d\n", e->id, e->cap_id, rc);
        free(out);
        break;
    }
    case ACT_EMIT:
        RTK_LOGI(TAG, "fire id=%s action=emit -> event '%s'\n", e->id, e->event_type);
        claw_event_dispatcher_publish_trigger("cap_scheduler", e->event_type,
                                              e->id, e->payload_json);
        break;
    }
}

/* ---- Persistence: definitions ---- */

static void entry_to_json(cJSON *o, const sched_entry_t *e)
{
    cJSON_AddStringToObject(o, "id",     e->id);
    cJSON_AddNumberToObject(o, "enabled", e->enabled);
    cJSON_AddStringToObject(o, "kind",   kind_str(e->kind));
    cJSON_AddStringToObject(o, "action", action_str(e->action));
    if (e->start_at)          cJSON_AddNumberToObject(o, "start_at",     (double)e->start_at);
    if (e->interval_sec)      cJSON_AddNumberToObject(o, "interval_sec", (double)e->interval_sec);
    if (e->cron_expr[0])      cJSON_AddStringToObject(o, "cron_expr",    e->cron_expr);
    if (e->trigger_event[0])  cJSON_AddStringToObject(o, "trigger_event",e->trigger_event);
    if (e->max_runs)          cJSON_AddNumberToObject(o, "max_runs",     (double)e->max_runs);
    if (e->end_at)            cJSON_AddNumberToObject(o, "end_at",       (double)e->end_at);
    if (e->prompt[0])         cJSON_AddStringToObject(o, "prompt",       e->prompt);
    if (e->cap_id[0])         cJSON_AddStringToObject(o, "cap_id",       e->cap_id);
    if (e->cap_args[0])       cJSON_AddStringToObject(o, "cap_args",     e->cap_args);
    if (e->event_type[0])     cJSON_AddStringToObject(o, "event_type",   e->event_type);
    if (e->payload_json[0])   cJSON_AddStringToObject(o, "payload_json", e->payload_json);
    if (e->channel[0])        cJSON_AddStringToObject(o, "channel",      e->channel);
    if (e->chat_id[0])        cJSON_AddStringToObject(o, "chat_id",      e->chat_id);
    if (e->session_policy)    cJSON_AddNumberToObject(o, "session_policy",(double)e->session_policy);
}

static void write_json_file(const char *path, cJSON *root)
{
    char *str = cJSON_PrintUnformatted(root);
    if (!str) { RTK_LOGE(TAG, "json print failed for %s\n", path); return; }
    FILE *f = fopen(path, "w");
    if (f) {
        size_t len = strlen(str);
        size_t w = fwrite(str, 1, len, f);
        fclose(f);
        if (w != len) RTK_LOGE(TAG, "write incomplete %s (%u/%u)\n", path, (unsigned)w, (unsigned)len);
    } else {
        RTK_LOGE(TAG, "open %s for write failed\n", path);
    }
    free(str);
}

static void save_defs(void)
{
    cJSON *root = cJSON_CreateArray();
    if (!root) return;
    for (int i = 0; i < s.count; i++) {
        cJSON *o = cJSON_CreateObject();
        if (!o) break;
        entry_to_json(o, &s.entries[i]);
        cJSON_AddItemToArray(root, o);
    }
    write_json_file(s.def_file, root);
    cJSON_Delete(root);
}

static void save_state(void)
{
    cJSON *root = cJSON_CreateArray();
    if (!root) return;
    for (int i = 0; i < s.count; i++) {
        sched_entry_t *e = &s.entries[i];
        cJSON *o = cJSON_CreateObject();
        if (!o) break;
        cJSON_AddStringToObject(o, "id", e->id);
        cJSON_AddNumberToObject(o, "run_count",    (double)e->run_count);
        cJSON_AddNumberToObject(o, "missed_count", (double)e->missed_count);
        cJSON_AddNumberToObject(o, "last_fire",    (double)e->last_fire);
        cJSON_AddItemToArray(root, o);
    }
    write_json_file(s.state_file, root);
    cJSON_Delete(root);
}

/* Read whole file into a heap buffer (<= 16 KB). Caller frees. NULL on miss. */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 16384) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static void js(cJSON *o, const char *k, char *dst, size_t sz)
{
    cJSON *it = cJSON_GetObjectItem(o, k);
    if (it && cJSON_IsString(it)) strlcpy(dst, it->valuestring, sz);
}
static double jn(cJSON *o, const char *k, double dflt)
{
    cJSON *it = cJSON_GetObjectItem(o, k);
    return (it && cJSON_IsNumber(it)) ? it->valuedouble : dflt;
}

/* Migrate one legacy job object (no "kind" field) into a new entry. */
static void migrate_legacy(cJSON *o, sched_entry_t *e, time_t now)
{
    char evt[48] = {0};
    js(o, "event_type", evt, sizeof(evt));
    js(o, "cap_id",   e->cap_id,   sizeof(e->cap_id));
    js(o, "cap_args", e->cap_args, sizeof(e->cap_args));
    js(o, "channel",  e->channel,  sizeof(e->channel));
    js(o, "chat_id",  e->chat_id,  sizeof(e->chat_id));
    uint32_t interval = (uint32_t)jn(o, "interval_sec", 0);
    uint32_t delay    = (uint32_t)jn(o, "delay_sec", 0);

    if (evt[0]) {
        /* legacy event job → on_event */
        e->kind = KIND_ON_EVENT;
        strlcpy(e->trigger_event, evt, sizeof(e->trigger_event));
    } else if (interval > 0) {
        e->kind = KIND_INTERVAL;
        e->interval_sec = interval;
    } else {
        e->kind = KIND_ONCE;
        e->start_at = now + (time_t)delay;
    }

    if (e->cap_id[0]) {
        e->action = ACT_CAP;
    } else {
        e->action = ACT_EMIT;
        strlcpy(e->event_type, evt, sizeof(e->event_type));
        js(o, "payload_json", e->payload_json, sizeof(e->payload_json));
    }
    RTK_LOGI(TAG, "migrated legacy job id=%s → kind=%s action=%s\n",
             e->id, kind_str(e->kind), action_str(e->action));
}

static void load_one(cJSON *o, sched_entry_t *e, time_t now)
{
    _memset(e, 0, sizeof(*e));
    js(o, "id", e->id, sizeof(e->id));
    e->enabled = (int)jn(o, "enabled", 1);

    cJSON *jkind = cJSON_GetObjectItem(o, "kind");
    if (!jkind || !cJSON_IsString(jkind)) {
        migrate_legacy(o, e, now);
    } else {
        kind_from_str(jkind->valuestring, &e->kind);
        char act[16] = {0};
        js(o, "action", act, sizeof(act));
        action_from_str(act, &e->action);
        e->start_at      = (time_t)jn(o, "start_at", 0);
        e->interval_sec  = (uint32_t)jn(o, "interval_sec", 0);
        js(o, "cron_expr",     e->cron_expr,     sizeof(e->cron_expr));
        js(o, "trigger_event", e->trigger_event, sizeof(e->trigger_event));
        e->max_runs      = (uint32_t)jn(o, "max_runs", 0);
        e->end_at        = (time_t)jn(o, "end_at", 0);
        js(o, "prompt",       e->prompt,       sizeof(e->prompt));
        js(o, "cap_id",       e->cap_id,       sizeof(e->cap_id));
        js(o, "cap_args",     e->cap_args,     sizeof(e->cap_args));
        js(o, "event_type",   e->event_type,   sizeof(e->event_type));
        js(o, "payload_json", e->payload_json, sizeof(e->payload_json));
        js(o, "channel",      e->channel,      sizeof(e->channel));
        js(o, "chat_id",      e->chat_id,      sizeof(e->chat_id));
        e->session_policy = (int)jn(o, "session_policy", 0);
    }

    /* Timer entries start un-armed (recomputed on first valid poll). */
    e->next_fire = (e->kind == KIND_ON_EVENT) ? NF_DONE : NF_ARM;
}

static void load_all(void)
{
    time_t now = time(NULL);
    char *defs = read_file(s.def_file);
    if (!defs) return;

    cJSON *root = cJSON_Parse(defs);
    free(defs);
    if (!root) { RTK_LOGW(TAG, "defs parse failed\n"); return; }

    /* Accept bare array or {"jobs":[...]} */
    cJSON *arr = cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "jobs");
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(root); return; }

    int max = s.max_jobs < MAX_JOBS ? s.max_jobs : MAX_JOBS;
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (s.count >= max) break;
        sched_entry_t *e = &s.entries[s.count];
        load_one(it, e, now);
        if (e->id[0]) s.count++;
    }
    cJSON_Delete(root);

    /* Merge runtime state (run_count/missed/last_fire) by id. */
    char *st = read_file(s.state_file);
    if (st) {
        cJSON *sroot = cJSON_Parse(st);
        free(st);
        if (sroot && cJSON_IsArray(sroot)) {
            cJSON *so;
            cJSON_ArrayForEach(so, sroot) {
                char id[32] = {0};
                js(so, "id", id, sizeof(id));
                int idx = find_idx(id);
                if (idx >= 0) {
                    s.entries[idx].run_count    = (uint32_t)jn(so, "run_count", 0);
                    s.entries[idx].missed_count = (uint32_t)jn(so, "missed_count", 0);
                    s.entries[idx].last_fire    = (time_t)jn(so, "last_fire", 0);
                    /* A completed once (already ran) must not re-arm. */
                    if (s.entries[idx].kind == KIND_ONCE && s.entries[idx].run_count > 0)
                        s.entries[idx].next_fire = NF_DONE;
                }
            }
        }
        if (sroot) cJSON_Delete(sroot);
    }

    /* If we migrated legacy defs, rewrite in the new format. */
    save_defs();
    RTK_LOGI(TAG, "loaded %d schedule entries\n", s.count);
}

/* ---- on_event firing (wall-clock independent) ---- */

void cap_scheduler_fire_event(const char *event_type)
{
    if (!s.running || !event_type || !event_type[0]) return;

    sched_entry_t *snap = malloc(MAX_JOBS * sizeof(sched_entry_t));
    if (!snap) { RTK_LOGE(TAG, "fire_event OOM\n"); return; }
    int n = 0;

    rtos_mutex_take(s.mutex, 0xFFFFFFFFUL);
    for (int i = 0; i < s.count && n < MAX_JOBS; i++) {
        sched_entry_t *e = &s.entries[i];
        if (!e->enabled || e->paused) continue;
        if (e->kind != KIND_ON_EVENT) continue;
        if (strcmp(e->trigger_event, event_type) != 0) continue;
        snap[n] = *e;
        e->run_count++;
        e->last_fire = time(NULL);
        n++;
    }
    if (n) save_state();
    rtos_mutex_give(s.mutex);

    for (int i = 0; i < n; i++) execute_action(&snap[i]);
    free(snap);
}

/* ---- Poll task ---- */

typedef enum { INTENT_RECOMPUTE = 0, INTENT_DONE = 1 } intent_t;
typedef struct { sched_entry_t e; int do_fire; intent_t intent; } work_t;

static int count_timer_entries_locked(void)
{
    int n = 0;
    for (int i = 0; i < s.count; i++)
        if (s.entries[i].enabled && !s.entries[i].paused &&
            s.entries[i].kind != KIND_ON_EVENT) n++;
    return n;
}

static void scheduler_task(void *arg)
{
    (void)arg;
    work_t *work = malloc(MAX_JOBS * sizeof(work_t));
    if (!work) { RTK_LOGE(TAG, "task OOM\n"); s.running = 0; rtos_task_delete(NULL); return; }

    while (s.running) {
        rtos_time_delay_ms(CLAW_SCHEDULER_POLL_MS);
        if (!s.running) break;

        time_t now = time(NULL);
        long   off = 0;
        bool   synced  = now >= CLAW_TIME_MIN_VALID_UNIX;
        bool   tz_set  = cap_time_get_tz_offset_sec(&off);
        bool   valid   = synced && tz_set;

        if (!valid) {
            /* Throttled warning if there are timer entries waiting. */
            rtos_mutex_take(s.mutex, 0xFFFFFFFFUL);
            int pending = count_timer_entries_locked();
            rtos_mutex_give(s.mutex);
            uint32_t ms = (uint32_t)rtos_time_get_current_system_time_ms();
            if (pending > 0 &&
                (s.last_warn_ms == 0 ||
                 (ms - s.last_warn_ms) >= CLAW_SCHEDULER_WARN_THROTTLE_SEC * 1000u)) {
                RTK_LOGW(TAG, "%d scheduled task(s) suspended: %s\n", pending,
                         !synced ? "clock not synced (needs network/SNTP)"
                                 : "timezone not set (ask user, call set_timezone)");
                s.last_warn_ms = ms;
            }
            s.clock_was_valid = 0;
            continue;
        }
        if (!s.clock_was_valid) {
            RTK_LOGI(TAG, "clock valid — scheduling active\n");
            s.clock_was_valid = 1;
        }

        /* Phase 1 (locked): decide fire / miss / arm; snapshot work items. */
        int k = 0;
        rtos_mutex_take(s.mutex, 0xFFFFFFFFUL);
        for (int i = 0; i < s.count && k < MAX_JOBS; i++) {
            sched_entry_t *e = &s.entries[i];
            if (!e->enabled || e->paused) continue;
            if (e->kind == KIND_ON_EVENT) continue;
            if (e->next_fire == NF_DONE || e->next_fire == NF_PENDING) continue;

            int      do_fire = 0;
            intent_t intent  = INTENT_RECOMPUTE;
            int      work_it = 0;

            if (e->next_fire == NF_ARM) {
                work_it = 1;                      /* first arm */
            } else if (now >= e->next_fire) {
                work_it = 1;
                if (now > e->next_fire + CLAW_SCHEDULER_MISS_GRACE_SEC) {
                    e->missed_count++;
                    /* A miss used to be silent; log it so an offline gap that
                     * skips a reminder leaves a trace (id, how late, fate). */
                    RTK_LOGW(TAG, "missed id=%s (%ld s late > grace %d s) — %s\n",
                             e->id, (long)(now - e->next_fire),
                             (int)CLAW_SCHEDULER_MISS_GRACE_SEC,
                             e->kind == KIND_ONCE ? "dropped" : "advancing to next");
                    intent = (e->kind == KIND_ONCE) ? INTENT_DONE : INTENT_RECOMPUTE;
                } else {
                    do_fire = 1;
                    e->run_count++;
                    e->last_fire = now;
                    e->late = (now > e->next_fire + 2) ? 1 : 0;
                    if (e->kind == KIND_ONCE)
                        intent = INTENT_DONE;
                    else if (e->max_runs > 0 && e->run_count >= e->max_runs)
                        intent = INTENT_DONE;
                    else
                        intent = INTENT_RECOMPUTE;
                }
            }

            if (work_it) {
                work[k].e       = *e;
                work[k].do_fire = do_fire;
                work[k].intent  = intent;
                k++;
                e->next_fire = NF_PENDING;   /* block re-fire until write-back */
            }
        }
        rtos_mutex_give(s.mutex);

        /* Phase 2 (lock-free): execute actions + compute next_fire (cron scan). */
        int changed_state = 0, changed_defs = 0;
        for (int j = 0; j < k; j++) {
            work_t *w = &work[j];
            if (w->do_fire) execute_action(&w->e);

            time_t nf;
            if (w->intent == INTENT_DONE) {
                nf = NF_DONE;
            } else {
                nf = compute_arm(w->e.kind, now, off, w->e.interval_sec,
                                 w->e.cron_expr, w->e.start_at, w->e.id);
                if (nf > 0 && w->e.end_at > 0 && nf > w->e.end_at) nf = NF_DONE;
            }

            /* Write back if the live entry is still the one we snapshotted. */
            rtos_mutex_take(s.mutex, 0xFFFFFFFFUL);
            int idx = find_idx(w->e.id);
            if (idx >= 0 && s.entries[idx].next_fire == NF_PENDING) {
                if (w->e.kind == KIND_ONCE && nf == NF_DONE) {
                    /* F2: a finished one-shot is removed so completed reminders
                     * cannot pile up and eventually fill the job table. */
                    for (int i = idx; i < s.count - 1; i++) s.entries[i] = s.entries[i + 1];
                    s.count--;
                    changed_defs = 1;
                } else {
                    s.entries[idx].next_fire = nf;
                }
                changed_state = 1;
            }
            rtos_mutex_give(s.mutex);
        }
        if (changed_state || changed_defs) {
            rtos_mutex_take(s.mutex, 0xFFFFFFFFUL);
            if (changed_defs) save_defs();
            save_state();
            rtos_mutex_give(s.mutex);
        }
    }

    free(work);
    rtos_task_delete(NULL);
}

/* ---- add / upsert ---- */

/* Parse a local datetime "YYYY-MM-DD HH:MM[:SS]" → UTC epoch, using cap_time's
 * offset. Returns 0 on parse failure. */
static time_t parse_local_datetime(const char *str, long off_sec)
{
    int y, mo, d, h, mi, se = 0;
    int got = sscanf(str, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se);
    if (got < 5) return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59)
        return 0;
    time_t local_epoch = tm_to_epoch_utc(y, mo, d, h, mi, se);
    return local_epoch - (time_t)off_sec;
}

static int cap_add(const char *input_json, const claw_cap_call_context_t *ctx, char **output)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) { *output = strdup("{\"error\":\"invalid json\"}"); return RTK_FAIL; }

    sched_entry_t tmp;
    _memset(&tmp, 0, sizeof(tmp));

    /* --- action --- */
    char act[16] = {0};
    js(root, "action", act, sizeof(act));
    js(root, "prompt",       tmp.prompt,       sizeof(tmp.prompt));
    js(root, "cap_id",       tmp.cap_id,       sizeof(tmp.cap_id));
    /* cap_args accepts object or string */
    cJSON *jca = cJSON_GetObjectItem(root, "cap_args");
    if (jca) {
        if (cJSON_IsString(jca)) strlcpy(tmp.cap_args, jca->valuestring, sizeof(tmp.cap_args));
        else if (cJSON_IsObject(jca)) {
            char *cs = cJSON_PrintUnformatted(jca);
            if (cs) { strlcpy(tmp.cap_args, cs, sizeof(tmp.cap_args)); free(cs); }
        }
    }
    js(root, "event_type",   tmp.event_type,   sizeof(tmp.event_type));
    js(root, "payload_json", tmp.payload_json, sizeof(tmp.payload_json));

    if (!action_from_str(act, &tmp.action)) {
        /* infer */
        if (tmp.prompt[0])      tmp.action = ACT_AGENT;
        else if (tmp.cap_id[0]) tmp.action = ACT_CAP;
        else if (tmp.event_type[0]) tmp.action = ACT_EMIT;
        else tmp.action = ACT_AGENT;
    }

    /* --- trigger --- */
    char kindstr[16] = {0};
    js(root, "kind", kindstr, sizeof(kindstr));
    js(root, "cron_expr",     tmp.cron_expr,     sizeof(tmp.cron_expr));
    js(root, "trigger_event", tmp.trigger_event, sizeof(tmp.trigger_event));
    tmp.interval_sec = (uint32_t)jn(root, "interval_sec", 0);
    tmp.max_runs     = (uint32_t)jn(root, "max_runs", 0);

    long off = 0;
    bool tz_set = cap_time_get_tz_offset_sec(&off);
    time_t now = time(NULL);

    /* "at" = local datetime string; "in_sec" = relative seconds (for once). */
    char atbuf[32] = {0};
    js(root, "at", atbuf, sizeof(atbuf));
    double in_sec = jn(root, "in_sec", -1);
    if (atbuf[0] && tz_set) tmp.start_at = parse_local_datetime(atbuf, off);
    else if (in_sec >= 0 && now >= CLAW_TIME_MIN_VALID_UNIX) tmp.start_at = now + (time_t)in_sec;
    /* end_at also accepts a local datetime */
    char endbuf[32] = {0};
    js(root, "end_at", endbuf, sizeof(endbuf));
    if (endbuf[0] && tz_set) tmp.end_at = parse_local_datetime(endbuf, off);

    if (!kind_from_str(kindstr, &tmp.kind)) {
        /* infer */
        if (tmp.cron_expr[0])          tmp.kind = KIND_CRON;
        else if (tmp.trigger_event[0]) tmp.kind = KIND_ON_EVENT;
        else if (tmp.interval_sec > 0) tmp.kind = KIND_INTERVAL;
        else                            tmp.kind = KIND_ONCE;
    }

    /* --- validate per kind --- */
    if (tmp.kind == KIND_CRON) {
        cron_expr_t c;
        if (!tmp.cron_expr[0] || !cron_parse(tmp.cron_expr, &c)) {
            cJSON_Delete(root);
            *output = strdup("{\"error\":\"invalid or missing cron_expr (5 fields: min hour day month weekday)\"}");
            return RTK_FAIL;
        }
    } else if (tmp.kind == KIND_ON_EVENT) {
        if (!tmp.trigger_event[0]) {
            cJSON_Delete(root);
            *output = strdup("{\"error\":\"on_event requires trigger_event (e.g. wifi_connected)\"}");
            return RTK_FAIL;
        }
        if (!event_is_known(tmp.trigger_event)) {
            char eb[192];
            known_events_error(eb, sizeof(eb), tmp.trigger_event);
            cJSON_Delete(root);
            *output = strdup(eb);
            return RTK_FAIL;
        }
    } else if (tmp.kind == KIND_INTERVAL) {
        if (tmp.interval_sec < CLAW_SCHEDULER_MIN_INTERVAL_SEC) {
            RTK_LOGW(TAG, "interval_sec %u below floor, clamped to %u\n",
                     (unsigned)tmp.interval_sec, (unsigned)CLAW_SCHEDULER_MIN_INTERVAL_SEC);
            tmp.interval_sec = CLAW_SCHEDULER_MIN_INTERVAL_SEC;
        }
    } else { /* once */
        if (tmp.start_at <= 0) {
            cJSON_Delete(root);
            *output = strdup("{\"error\":\"once requires 'at' (local 'YYYY-MM-DD HH:MM') or 'in_sec'; and a synced clock + timezone\"}");
            return RTK_FAIL;
        }
    }

    if (tmp.action == ACT_AGENT && !tmp.prompt[0]) {
        cJSON_Delete(root);
        *output = strdup("{\"error\":\"action=agent requires a 'prompt'\"}");
        return RTK_FAIL;
    }
    if (tmp.action == ACT_CAP && !tmp.cap_id[0]) {
        cJSON_Delete(root);
        *output = strdup("{\"error\":\"action=cap requires 'cap_id'\"}");
        return RTK_FAIL;
    }

    /* --- capture target session from caller ctx --- */
    if (ctx && ctx->channel && ctx->channel[0]) strlcpy(tmp.channel, ctx->channel, sizeof(tmp.channel));
    if (ctx && ctx->chat_id && ctx->chat_id[0]) strlcpy(tmp.chat_id, ctx->chat_id, sizeof(tmp.chat_id));
    tmp.session_policy = (int)jn(root, "session_policy", 0);
    tmp.enabled = 1;
    tmp.next_fire = (tmp.kind == KIND_ON_EVENT) ? NF_DONE : NF_ARM;

    char reqid[32] = {0};
    js(root, "id", reqid, sizeof(reqid));

    /* F1: arm timer kinds NOW (cron scan off-lock) so the result reports the
     * real next fire — or a clear warning when the clock/timezone isn't ready,
     * instead of the task silently sitting suspended with no feedback. */
    char status[144] = {0};
    if (tmp.kind != KIND_ON_EVENT) {
        time_t now2 = time(NULL);
        bool synced = now2 >= CLAW_TIME_MIN_VALID_UNIX;
        long off2 = 0;
        bool tzset = cap_time_get_tz_offset_sec(&off2);
        if (synced && tzset) {
            tmp.next_fire = compute_arm(tmp.kind, now2, off2, tmp.interval_sec,
                                        tmp.cron_expr, tmp.start_at,
                                        reqid[0] ? reqid : "(new)");
            if (tmp.next_fire > 0) {
                char lb[32];
                epoch_to_local_str(tmp.next_fire, lb, sizeof(lb));
                DiagSnPrintf(status, sizeof(status), ",\"next_fire\":\"%s\"", lb);
            } else {
                strlcpy(status, ",\"warning\":\"no future fire — check the expression/time\"",
                        sizeof(status));
            }
        } else {
            tmp.next_fire = NF_ARM;   /* stays suspended until clock+tz ready */
            strlcpy(status, !synced
                ? ",\"warning\":\"created but SUSPENDED: clock not synced yet (needs network); it will start automatically after time sync\""
                : ",\"warning\":\"created but SUSPENDED: timezone not set — ask the user their timezone and call set_timezone, or it will never fire\"",
                sizeof(status));
        }
    }

    rtos_mutex_take(s.mutex, 0xFFFFFFFFUL);
    int idx = reqid[0] ? find_idx(reqid) : -1;
    if (idx < 0 && s.count >= s.max_jobs) {
        rtos_mutex_give(s.mutex);
        cJSON_Delete(root);
        *output = strdup("{\"error\":\"max jobs reached\"}");
        return RTK_FAIL;
    }
    if (idx < 0) { idx = s.count; s.count++; }
    /* preserve id if provided, else generate */
    if (reqid[0]) strlcpy(tmp.id, reqid, sizeof(tmp.id));
    else DiagSnPrintf(tmp.id, sizeof(tmp.id), "sch_%d", idx);
    s.entries[idx] = tmp;
    save_defs();
    save_state();
    char id_copy[32];
    strlcpy(id_copy, s.entries[idx].id, sizeof(id_copy));
    rtos_mutex_give(s.mutex);
    cJSON_Delete(root);

    const char *fmt = "{\"ok\":true,\"id\":\"%s\",\"kind\":\"%s\",\"action\":\"%s\"%s}";
    int n = DiagSnPrintf(NULL, 0, fmt, id_copy, kind_str(tmp.kind), action_str(tmp.action), status);
    *output = malloc((size_t)n + 1);
    if (!*output) return RTK_ERR_NOMEM;
    DiagSnPrintf(*output, (size_t)n + 1, fmt, id_copy, kind_str(tmp.kind), action_str(tmp.action), status);
    return RTK_SUCCESS;
}

/* ---- list / get ---- */

static void entry_to_view(cJSON *o, const sched_entry_t *e)
{
    cJSON_AddStringToObject(o, "id",     e->id);
    cJSON_AddStringToObject(o, "kind",   kind_str(e->kind));
    cJSON_AddStringToObject(o, "action", action_str(e->action));
    cJSON_AddBoolToObject(o, "enabled", e->enabled);
    cJSON_AddBoolToObject(o, "paused",  e->paused);
    if (e->cron_expr[0])     cJSON_AddStringToObject(o, "cron_expr", e->cron_expr);
    if (e->interval_sec)     cJSON_AddNumberToObject(o, "interval_sec", (double)e->interval_sec);
    if (e->trigger_event[0]) cJSON_AddStringToObject(o, "trigger_event", e->trigger_event);
    if (e->prompt[0])        cJSON_AddStringToObject(o, "prompt", e->prompt);
    if (e->cap_id[0])        cJSON_AddStringToObject(o, "cap_id", e->cap_id);
    /* Echo the action's payload too — without cap_args/event_type a cap/emit
     * task's actual behaviour was un-inspectable from list/get, so "created but
     * misbehaving" could not be told apart from "created correctly". */
    if (e->cap_args[0])      cJSON_AddStringToObject(o, "cap_args", e->cap_args);
    if (e->event_type[0])    cJSON_AddStringToObject(o, "event_type", e->event_type);
    if (e->payload_json[0])  cJSON_AddStringToObject(o, "payload_json", e->payload_json);
    cJSON_AddNumberToObject(o, "run_count",    (double)e->run_count);
    cJSON_AddNumberToObject(o, "missed_count", (double)e->missed_count);

    char buf[32];
    if (e->kind != KIND_ON_EVENT) {
        if (e->next_fire == NF_DONE)      cJSON_AddStringToObject(o, "next_fire", "completed");
        else if (e->next_fire <= 0)       cJSON_AddStringToObject(o, "next_fire", "pending");
        else { epoch_to_local_str(e->next_fire, buf, sizeof(buf));
               cJSON_AddStringToObject(o, "next_fire", buf); }
    }
    if (e->last_fire > 0) {
        epoch_to_local_str(e->last_fire, buf, sizeof(buf));
        cJSON_AddStringToObject(o, "last_fire", buf);
    }
}

static int cap_list(const char *input_json, const claw_cap_call_context_t *ctx, char **output)
{
    (void)input_json; (void)ctx;
    rtos_mutex_take(s.mutex, 0xFFFFFFFFUL);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s.count; i++) {
        cJSON *o = cJSON_CreateObject();
        if (!o) break;
        entry_to_view(o, &s.entries[i]);
        cJSON_AddItemToArray(arr, o);
    }
    rtos_mutex_give(s.mutex);
    *output = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!*output) *output = strdup("[]");
    return RTK_SUCCESS;
}

static int cap_get(const char *input_json, const claw_cap_call_context_t *ctx, char **output)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(input_json);
    cJSON *jid = root ? cJSON_GetObjectItem(root, "id") : NULL;
    if (!jid || !cJSON_IsString(jid)) {
        cJSON_Delete(root);
        claw_cap_set_output(output, "{\"error\":\"id required\"}");
        return RTK_FAIL;
    }
    rtos_mutex_take(s.mutex, 0xFFFFFFFFUL);
    int idx = find_idx(jid->valuestring);
    cJSON *o = NULL;
    if (idx >= 0) { o = cJSON_CreateObject(); if (o) entry_to_view(o, &s.entries[idx]); }
    rtos_mutex_give(s.mutex);
    cJSON_Delete(root);
    if (idx < 0) { claw_cap_set_output(output, "{\"error\":\"not found\"}"); return RTK_FAIL; }
    *output = o ? cJSON_PrintUnformatted(o) : strdup("{}");
    if (o) cJSON_Delete(o);
    return RTK_SUCCESS;
}

/* ---- remove / enable / disable / pause / resume ---- */

static int id_op(const char *input_json, char **output, int op)
{
    /* op: 0=remove 1=enable 2=disable 3=pause 4=resume 5=trigger_now */
    cJSON *root = cJSON_Parse(input_json);
    cJSON *jid = root ? cJSON_GetObjectItem(root, "id") : NULL;
    if (!jid || !cJSON_IsString(jid)) {
        cJSON_Delete(root);
        claw_cap_set_output(output, "{\"error\":\"id required\"}");
        return RTK_FAIL;
    }

    sched_entry_t fire_copy;
    int do_fire = 0;

    rtos_mutex_take(s.mutex, 0xFFFFFFFFUL);
    int idx = find_idx(jid->valuestring);
    if (idx >= 0) {
        sched_entry_t *e = &s.entries[idx];
        switch (op) {
        case 0: /* remove */
            for (int i = idx; i < s.count - 1; i++) s.entries[i] = s.entries[i + 1];
            s.count--;
            save_defs(); save_state();
            break;
        case 1: e->enabled = 1; if (e->kind != KIND_ON_EVENT) e->next_fire = NF_ARM; save_defs(); break;
        case 2: e->enabled = 0; save_defs(); break;
        case 3: e->paused = 1; save_defs(); break;
        case 4: e->paused = 0; if (e->kind != KIND_ON_EVENT) e->next_fire = NF_ARM; save_defs(); break;
        case 5: fire_copy = *e; do_fire = 1; break;   /* trigger_now */
        }
    }
    rtos_mutex_give(s.mutex);
    cJSON_Delete(root);

    if (idx < 0) { claw_cap_set_output(output, "{\"error\":\"not found\"}"); return RTK_FAIL; }
    if (do_fire) execute_action(&fire_copy);
    return claw_cap_set_output(output, "{\"ok\":true}");
}

static int cap_remove (const char *in, const claw_cap_call_context_t *c, char **o){ (void)c; return id_op(in,o,0); }
static int cap_enable (const char *in, const claw_cap_call_context_t *c, char **o){ (void)c; return id_op(in,o,1); }
static int cap_disable(const char *in, const claw_cap_call_context_t *c, char **o){ (void)c; return id_op(in,o,2); }
static int cap_pause  (const char *in, const claw_cap_call_context_t *c, char **o){ (void)c; return id_op(in,o,3); }
static int cap_resume (const char *in, const claw_cap_call_context_t *c, char **o){ (void)c; return id_op(in,o,4); }
static int cap_trigger(const char *in, const claw_cap_call_context_t *c, char **o){ (void)c; return id_op(in,o,5); }

/* ---- Cap descriptors ---- */

#define ID_SCHEMA "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}"

static const claw_cap_descriptor_t s_caps[] = {
    {
        .id = "scheduler_add_job", .name = "scheduler_add_job", .family = "scheduler",
        .description =
            "Create or update (upsert by id) a scheduled task. A task has a TRIGGER (when) and an ACTION (what)."
            "\n\nTRIGGER — set `kind` (or it is inferred):"
            "\n- cron: repeating wall-clock. Set `cron_expr` = 5 fields 'min hour day month weekday'. "
            "Supports ranges/lists/steps and names, e.g. '0 9 * * 1-5' = 09:00 Mon–Fri, '30 8 * * *' = 08:30 daily."
            "\n- once: one time. Set `at`='YYYY-MM-DD HH:MM' (LOCAL time) or `in_sec`=<seconds from now>. It fires EXACTLY ONCE then is auto-removed — a completed once does NOT come back after reboot. For anything recurring use cron/interval; to run on every power-up use on_event."
            "\n- interval: every N seconds. Set `interval_sec`."
            "\n- on_event: on a system event. Set `trigger_event` to a SUPPORTED event — currently only 'wifi_connected' (device came online; fires once per boot, before clock sync). Any other name is rejected. Works before clock sync, so this is the trigger for run-on-boot."
            "\n\nACTION — set `action` (or it is inferred):"
            "\n- agent (default): set `prompt` = a natural-language instruction; at fire time the agent runs it in YOUR conversation and can use any tool (search, send, etc). Best for reminders/'search news and tell me'."
            "\n- cap: set `cap_id` (+`cap_args`) to call a capability directly (deterministic, no LLM), e.g. run a Lua script."
            "\n- emit: set `event_type`(+`payload_json`) to publish a raw event for router rules (advanced)."
            "\n\nEXAMPLE — run a Lua script on every boot: "
            "{\"kind\":\"on_event\",\"trigger_event\":\"wifi_connected\",\"action\":\"cap\",\"cap_id\":\"lua_run\",\"cap_args\":{\"path\":\"vfs:/scripts/foo.lua\"}}. "
            "Use cap_id 'lua_run_async' instead for a script that loops forever (monitor/animation). The script must live under vfs:/scripts/ (survives reboot) and define a global run()."
            "\n\nOptional: `max_runs` (0=unlimited), `end_at` (local datetime), `id` (to update)."
            "\n\nNOTE: cron/once need a synced clock AND a timezone. If get_local_time says timezone is not configured, ask the user and call set_timezone first. The task inherits your channel/chat, so an agent reminder replies to you automatically.",
        .kind = CLAW_CAP_KIND_INVOKE, .cap_flags = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"id\":{\"type\":\"string\",\"description\":\"Task id; reuse to update an existing task\"},"
            "\"kind\":{\"type\":\"string\",\"enum\":[\"once\",\"interval\",\"cron\",\"on_event\"]},"
            "\"cron_expr\":{\"type\":\"string\",\"description\":\"cron: 'min hour day month weekday', e.g. '0 9 * * 1-5'\"},"
            "\"at\":{\"type\":\"string\",\"description\":\"once: local datetime 'YYYY-MM-DD HH:MM'\"},"
            "\"in_sec\":{\"type\":\"number\",\"description\":\"once: seconds from now\"},"
            "\"interval_sec\":{\"type\":\"number\",\"description\":\"interval: period in seconds\"},"
            "\"trigger_event\":{\"type\":\"string\",\"description\":\"on_event: system event name e.g. wifi_connected\"},"
            "\"action\":{\"type\":\"string\",\"enum\":[\"agent\",\"cap\",\"emit\"]},"
            "\"prompt\":{\"type\":\"string\",\"description\":\"action=agent: what to do when it fires. Phrase it as the message to DELIVER (e.g. 'tell the user it is time for the morning meeting'), not 'remind me to…'. It runs one agent turn and should just deliver/act once — it must not create new scheduled tasks.\"},"
            "\"cap_id\":{\"type\":\"string\",\"description\":\"action=cap: capability to call\"},"
            "\"cap_args\":{\"description\":\"action=cap: args (object or JSON string)\"},"
            "\"event_type\":{\"type\":\"string\",\"description\":\"action=emit: event to publish\"},"
            "\"payload_json\":{\"type\":\"string\"},"
            "\"max_runs\":{\"type\":\"number\"},"
            "\"end_at\":{\"type\":\"string\",\"description\":\"local datetime to stop after\"}"
            "}}",
        .execute = cap_add,
    },
    { .id="scheduler_list_jobs", .name="scheduler_list_jobs", .family="scheduler",
      .description="List all scheduled tasks with kind, action, next_fire (local time) and run/missed counts.",
      .kind=CLAW_CAP_KIND_INVOKE, .cap_flags=CLAW_CAP_FLAG_LLM_ACCESS,
      .input_schema_json="{\"type\":\"object\",\"properties\":{}}", .execute=cap_list },
    { .id="scheduler_get_job", .name="scheduler_get_job", .family="scheduler",
      .description="Get one scheduled task by id (full detail incl. next_fire in local time).",
      .kind=CLAW_CAP_KIND_INVOKE, .cap_flags=CLAW_CAP_FLAG_LLM_ACCESS,
      .input_schema_json=ID_SCHEMA, .execute=cap_get },
    { .id="scheduler_remove_job", .name="scheduler_remove_job", .family="scheduler",
      .description="Remove a scheduled task by id.",
      .kind=CLAW_CAP_KIND_INVOKE, .cap_flags=CLAW_CAP_FLAG_LLM_ACCESS,
      .input_schema_json=ID_SCHEMA, .execute=cap_remove },
    { .id="scheduler_enable_job", .name="scheduler_enable_job", .family="scheduler",
      .description="Enable a disabled task by id.",
      .kind=CLAW_CAP_KIND_INVOKE, .cap_flags=CLAW_CAP_FLAG_LLM_ACCESS,
      .input_schema_json=ID_SCHEMA, .execute=cap_enable },
    { .id="scheduler_disable_job", .name="scheduler_disable_job", .family="scheduler",
      .description="Disable a task by id (keeps definition; stops firing).",
      .kind=CLAW_CAP_KIND_INVOKE, .cap_flags=CLAW_CAP_FLAG_LLM_ACCESS,
      .input_schema_json=ID_SCHEMA, .execute=cap_disable },
    { .id="scheduler_pause_job", .name="scheduler_pause_job", .family="scheduler",
      .description="Temporarily pause a task by id without disabling it (e.g. 'skip today's alarm'). Resume with scheduler_resume_job.",
      .kind=CLAW_CAP_KIND_INVOKE, .cap_flags=CLAW_CAP_FLAG_LLM_ACCESS,
      .input_schema_json=ID_SCHEMA, .execute=cap_pause },
    { .id="scheduler_resume_job", .name="scheduler_resume_job", .family="scheduler",
      .description="Resume a paused task by id.",
      .kind=CLAW_CAP_KIND_INVOKE, .cap_flags=CLAW_CAP_FLAG_LLM_ACCESS,
      .input_schema_json=ID_SCHEMA, .execute=cap_resume },
    { .id="scheduler_trigger_now", .name="scheduler_trigger_now", .family="scheduler",
      .description="Run a task's action immediately once (for testing it works), without changing its schedule.",
      .kind=CLAW_CAP_KIND_INVOKE, .cap_flags=CLAW_CAP_FLAG_LLM_ACCESS,
      .input_schema_json=ID_SCHEMA, .execute=cap_trigger },
};

static const claw_cap_group_t s_group = {
    .group_id = "scheduler", .plugin_name = "cap_scheduler", .version = "2",
    .descriptors = s_caps, .descriptor_count = sizeof(s_caps)/sizeof(s_caps[0]),
};

/* ---- Public API ---- */

int cap_scheduler_init(const cap_scheduler_config_t *config)
{
    _memset(&s, 0, sizeof(s));
    s.max_jobs = (config->max_jobs < MAX_JOBS) ? (int)config->max_jobs : MAX_JOBS;

    mkdir(config->schedule_root_dir, 0777);
    DiagSnPrintf(s.def_file,   sizeof(s.def_file),   "%s/schedules.json", config->schedule_root_dir);
    DiagSnPrintf(s.state_file, sizeof(s.state_file), "%s/state.json",     config->schedule_root_dir);

    if (rtos_mutex_create(&s.mutex) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "mutex create failed\n");
        return RTK_FAIL;
    }
    load_all();

    int err = claw_cap_register_group(&s_group);
    if (err != RTK_SUCCESS) { RTK_LOGE(TAG, "register group failed: %d\n", err); return err; }

    RTK_LOGI(TAG, "init (dir=%s, max=%d, loaded %d)\n",
             config->schedule_root_dir, s.max_jobs, s.count);
    return RTK_SUCCESS;
}

int cap_scheduler_start(void)
{
    s.running = 1;
    if (rtos_task_create(&s.task, "sched_task", scheduler_task, NULL, 4096, 1) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "task create failed\n");
        s.running = 0;
        return RTK_FAIL;
    }
    return RTK_SUCCESS;
}

int cap_scheduler_stop(void)
{
    s.running = 0;
    return RTK_SUCCESS;
}

/* ---- Lifecycle registration ---- */

static void scheduler_on_wifi_connected(void)
{
    cap_scheduler_fire_event("wifi_connected");
}
static void scheduler_on_init(const claw_config_t *cfg)
{
    (void)cfg;
    const cap_scheduler_config_t c = { .schedule_root_dir = "vfs:/scheduler",
                                       .max_jobs = CLAW_SCHEDULER_MAX_JOBS };
    cap_scheduler_init(&c);
}
static void scheduler_on_io(const claw_config_t *cfg)
{
    (void)cfg;
    cap_scheduler_start();
    claw_wifi_mgr_register_on_connected(scheduler_on_wifi_connected);
}

CLAW_CAP_REGISTER(scheduler, {
    .group   = "scheduler",
    .order   = 85,
    .on_init = scheduler_on_init,
    .on_io   = scheduler_on_io,
});

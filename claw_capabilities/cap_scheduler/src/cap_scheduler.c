/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_scheduler.h"
#include "ameba_claw_defs.h"
#include "claw_cap.h"
#include "claw_event_publisher.h"
#include <cJSON.h>
#include "platform_stdlib.h"
#include "os_wrapper.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TAG "cap_scheduler"
#define MAX_JOBS CLAW_SCHEDULER_MAX_JOBS

/* ---- Internal job structure ---- */

typedef struct {
    char id[32];
    char event_type[48];
    char payload_json[256];
    char cap_id[48];      /* optional: call this cap directly on fire */
    char cap_args[256];   /* args json for the direct cap call */
    uint32_t interval_sec;
    int enabled;
    uint32_t next_fire_ms;
} sched_job_t;

/* ---- Runtime state ---- */

static struct {
    sched_job_t jobs[MAX_JOBS];
    int job_count;
    rtos_mutex_t mutex;
    rtos_task_t task;
    int running;
    char schedule_file[128];
    int max_jobs;
} s;

/* ---- Forward declarations ---- */

static int cap_add_job(const char *input_json, const claw_cap_call_context_t *ctx,
                              char **output);
static int cap_list_jobs(const char *input_json, const claw_cap_call_context_t *ctx,
                                char **output);
static int cap_remove_job(const char *input_json, const claw_cap_call_context_t *ctx,
                                 char **output);
static int cap_enable_job(const char *input_json, const claw_cap_call_context_t *ctx,
                                 char **output);
static int cap_disable_job(const char *input_json, const claw_cap_call_context_t *ctx,
                                  char **output);

/* ---- File persistence ---- */

static void save_jobs(void)
{
    cJSON *root = cJSON_CreateArray();
    if (!root) {
        return;
    }

    uint32_t now_ms = rtos_time_get_current_system_time_ms();
    for (int i = 0; i < s.job_count; i++) {
        cJSON *job = cJSON_CreateObject();
        cJSON_AddStringToObject(job, "id", s.jobs[i].id);
        cJSON_AddStringToObject(job, "event_type", s.jobs[i].event_type);
        cJSON_AddStringToObject(job, "payload_json", s.jobs[i].payload_json);
        if (s.jobs[i].cap_id[0])
            cJSON_AddStringToObject(job, "cap_id",   s.jobs[i].cap_id);
        if (s.jobs[i].cap_args[0])
            cJSON_AddStringToObject(job, "cap_args", s.jobs[i].cap_args);
        cJSON_AddNumberToObject(job, "interval_sec", (double)s.jobs[i].interval_sec);
        cJSON_AddNumberToObject(job, "enabled", s.jobs[i].enabled);
        /* Save remaining delay so the job fires at the right time after reboot.
         * If next_fire_ms is in the past (already fired), save delay_sec=0 so
         * the job fires promptly on next boot. */
        uint32_t remaining = (s.jobs[i].next_fire_ms > now_ms)
                             ? (s.jobs[i].next_fire_ms - now_ms) / 1000u
                             : 0;
        cJSON_AddNumberToObject(job, "delay_sec", (double)remaining);
        cJSON_AddItemToArray(root, job);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        RTK_LOGE(TAG, "save_jobs: cJSON_PrintUnformatted failed (OOM?)\n");
        return;
    }

    /* Use fwrite instead of fputs to handle strings with embedded NULs and
     * to get a reliable return-value indicating actual bytes written. */
    size_t json_len = strlen(json_str);
    FILE *f = fopen(s.schedule_file, "w");
    if (f) {
        size_t written = fwrite(json_str, 1, json_len, f);
        fclose(f);
        if (written != json_len) {
            RTK_LOGE(TAG, "save_jobs: fwrite incomplete (%u/%u) — fs full?\n",
                     (unsigned)written, (unsigned)json_len);
        }
    } else {
        RTK_LOGE(TAG, "save_jobs: failed to open %s for write\n", s.schedule_file);
    }

    free(json_str);
}

static void load_jobs(void)
{
    FILE *f = fopen(s.schedule_file, "r");
    if (!f) {
        return;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 16384) {
        fclose(f);
        return;
    }

    char *buf = malloc((size_t)fsize + 1);
    if (!buf) {
        fclose(f);
        return;
    }

    size_t nread = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    buf[nread] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        RTK_LOGW(TAG, "load_jobs: JSON parse failed (corrupt file?)\n");
        return;
    }

    /* Accept both the canonical array format  [{"id":...}, ...]
     * and the LLM-friendly object format       {"jobs": [...]}  */
    if (cJSON_IsObject(root)) {
        cJSON *arr = cJSON_GetObjectItem(root, "jobs");
        if (arr && cJSON_IsArray(arr)) {
            cJSON *arr_copy = cJSON_Duplicate(arr, 1);
            cJSON_Delete(root);
            root = arr_copy;
        } else {
            RTK_LOGW(TAG, "load_jobs: object root has no 'jobs' array\n");
            cJSON_Delete(root);
            return;
        }
    }

    if (!cJSON_IsArray(root)) {
        RTK_LOGW(TAG, "load_jobs: expected JSON array, got type %d\n", root->type);
        cJSON_Delete(root);
        return;
    }

    int count = 0;
    int max = s.max_jobs < MAX_JOBS ? s.max_jobs : MAX_JOBS;
    cJSON *item = NULL;

    cJSON_ArrayForEach(item, root) {
        if (count >= max) {
            break;
        }

        cJSON *jid   = cJSON_GetObjectItem(item, "id");
        cJSON *jevt  = cJSON_GetObjectItem(item, "event_type");
        cJSON *jpay  = cJSON_GetObjectItem(item, "payload_json");
        cJSON *jcap  = cJSON_GetObjectItem(item, "cap_id");
        cJSON *jcapa = cJSON_GetObjectItem(item, "cap_args");
        cJSON *jint  = cJSON_GetObjectItem(item, "interval_sec");
        cJSON *jenbl = cJSON_GetObjectItem(item, "enabled");

        if (!jid || !jevt) {
            continue;
        }

        sched_job_t *job = &s.jobs[count];
        strncpy(job->id, jid->valuestring, sizeof(job->id) - 1);
        job->id[sizeof(job->id) - 1] = '\0';
        strncpy(job->event_type, jevt->valuestring, sizeof(job->event_type) - 1);
        job->event_type[sizeof(job->event_type) - 1] = '\0';

        if (jpay && cJSON_IsString(jpay)) {
            strncpy(job->payload_json, jpay->valuestring, sizeof(job->payload_json) - 1);
            job->payload_json[sizeof(job->payload_json) - 1] = '\0';
        } else {
            job->payload_json[0] = '\0';
        }

        if (jcap && cJSON_IsString(jcap)) {
            strncpy(job->cap_id, jcap->valuestring, sizeof(job->cap_id) - 1);
            job->cap_id[sizeof(job->cap_id) - 1] = '\0';
        } else {
            job->cap_id[0] = '\0';
        }
        if (jcapa && cJSON_IsString(jcapa)) {
            strncpy(job->cap_args, jcapa->valuestring, sizeof(job->cap_args) - 1);
            job->cap_args[sizeof(job->cap_args) - 1] = '\0';
        } else {
            job->cap_args[0] = '\0';
        }

        job->interval_sec = (jint && cJSON_IsNumber(jint)) ? (uint32_t)jint->valuedouble : 60;
        job->enabled      = (jenbl && cJSON_IsNumber(jenbl)) ? (int)jenbl->valuedouble : 1;

        /* Use persisted delay_sec for the first fire after reboot so the job
         * respects its configured initial delay even across restarts.
         * Fall back to interval_sec when the field is absent (old files). */
        cJSON *jdel_saved = cJSON_GetObjectItem(item, "delay_sec");
        uint32_t first_delay = (jdel_saved && cJSON_IsNumber(jdel_saved))
                               ? (uint32_t)jdel_saved->valuedouble
                               : job->interval_sec;
        job->next_fire_ms = rtos_time_get_current_system_time_ms()
                            + (uint32_t)((uint64_t)first_delay * 1000u);

        count++;
    }

    s.job_count = count;
    cJSON_Delete(root);
}

/* ---- Internal helpers ---- */

/* Returns the index of the job with the given id, or -1 if not found.
 * Caller must hold s.mutex. */
static int find_job_idx(const char *id)
{
    for (int i = 0; i < s.job_count; i++) {
        if (strcmp(s.jobs[i].id, id) == 0) return i;
    }
    return -1;
}

/* ---- Cap execute functions ---- */

static int cap_add_job(const char *input_json, const claw_cap_call_context_t *ctx,
                              char **output)
{
    (void)ctx;

    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        *output = strdup("{\"error\":\"invalid json\"}");
        return RTK_FAIL;
    }

    cJSON *jevt  = cJSON_GetObjectItem(root, "event_type");
    cJSON *jcap  = cJSON_GetObjectItem(root, "cap_id");
    cJSON *jcapa = cJSON_GetObjectItem(root, "cap_args");

    /* At least one of event_type or cap_id is required */
    if ((!jevt || !cJSON_IsString(jevt)) && (!jcap || !cJSON_IsString(jcap))) {
        cJSON_Delete(root);
        *output = strdup("{\"error\":\"event_type or cap_id required\"}");
        return RTK_FAIL;
    }

    /* Guard: vfs:/tmp/ scripts are wiped on reboot — a scheduled job firing after
     * restart would silently fail.  Check at job-creation time so the LLM gets an
     * actionable error immediately (fire-time errors are not visible to the LLM).
     * This covers both lua_run and lua_run_async.  cap_lua enforces the same rule
     * as a defence-in-depth for internal callers. */
    if (jcap && cJSON_IsString(jcap) && jcapa && cJSON_IsString(jcapa)) {
        const char *cap_id   = jcap->valuestring;
        const char *cap_args = jcapa->valuestring;
        bool is_lua = (strncmp(cap_id, "lua_run", 7) == 0 &&
                       (cap_id[7] == '\0' || cap_id[7] == '_'));
        if (is_lua && strstr(cap_args, "vfs:/tmp/")) {
            cJSON_Delete(root);
            *output = strdup(
                "{\"error\":\"vfs:/tmp/ scripts are wiped on reboot — scheduled jobs would "
                "silently fail after restart. For IM reminders use cap_id=<reply_cap from "
                "conversation context> with cap_args={\\\"chat_id\\\":\\\"...\\\","
                "\\\"text\\\":\\\"...\\\"}. For persistent scripts use vfs:/scripts/.\"}");
            return RTK_FAIL;
        }
    }

    cJSON *jpay  = cJSON_GetObjectItem(root, "payload_json");
    cJSON *jint  = cJSON_GetObjectItem(root, "interval_sec");
    cJSON *jdel  = cJSON_GetObjectItem(root, "delay_sec");
    cJSON *jid   = cJSON_GetObjectItem(root, "id");

    uint32_t interval_sec = (jint && cJSON_IsNumber(jint)) ? (uint32_t)jint->valuedouble : 60;
    uint32_t delay_sec    = (jdel && cJSON_IsNumber(jdel)) ? (uint32_t)jdel->valuedouble : interval_sec;

    rtos_mutex_take(s.mutex, 0xFFFFFFFFUL);

    /* Resolve destination slot: upsert by id (id is the primary key).
     * If a job with this id already exists, reuse its slot — same semantics
     * as cron / systemd timer: a named job definition is unique by name.
     * Only allocate a new slot when no existing job matches. */
    const char *req_id = (jid && cJSON_IsString(jid)) ? jid->valuestring : NULL;
    int existing = req_id ? find_job_idx(req_id) : -1;

    sched_job_t *job;
    if (existing >= 0) {
        job = &s.jobs[existing];
    } else {
        /* New slot — check capacity first. */
        if (s.job_count >= s.max_jobs) {
            rtos_mutex_give(s.mutex);
            cJSON_Delete(root);
            *output = strdup("{\"error\":\"max jobs reached\"}");
            return RTK_FAIL;
        }
        job = &s.jobs[s.job_count];
        s.job_count++;
    }

    _memset(job, 0, sizeof(*job));

    if (req_id) {
        strncpy(job->id, req_id, sizeof(job->id) - 1);
    } else {
        DiagSnPrintf(job->id, sizeof(job->id), "job_%d", s.job_count - 1);
    }

    if (jevt && cJSON_IsString(jevt))
        strncpy(job->event_type, jevt->valuestring, sizeof(job->event_type) - 1);

    if (jpay && cJSON_IsString(jpay))
        strncpy(job->payload_json, jpay->valuestring, sizeof(job->payload_json) - 1);

    if (jcap && cJSON_IsString(jcap))
        strncpy(job->cap_id, jcap->valuestring, sizeof(job->cap_id) - 1);

    if (jcapa && cJSON_IsString(jcapa))
        strncpy(job->cap_args, jcapa->valuestring, sizeof(job->cap_args) - 1);

    job->interval_sec = interval_sec;
    job->enabled      = 1;
    job->next_fire_ms = rtos_time_get_current_system_time_ms()
                        + (uint32_t)((uint64_t)delay_sec * 1000u);

    save_jobs();

    rtos_mutex_give(s.mutex);

    char job_id_copy[32];
    strncpy(job_id_copy, job->id, sizeof(job_id_copy) - 1);
    job_id_copy[sizeof(job_id_copy) - 1] = '\0';
    cJSON_Delete(root);

    const char *fmt = "{\"ok\":true,\"id\":\"%s\"}";
    int n = DiagSnPrintf(NULL, 0, fmt, job_id_copy);
    *output = malloc((size_t)n + 1);
    if (*output) DiagSnPrintf(*output, (size_t)n + 1, fmt, job_id_copy);
    return *output ? RTK_SUCCESS : RTK_ERR_NOMEM;
}

static int cap_list_jobs(const char *input_json, const claw_cap_call_context_t *ctx,
                                char **output)
{
    (void)input_json;
    (void)ctx;

    rtos_mutex_take(s.mutex, 0xFFFFFFFFUL);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s.job_count; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id", s.jobs[i].id);
        cJSON_AddStringToObject(obj, "event_type", s.jobs[i].event_type);
        cJSON_AddNumberToObject(obj, "interval_sec", (double)s.jobs[i].interval_sec);
        cJSON_AddNumberToObject(obj, "enabled", s.jobs[i].enabled);
        cJSON_AddItemToArray(arr, obj);
    }

    rtos_mutex_give(s.mutex);

    *output = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if (!*output) {
        *output = strdup("[]");
    }

    return RTK_SUCCESS;
}

static int cap_remove_job(const char *input_json, const claw_cap_call_context_t *ctx,
                                 char **output)
{
    (void)ctx;

    cJSON *root = cJSON_Parse(input_json);
    cJSON *jid  = root ? cJSON_GetObjectItem(root, "id") : NULL;

    if (!jid || !cJSON_IsString(jid)) {
        cJSON_Delete(root);
        claw_cap_set_output(output, "{\"error\":\"id required\"}");
        return RTK_FAIL;
    }

    rtos_mutex_take(s.mutex, 0xFFFFFFFFUL);

    int found = find_job_idx(jid->valuestring);

    if (found >= 0) {
        /* shift remaining jobs */
        for (int i = found; i < s.job_count - 1; i++) {
            s.jobs[i] = s.jobs[i + 1];
        }
        s.job_count--;
        save_jobs();
    }

    rtos_mutex_give(s.mutex);

    cJSON_Delete(root);

    if (found >= 0) {
        return claw_cap_set_output(output, "{\"ok\":true}");
    } else {
        claw_cap_set_output(output, "{\"error\":\"job not found\"}");
        return RTK_FAIL;
    }
}

static int cap_enable_job(const char *input_json, const claw_cap_call_context_t *ctx,
                                 char **output)
{
    (void)ctx;

    cJSON *root = cJSON_Parse(input_json);
    cJSON *jid  = root ? cJSON_GetObjectItem(root, "id") : NULL;

    if (!jid || !cJSON_IsString(jid)) {
        cJSON_Delete(root);
        claw_cap_set_output(output, "{\"error\":\"id required\"}");
        return RTK_FAIL;
    }

    rtos_mutex_take(s.mutex, 0xFFFFFFFFUL);

    int idx = find_job_idx(jid->valuestring);
    if (idx >= 0) {
        s.jobs[idx].enabled = 1;
        save_jobs();
    }

    rtos_mutex_give(s.mutex);

    cJSON_Delete(root);

    if (idx >= 0) {
        return claw_cap_set_output(output, "{\"ok\":true}");
    } else {
        claw_cap_set_output(output, "{\"error\":\"job not found\"}");
        return RTK_FAIL;
    }
}

static int cap_disable_job(const char *input_json, const claw_cap_call_context_t *ctx,
                                  char **output)
{
    (void)ctx;

    cJSON *root = cJSON_Parse(input_json);
    cJSON *jid  = root ? cJSON_GetObjectItem(root, "id") : NULL;

    if (!jid || !cJSON_IsString(jid)) {
        cJSON_Delete(root);
        claw_cap_set_output(output, "{\"error\":\"id required\"}");
        return RTK_FAIL;
    }

    rtos_mutex_take(s.mutex, 0xFFFFFFFFUL);

    int idx = find_job_idx(jid->valuestring);
    if (idx >= 0) {
        s.jobs[idx].enabled = 0;
        save_jobs();
    }

    rtos_mutex_give(s.mutex);

    cJSON_Delete(root);

    if (idx >= 0) {
        return claw_cap_set_output(output, "{\"ok\":true}");
    } else {
        claw_cap_set_output(output, "{\"error\":\"job not found\"}");
        return RTK_FAIL;
    }
}

/* ---- Job execution helper ---- */

typedef struct {
    char evt[48]; char id[32]; char pay[256];
    char cap[48]; char cap_args[256];
} fired_t;

static void execute_fired_jobs(const fired_t *fired, int cnt)
{
    for (int i = 0; i < cnt; i++) {
        if (fired[i].cap[0]) {
            char *out = NULL;
            claw_cap_call_context_t ctx = {0};
            claw_cap_call(fired[i].cap,
                          fired[i].cap_args[0] ? fired[i].cap_args : "{}",
                          &ctx, &out);
            free(out);
        } else if (fired[i].evt[0]) {
            claw_event_dispatcher_publish_trigger(
                "cap_scheduler", fired[i].evt, fired[i].id, fired[i].pay);
        }
    }
}

/* Fire all enabled jobs whose event_type matches.  Thread-safe. */
void cap_scheduler_fire_event(const char *event_type)
{
    if (!s.running || !event_type || !event_type[0]) return;

    /* Heap-allocate the snapshot so this function is re-entrant:
     * cap_scheduler_fire_event may be called from any task (wifi_mgr,
     * Lua, LLM agent) concurrently with scheduler_task. */
    fired_t *fired = malloc(MAX_JOBS * sizeof(fired_t));
    if (!fired) {
        RTK_LOGE("cap_sched", "fire_event: OOM\n");
        return;
    }
    int  fired_cnt = 0;
    bool needs_save = false;

    uint32_t now_ms = rtos_time_get_current_system_time_ms();
    rtos_mutex_take(s.mutex, 0xFFFFFFFFUL);
    for (int i = 0; i < s.job_count; i++) {
        sched_job_t *job = &s.jobs[i];
        if (!job->enabled) continue;
        if (strcmp(job->event_type, event_type) != 0) continue;
        if (fired_cnt >= MAX_JOBS) break;

        /* Cooldown check: interval_sec on an event job means "minimum seconds
         * between consecutive fires of the same event".  next_fire_ms records
         * when the cooldown expires.  interval=0 → no cooldown, always fire.
         * On the very first fire after load next_fire_ms==0 (past) → always fires. */
        if (job->interval_sec > 0 &&
            (int32_t)(now_ms - job->next_fire_ms) < 0) {
            continue;   /* still in cooldown, skip this event */
        }

        strncpy(fired[fired_cnt].evt,      job->event_type,  sizeof(fired[0].evt)      - 1);
        strncpy(fired[fired_cnt].id,       job->id,          sizeof(fired[0].id)        - 1);
        strncpy(fired[fired_cnt].pay,      job->payload_json, sizeof(fired[0].pay)      - 1);
        strncpy(fired[fired_cnt].cap,      job->cap_id,      sizeof(fired[0].cap)       - 1);
        strncpy(fired[fired_cnt].cap_args, job->cap_args,    sizeof(fired[0].cap_args)  - 1);
        fired[fired_cnt].evt[sizeof(fired[0].evt)-1]           = '\0';
        fired[fired_cnt].id[sizeof(fired[0].id)-1]             = '\0';
        fired[fired_cnt].pay[sizeof(fired[0].pay)-1]           = '\0';
        fired[fired_cnt].cap[sizeof(fired[0].cap)-1]           = '\0';
        fired[fired_cnt].cap_args[sizeof(fired[0].cap_args)-1] = '\0';

        /* interval_sec semantics unified across both job types:
         *   interval>0  → minimum trigger interval (timer: re-arm period;
         *                  event: cooldown before next same-event fire)
         *   interval=0  → timer: one-shot disable; event: fire every occurrence
         * Only write flash when state actually changes. */
        if (job->interval_sec > 0) {
            job->next_fire_ms = now_ms + (uint32_t)((uint64_t)job->interval_sec * 1000u);
            needs_save = true;
        } else if (job->event_type[0] == '\0') {
            job->enabled = 0;   /* timer one-shot */
            needs_save = true;
        }
        /* event + interval=0: no state change, no flash write */
        fired_cnt++;
    }
    if (needs_save) save_jobs();
    rtos_mutex_give(s.mutex);

    execute_fired_jobs(fired, fired_cnt);
    free(fired);
}

/* ---- Scheduler task ---- */

static void scheduler_task(void *arg)
{
    (void)arg;

    while (s.running) {
        rtos_time_delay_ms(10000);

        if (!s.running) {
            break;
        }

        /* Snapshot fired jobs under the mutex, then execute outside it. */
        fired_t *fired = malloc(MAX_JOBS * sizeof(fired_t));
        if (!fired) {
            RTK_LOGE("cap_sched", "scheduler_task: OOM\n");
            continue;
        }
        int  fired_cnt  = 0;
        bool needs_save = false;

        rtos_mutex_take(s.mutex, 0xFFFFFFFFUL);

        uint32_t now = rtos_time_get_current_system_time_ms();

        for (int i = 0; i < s.job_count; i++) {
            sched_job_t *job = &s.jobs[i];

            if (!job->enabled) {
                continue;
            }

            if ((int32_t)(now - job->next_fire_ms) >= 0) {
                if (fired_cnt < MAX_JOBS) {
                    strncpy(fired[fired_cnt].evt, job->event_type,   sizeof(fired[0].evt) - 1);
                    fired[fired_cnt].evt[sizeof(fired[0].evt) - 1]       = '\0';
                    strncpy(fired[fired_cnt].id,  job->id,             sizeof(fired[0].id)  - 1);
                    fired[fired_cnt].id[sizeof(fired[0].id) - 1]         = '\0';
                    strncpy(fired[fired_cnt].pay, job->payload_json,   sizeof(fired[0].pay) - 1);
                    fired[fired_cnt].pay[sizeof(fired[0].pay) - 1]       = '\0';
                    strncpy(fired[fired_cnt].cap, job->cap_id,         sizeof(fired[0].cap) - 1);
                    fired[fired_cnt].cap[sizeof(fired[0].cap) - 1]       = '\0';
                    strncpy(fired[fired_cnt].cap_args, job->cap_args,  sizeof(fired[0].cap_args) - 1);
                    fired[fired_cnt].cap_args[sizeof(fired[0].cap_args) - 1] = '\0';
                    fired_cnt++;
                }

                if (job->interval_sec > 0) {
                    job->next_fire_ms = now + (uint32_t)((uint64_t)job->interval_sec * 1000u);
                    needs_save = true;
                } else if (job->event_type[0] == '\0') {
                    job->enabled = 0;   /* timer one-shot */
                    needs_save = true;
                }
                /* event-driven + interval=0 → no state change, no flash write needed */
            }
        }

        if (needs_save) save_jobs();
        rtos_mutex_give(s.mutex);

        execute_fired_jobs(fired, fired_cnt);
        free(fired);
    }

    rtos_task_delete(NULL);
}

/* ---- Static cap descriptors ---- */

static const claw_cap_descriptor_t s_caps[] = {
    {
        .id          = "scheduler_add_job",
        .name        = "scheduler_add_job",
        .family      = "scheduler",
        .description =
            "Add or update a scheduled job (upsert by id). "
            "Use cap_id+cap_args for direct capability calls, or event_type for event-driven triggers. "
            "\n\nTiming patterns:"
            "\n- Once after N sec: delay_sec=N, interval_sec=0 (timer one-shot)"
            "\n- Repeat every N sec: interval_sec=N"
            "\n- On WiFi ready (fires every boot): event_type='wifi_connected', interval_sec=0"
            "\n\ninterval_sec unified semantics:"
            "\n  timer job (no event_type): re-arm period; 0=one-shot"
            "\n  event job (has event_type): cooldown between same-event fires; 0=no cooldown (fire every time)"
            "\n\nIM reminder pattern:"
            "\n  cap_id = reply_cap from conversation context"
            "\n  cap_args = '{\"chat_id\":\"<chat_id>\",\"text\":\"<message>\"}'"
            "\n  delay_sec = <seconds>, interval_sec = 0",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"cap_id\":{\"type\":\"string\",\"description\":\"Capability to call directly on each fire (e.g. lua_run)\"},"
            "\"cap_args\":{\"type\":\"string\",\"description\":\"JSON args string passed to cap_id on each fire\"},"
            "\"event_type\":{\"type\":\"string\",\"description\":\"System event that triggers this job (e.g. 'wifi_connected'). Use instead of delay_sec when the job requires WiFi.\"},"
            "\"payload_json\":{\"type\":\"string\",\"description\":\"Payload for event_type\"},"
            "\"interval_sec\":{\"type\":\"number\",\"description\":\"Re-arm interval in seconds after firing (0 = one-shot, fires once then stops)\"},"
            "\"delay_sec\":{\"type\":\"number\",\"description\":\"Initial delay before first fire (seconds). "
              "Pattern — once after N sec: delay_sec=N interval_sec=0. "
              "Pattern — repeat every N sec: interval_sec=N. "
              "Do NOT use for WiFi wait — use event_type=wifi_connected instead.\"},"
            "\"id\":{\"type\":\"string\",\"description\":\"Job ID\"}"
            "}}",
        .execute = cap_add_job,
    },
    {
        .id          = "scheduler_list_jobs",
        .name        = "scheduler_list_jobs",
        .family      = "scheduler",
        .description = "List all scheduled jobs.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_list_jobs,
    },
    {
        .id          = "scheduler_remove_job",
        .name        = "scheduler_remove_job",
        .family      = "scheduler",
        .description = "Remove a scheduled job by ID.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"Job ID\"}},"
            "\"required\":[\"id\"]}",
        .execute = cap_remove_job,
    },
    {
        .id          = "scheduler_enable_job",
        .name        = "scheduler_enable_job",
        .family      = "scheduler",
        .description = "Enable a scheduled job by ID.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"Job ID\"}},"
            "\"required\":[\"id\"]}",
        .execute = cap_enable_job,
    },
    {
        .id          = "scheduler_disable_job",
        .name        = "scheduler_disable_job",
        .family      = "scheduler",
        .description = "Disable a scheduled job by ID.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"Job ID\"}},"
            "\"required\":[\"id\"]}",
        .execute = cap_disable_job,
    },
};

static const claw_cap_group_t s_group = {
    .group_id         = "scheduler",
    .plugin_name      = "cap_scheduler",
    .version          = "1",
    .descriptors      = s_caps,
    .descriptor_count = 5,
};

/* ---- Public API ---- */

int cap_scheduler_init(const cap_scheduler_config_t *config)
{
    _memset(&s, 0, sizeof(s));

    s.max_jobs = (config->max_jobs < MAX_JOBS) ? (int)config->max_jobs : MAX_JOBS;

    mkdir(config->schedule_root_dir, 0777);

    DiagSnPrintf(s.schedule_file, sizeof(s.schedule_file), "%s/schedules.json",
             config->schedule_root_dir);

    int err_mutex = rtos_mutex_create(&s.mutex);
    if (err_mutex != RTK_SUCCESS) {
        RTK_LOGE(TAG, "Failed to create mutex\n");
        return RTK_FAIL;
    }

    load_jobs();

    int err = claw_cap_register_group(&s_group);
    if (err != RTK_SUCCESS) {
        RTK_LOGE(TAG, "claw_cap_register_group failed: %d\n", err);
        return err;
    }

    RTK_LOGI(TAG, "Initialized (dir=%s, max_jobs=%d, loaded %d jobs)\n",
             config->schedule_root_dir, s.max_jobs, s.job_count);

    return RTK_SUCCESS;
}

int cap_scheduler_start(void)
{
    s.running = 1;

    int ret = rtos_task_create(&s.task, "sched_task", scheduler_task, NULL, 4096, 1);
    if (ret != RTK_SUCCESS) {
        RTK_LOGE(TAG, "Failed to create scheduler task\n");
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

/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * claw_memory_extract.c — post-conversation long-term memory extraction.
 *
 * The observer enqueues a copy of the conversation turn; a dedicated task
 * performs the LLM call asynchronously so that engine_task's stack is not
 * consumed by a second TLS handshake.
 */

#include "ameba_soc.h"
#include "claw_memory_extract.h"
#include "claw_memory.h"
#include "claw_memory_compact.h"
#include "claw_agent_llm.h"
#include "cJSON.h"
#include "basic_types.h"
#include "os_wrapper.h"
#include "FreeRTOS.h"
#include "queue.h"
#include <string.h>
#include <stdlib.h>

#define TAG "mem_extract"

#define EXTRACT_SYSTEM_PROMPT \
    "You are a memory extraction assistant for an embedded AI device. " \
    "Analyze the conversation turn below and decide if it contains any " \
    "durable user facts, preferences, or habits worth remembering long-term. " \
    "Return JSON only, no explanation:\n" \
    "{\"intent\":\"none|remember\",\"memories\":[" \
      "{\"content\":\"...\",\"summary\":\"...\",\"tags\":\"...\"}]}\n" \
    "Rules:\n" \
    "- intent=none if nothing durable to remember (casual chat, one-off questions)\n" \
    "- Only extract facts that remain true across sessions: name, preferences, habits, " \
      "important personal info\n" \
    "- Do NOT extract temporary states, task results, or questions\n" \
    "- content: one full fact sentence, under 100 chars\n" \
    "- summary: a 5–15 character label that will be shown in future prompts as an index entry " \
      "(e.g. \"用户偏好-饮食-不吃辣\", \"设备配置-WiFi-家里\")\n" \
    "- tags: comma-separated keywords, e.g. \"preference,food\""

static void do_extract(const char *session_id,
                       const char *user_text,
                       const char *assistant_text)
{
    char *prompt = NULL;
    size_t prompt_len;
    cJSON *messages = NULL;
    cJSON *msg = NULL;
    llm_resp_t resp = {0};
    char *err = NULL;
    int rc;

    prompt_len = strlen("User: ") + strlen(user_text) +
                 strlen("\nAssistant: ") + strlen(assistant_text) + 4;
    prompt = malloc(prompt_len);
    if (!prompt) {
        RTK_LOGE(TAG, "OOM building prompt\n");
        return;
    }
    DiagSnPrintf(prompt, prompt_len, "User: %s\nAssistant: %s", user_text, assistant_text);

    messages = cJSON_CreateArray();
    if (!messages) { free(prompt); return; }

    msg = cJSON_CreateObject();
    if (!msg) { cJSON_Delete(messages); free(prompt); return; }
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", prompt);
    cJSON_AddItemToArray(messages, msg);
    free(prompt);

    /* S2: serialize TLS handshakes between mem_extract and mem_compact. */
    claw_memory_summary_lock_take();
    rc = claw_agent_llm_chat_messages(EXTRACT_SYSTEM_PROMPT, messages, NULL, &resp, &err);
    claw_memory_summary_lock_give();
    cJSON_Delete(messages);

    if (rc != RTK_SUCCESS) {
        RTK_LOGW(TAG, "extract LLM call failed: %s\n", err ? err : "?");
        free(err);
        return;
    }

    if (!resp.reply || !resp.reply[0]) {
        claw_agent_llm_response_free(&resp);
        return;
    }

    /* LLM may wrap JSON in markdown fences — find first '{' to last '}'. */
    const char *json_start = strchr(resp.reply, '{');
    const char *json_end   = strrchr(resp.reply, '}');
    cJSON *root = NULL;
    if (json_start && json_end && json_end > json_start) {
        size_t json_len = (size_t)(json_end - json_start + 1);
        char *json_buf = malloc(json_len + 1);
        if (json_buf) {
            memcpy(json_buf, json_start, json_len);
            json_buf[json_len] = '\0';
            root = cJSON_Parse(json_buf);
            free(json_buf);
        }
    }
    claw_agent_llm_response_free(&resp);

    if (!root) {
        RTK_LOGW(TAG, "extract: bad JSON\n");
        return;
    }

    cJSON *intent = cJSON_GetObjectItem(root, "intent");
    if (!intent || !cJSON_IsString(intent) ||
            strcmp(intent->valuestring, "remember") != 0) {
        cJSON_Delete(root);
        return;
    }

    cJSON *mems = cJSON_GetObjectItem(root, "memories");
    if (!mems || !cJSON_IsArray(mems)) {
        cJSON_Delete(root);
        return;
    }

    cJSON *item;
    int stored = 0;
    cJSON_ArrayForEach(item, mems) {
        cJSON *content = cJSON_GetObjectItem(item, "content");
        cJSON *tags    = cJSON_GetObjectItem(item, "tags");
        cJSON *jsummary = cJSON_GetObjectItem(item, "summary");

        if (!content || !cJSON_IsString(content) || !content->valuestring[0]) {
            continue;
        }

        claw_memory_item_t m = {0};
        strncpy(m.source,  "auto",                  sizeof(m.source)  - 1);
        /* UTF-8 boundary-safe: a raw strncpy can split a multi-byte (CJK)
         * character at the byte limit, producing invalid UTF-8 that the LLM
         * backend later rejects ("Invalid UTF-8 middle byte"). */
        claw_memory_utf8_safe_copy_marked(m.content, sizeof(m.content),
                                          content->valuestring, sizeof(m.content) - 1);
        if (tags && cJSON_IsString(tags)) {
            claw_memory_utf8_safe_copy(m.tags, sizeof(m.tags),
                                       tags->valuestring, sizeof(m.tags) - 1);
        }
        if (jsummary && cJSON_IsString(jsummary) && jsummary->valuestring[0]) {
            claw_memory_utf8_safe_copy(m.summary, sizeof(m.summary),
                                       jsummary->valuestring, sizeof(m.summary) - 1);
        } else {
            /* Fallback when the model omits summary: take ≤40 bytes of
             * content but never split a UTF-8 multi-byte sequence (which
             * would feed garbage bytes into the LLM's recall keyword). */
            claw_memory_utf8_safe_copy(m.summary, sizeof(m.summary),
                                       m.content, 40);
        }

        if (claw_memory_store(&m) == RTK_SUCCESS) {
            RTK_LOGI(TAG, "stored: %.80s\n", m.content);
            stored++;
        }
    }

    if (stored > 0) {
        RTK_LOGI(TAG, "session=%s extracted %d item(s)\n", session_id ? session_id : "?", stored);
    }

    cJSON_Delete(root);
}

/* ---------- async dispatch ---------- */

typedef struct {
    char *session_id;
    char *user_text;
    char *final_text;
} extract_job_t;

/* Queue depth was 4 with timeout=0; under load (compact holding the
 * summary_lock for 5–10 s) we silently dropped extract jobs. Bumped to
 * 8 to match the dispatcher event_queue_len, and the enqueue uses a
 * short timeout so a transient spike absorbs into a brief stall on the
 * caller (engine_task) rather than discarding a memorable turn. */
#define EXTRACT_QUEUE_LEN     8
#define EXTRACT_STACK_SIZE    (12 * 1024)  /* peak 6.1 KB; 12 KB = 2x margin */
#define EXTRACT_ENQUEUE_WAIT  pdMS_TO_TICKS(200)

static QueueHandle_t s_queue;

static void extract_task(void *arg)
{
    (void)arg;
    extract_job_t job;
    while (1) {
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) == pdTRUE) {
            do_extract(job.session_id, job.user_text, job.final_text);
            free(job.session_id);
            free(job.user_text);
            free(job.final_text);
        }
    }
}

void claw_memory_extract_init(void)
{
    s_queue = xQueueCreate(EXTRACT_QUEUE_LEN, sizeof(extract_job_t));
    if (!s_queue) {
        RTK_LOGE(TAG, "queue create failed\n");
        return;
    }
    if (rtos_task_create(NULL, "mem_extract", extract_task, NULL,
                         EXTRACT_STACK_SIZE, 1) != RTK_SUCCESS) {
        RTK_LOGE(TAG, "task create failed\n");
    }
}

/* Cheap heuristic: does the user_text plausibly contain a durable personal
 * fact worth extracting? Pure technical questions ("ring buffer 是什么"),
 * one-off task requests ("帮我查天气"), and small-talk lack any first-person
 * marker; running an LLM round just to hear "intent=none" wastes 3 KB of
 * uplink + ~$0.0001 + summary_lock contention. Returns true when extraction
 * SHOULD run.
 *
 * The rule of thumb: the model can only extract durable facts from text
 * that the USER said about themselves. If neither the question nor the
 * answer mentions "me/I/my" in CN or EN, skipping is safe — and we keep
 * the explicit "记住"/"remember" trigger as an override.
 */
static bool extract_worth_running(const char *user_text,
                                  const char *assistant_text)
{
    if (!user_text) return false;

    static const char *const personal_markers[] = {
        /* Chinese first-person / fact-stating */
        "我", "我的", "我是", "我叫", "我家", "我们",
        /* Chinese explicit memory triggers */
        "记住", "请记", "记一下", "别忘",
        /* English first-person / fact-stating */
        "I ", " I ", "I'm", "I am", "I have", "my ", "My ", "me ",
        /* English explicit memory triggers */
        "remember", "Remember", "don't forget", "note that",
        NULL,
    };

    for (size_t i = 0; personal_markers[i]; i++) {
        if (strstr(user_text, personal_markers[i])) return true;
    }

    /* Sometimes the personal claim is in the assistant's confirmation
     * ("好的，我记住了你叫小明") even when the user typed only their name.
     * Scan up to a 256-byte prefix; copy to a stack buffer first so we
     * stay portable (newlib-nano lacks GNU memmem/strnstr). */
    if (assistant_text && assistant_text[0]) {
        char probe[257];
        size_t n = strlen(assistant_text);
        if (n > sizeof(probe) - 1) n = sizeof(probe) - 1;
        memcpy(probe, assistant_text, n);
        probe[n] = '\0';
        for (size_t i = 0; personal_markers[i]; i++) {
            if (strstr(probe, personal_markers[i])) return true;
        }
    }
    return false;
}

void claw_memory_extract_observer(const claw_agent_completion_summary_t *summary,
                                  void *user_ctx)
{
    (void)user_ctx;

    if (!summary) return;
    if (!summary->user_text  || !summary->user_text[0])  return;
    if (!summary->final_text || !summary->final_text[0]) return;

    /* Skip when the LLM already wrote to long-term memory this turn —
     * the auto-extract path would otherwise duplicate similar facts. */
    if (summary->tool_calls_csv &&
        (strstr(summary->tool_calls_csv, "memory_store") ||
         strstr(summary->tool_calls_csv, "memory_update"))) {
        return;
    }

    /* Pre-filter: skip turns with no first-person markers. Saves an LLM call
     * for every "X 是什么 / 怎么 / 帮我 …" — empirically the majority. */
    if (!extract_worth_running(summary->user_text, summary->final_text)) {
        RTK_LOGD(TAG, "skip: no personal markers\n");
        return;
    }

    if (!s_queue) return;

    extract_job_t job = {
        .session_id = summary->session_id ? strdup(summary->session_id) : strdup(""),
        .user_text  = strdup(summary->user_text),
        .final_text = strdup(summary->final_text),
    };
    if (!job.session_id || !job.user_text || !job.final_text) {
        free(job.session_id);
        free(job.user_text);
        free(job.final_text);
        RTK_LOGE(TAG, "OOM enqueuing job\n");
        return;
    }

    if (xQueueSend(s_queue, &job, EXTRACT_ENQUEUE_WAIT) != pdTRUE) {
        RTK_LOGW(TAG, "extract queue full after 200ms, dropping\n");
        free(job.session_id);
        free(job.user_text);
        free(job.final_text);
    }
}

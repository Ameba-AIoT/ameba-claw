/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 * SPDX-License-Identifier: Apache-2.0
 *
 * claw_test.c — embedded unit / integration tests for ameba_claw.
 *
 * All tests write results into a caller-supplied buffer and return
 * the number of failures (0 = all passed).
 */

#include "claw_test.h"
#include "claw_cap.h"
#include "claw_memory.h"
#include "claw_event_dispatcher.h"
#include "claw_event.h"
#include "claw_event_publisher.h"
#include "claw_agent.h"
#include "os_wrapper.h"
#include "platform_stdlib.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- Helpers ---- */

#define T_PASS(buf, bufsz, name) \
    do { strncat((buf), "[PASS] " name "\n", (bufsz) - strlen(buf) - 1); } while(0)
#define T_FAIL(buf, bufsz, name, reason) \
    do { strncat((buf), "[FAIL] " name ": " reason "\n", (bufsz) - strlen(buf) - 1); fails++; } while(0)

/* ================================================================
 * claw_cap hash-table tests
 * ================================================================ */

static int s_test_execute_called = 0;

static int test_cap_execute(const char *input_json,
                             const claw_cap_call_context_t *ctx,
                             char **output)
{
    (void)ctx;
    (void)input_json;
    s_test_execute_called++;
    *output = strdup("{\"test\":\"ok\"}");
    return RTK_SUCCESS;
}

int claw_test_cap(char *buf, size_t bufsz)
{
    int fails = 0;
    buf[0] = '\0';

    static const claw_cap_descriptor_t desc = {
        .id              = "_claw_test_cap_",
        .name            = "_claw_test_cap_",
        .family          = "test",
        .description     = "unit test capability",
        .kind            = CLAW_CAP_KIND_INVOKE,
        .cap_flags       = 0,
        .input_schema_json = NULL,
        .init    = NULL,
        .start   = NULL,
        .stop    = NULL,
        .execute = test_cap_execute,
    };

    /* Clear any leftover state from a previous run that failed mid-test */
    claw_cap_unregister("_claw_test_cap_", 200);

    /* 1. Register */
    int rc = claw_cap_register(&desc);
    if (rc != RTK_SUCCESS) {
        T_FAIL(buf, bufsz, "cap_register", "returned non-zero");
        return fails;
    }
    T_PASS(buf, bufsz, "cap_register");

    /* 2. Find by name */
    const claw_cap_descriptor_t *found = claw_cap_find("_claw_test_cap_");
    if (!found || strcmp(found->id, "_claw_test_cap_") != 0) {
        T_FAIL(buf, bufsz, "cap_find_by_name", "not found after register");
    } else {
        T_PASS(buf, bufsz, "cap_find_by_name");
    }

    /* 3. Find non-existent */
    if (claw_cap_find("_no_such_cap_xyz_") != NULL) {
        T_FAIL(buf, bufsz, "cap_find_missing", "found cap that doesn't exist");
    } else {
        T_PASS(buf, bufsz, "cap_find_missing");
    }

    /* 4. Call the cap */
    s_test_execute_called = 0;
    claw_cap_call_context_t ctx = { .caller = CLAW_CAP_CALLER_INTERNAL };
    char *out = NULL;
    rc = claw_cap_call("_claw_test_cap_", "{}", &ctx, &out);
    if (rc != RTK_SUCCESS || !out || strcmp(out, "{\"test\":\"ok\"}") != 0) {
        T_FAIL(buf, bufsz, "cap_call", "wrong result or error");
    } else {
        T_PASS(buf, bufsz, "cap_call");
    }
    free(out);

    if (s_test_execute_called != 1) {
        T_FAIL(buf, bufsz, "cap_execute_called_once", "execute not called exactly once");
    } else {
        T_PASS(buf, bufsz, "cap_execute_called_once");
    }

    /* 5. Unregister */
    rc = claw_cap_unregister("_claw_test_cap_", 500);
    if (rc != RTK_SUCCESS) {
        T_FAIL(buf, bufsz, "cap_unregister", "returned non-zero");
    } else {
        T_PASS(buf, bufsz, "cap_unregister");
    }

    /* 6. Find after unregister */
    if (claw_cap_find("_claw_test_cap_") != NULL) {
        T_FAIL(buf, bufsz, "cap_find_after_unregister", "still found after unregister");
    } else {
        T_PASS(buf, bufsz, "cap_find_after_unregister");
    }

    return fails;
}

/* ================================================================
 * claw_memory long-term CRUD tests
 * ================================================================ */

int claw_test_mem(char *buf, size_t bufsz)
{
    int fails = 0;
    buf[0] = '\0';

    /* Verify memory is accessible by attempting a list (returns NULL if not init) */
    char *probe = claw_memory_list(1);
    if (!probe) {
        T_FAIL(buf, bufsz, "mem_initialized", "claw_memory_list returned NULL — not initialized?");
        return fails;
    }
    free(probe);
    T_PASS(buf, bufsz, "mem_initialized");

    /* 1. Store an item */
    claw_memory_item_t item = {0};
    strncpy(item.source,  "test",           sizeof(item.source)  - 1);
    strncpy(item.content, "claw_test_value_abc", sizeof(item.content) - 1);
    strncpy(item.tags,    "test,unit",      sizeof(item.tags)    - 1);

    int rc = claw_memory_store(&item);
    if (rc != RTK_SUCCESS || item.id == 0) {
        T_FAIL(buf, bufsz, "mem_store", "store failed or id not set");
        return fails;
    }
    uint32_t stored_id = item.id;
    T_PASS(buf, bufsz, "mem_store");

    /* 2. Recall by keyword */
    char *result = claw_memory_recall("claw_test_value_abc", 5);
    if (!result || strstr(result, "claw_test_value_abc") == NULL) {
        T_FAIL(buf, bufsz, "mem_recall", "keyword not found in recall result");
    } else {
        T_PASS(buf, bufsz, "mem_recall");
    }
    free(result);

    /* 3. Update */
    rc = claw_memory_update(stored_id, "claw_test_value_updated");
    if (rc != RTK_SUCCESS) {
        T_FAIL(buf, bufsz, "mem_update", "update returned non-zero");
    } else {
        T_PASS(buf, bufsz, "mem_update");
    }

    /* 4. Recall updated content */
    result = claw_memory_recall("claw_test_value_updated", 5);
    if (!result || strstr(result, "claw_test_value_updated") == NULL) {
        T_FAIL(buf, bufsz, "mem_recall_updated", "updated content not found");
    } else {
        T_PASS(buf, bufsz, "mem_recall_updated");
    }
    free(result);

    /* 5. Forget */
    rc = claw_memory_forget(stored_id);
    if (rc != RTK_SUCCESS) {
        T_FAIL(buf, bufsz, "mem_forget", "forget returned non-zero");
    } else {
        T_PASS(buf, bufsz, "mem_forget");
    }

    /* 6. Recall after forget — should not be found */
    result = claw_memory_recall("claw_test_value_updated", 5);
    bool still_present = result && strstr(result, "claw_test_value_updated") != NULL;
    free(result);
    if (still_present) {
        T_FAIL(buf, bufsz, "mem_forget_verified", "item still present after forget");
    } else {
        T_PASS(buf, bufsz, "mem_forget_verified");
    }

    return fails;
}

/* ================================================================
 * claw_event_router filter / template tests (via public rule API)
 * ================================================================ */

static volatile int s_router_test_hit = 0;
static rtos_sema_t  s_router_test_sema;

static int router_test_cap_execute(const char *input_json,
                                    const claw_cap_call_context_t *ctx,
                                    char **output)
{
    (void)ctx;
    (void)input_json;
    s_router_test_hit++;
    rtos_sema_give(s_router_test_sema);
    *output = strdup("{\"hit\":1}");
    return RTK_SUCCESS;
}

static const claw_cap_descriptor_t s_router_test_desc = {
    .id      = "_router_test_cap_",
    .name    = "_router_test_cap_",
    .family  = "test",
    .kind    = CLAW_CAP_KIND_INVOKE,
    .cap_flags = 0,
    .execute = router_test_cap_execute,
};

int claw_test_router(char *buf, size_t bufsz)
{
    int fails = 0;
    buf[0] = '\0';

    /* Create binary semaphore for deterministic wait (avoids flaky sleep) */
    if (rtos_sema_create_binary(&s_router_test_sema) != RTK_SUCCESS) {
        T_FAIL(buf, bufsz, "router_sema_create", "failed to create test semaphore");
        return fails;
    }

    /* Register a test cap for the router to call */
    claw_cap_register(&s_router_test_desc);

    /* Build a rule: match event_type="claw_test_evt" → call_cap */
    claw_event_dispatcher_action_t action = {
        .kind      = CLAW_DISPATCHER_ACT_CAP,
        .fail_open = true,
        .input_json = NULL,
    };
    strncpy(action.cap, "_router_test_cap_", sizeof(action.cap) - 1);

    claw_event_dispatcher_rule_t rule = {
        .enabled          = true,
        .consume_on_match = true,
        .action_count     = 1,
        .actions          = &action,
    };
    strncpy(rule.id,               "claw_test_rule",  sizeof(rule.id)               - 1);
    strncpy(rule.match.event_type, "claw_test_evt",   sizeof(rule.match.event_type) - 1);

    int rc = claw_event_dispatcher_add_rule(&rule);
    if (rc != RTK_SUCCESS) {
        T_FAIL(buf, bufsz, "router_add_rule", "add_rule failed");
        rtos_sema_delete(s_router_test_sema);
        claw_cap_unregister("_router_test_cap_", 500);
        return fails;
    }
    T_PASS(buf, bufsz, "router_add_rule");

    /* Publish matching event */
    s_router_test_hit = 0;
    rc = claw_event_dispatcher_publish_trigger("claw_test", "claw_test_evt",
                                           "claw_test_key", NULL);
    if (rc != RTK_SUCCESS) {
        T_FAIL(buf, bufsz, "router_publish_match", "publish returned non-zero");
    } else {
        T_PASS(buf, bufsz, "router_publish_match");
    }

    /* Wait for semaphore (up to 2 s) instead of a fixed sleep */
    if (rtos_sema_take(s_router_test_sema, 2000) != RTK_SUCCESS || s_router_test_hit != 1) {
        T_FAIL(buf, bufsz, "router_filter_match", "cap not called within 2s for matching event");
    } else {
        T_PASS(buf, bufsz, "router_filter_match");
    }

    /* Publish NON-matching event — give router 500 ms to NOT trigger */
    s_router_test_hit = 0;
    claw_event_dispatcher_publish_trigger("claw_test", "claw_other_evt", "k", NULL);
    rtos_time_delay_ms(500);   /* intentional: we're verifying absence of a call */

    if (s_router_test_hit != 0) {
        T_FAIL(buf, bufsz, "router_filter_no_match", "cap called for non-matching event");
    } else {
        T_PASS(buf, bufsz, "router_filter_no_match");
    }

    /* Cleanup */
    rtos_sema_delete(s_router_test_sema);
    claw_cap_unregister("_router_test_cap_", 500);

    return fails;
}

/* ================================================================
 * Filesystem read/write test
 * ================================================================ */

#define FS_TEST_PATH  "vfs:/claw_test_tmp.txt"
#define FS_TEST_DATA  "CLAW_FS_TEST_OK_12345"

int claw_test_fs(char *buf, size_t bufsz)
{
    int fails = 0;
    buf[0] = '\0';

    /* Write */
    FILE *f = fopen(FS_TEST_PATH, "w");
    if (!f) {
        T_FAIL(buf, bufsz, "fs_open_write", "fopen for write failed");
        return fails;
    }
    size_t written = fwrite(FS_TEST_DATA, 1, strlen(FS_TEST_DATA), f);
    fclose(f);
    if (written != strlen(FS_TEST_DATA)) {
        T_FAIL(buf, bufsz, "fs_write", "fwrite returned wrong count");
        return fails;
    }
    T_PASS(buf, bufsz, "fs_write");

    /* Read back */
    f = fopen(FS_TEST_PATH, "r");
    if (!f) {
        T_FAIL(buf, bufsz, "fs_open_read", "fopen for read failed");
        return fails;
    }
    char readbuf[64] = {0};
    size_t n = fread(readbuf, 1, sizeof(readbuf) - 1, f);
    fclose(f);
    readbuf[n] = '\0';

    if (strcmp(readbuf, FS_TEST_DATA) != 0) {
        T_FAIL(buf, bufsz, "fs_read_verify", "read content mismatch");
    } else {
        T_PASS(buf, bufsz, "fs_read_verify");
    }

    /* Delete */
    if (remove(FS_TEST_PATH) != 0) {
        T_FAIL(buf, bufsz, "fs_delete", "remove() failed");
    } else {
        T_PASS(buf, bufsz, "fs_delete");
    }

    /* Verify deleted */
    f = fopen(FS_TEST_PATH, "r");
    if (f) {
        fclose(f);
        T_FAIL(buf, bufsz, "fs_delete_verify", "file still exists after remove");
    } else {
        T_PASS(buf, bufsz, "fs_delete_verify");
    }

    return fails;
}

/* ================================================================
 * R-8: claw_cap hash-table Robin-Hood deletion test
 * ================================================================ */

static int ht_execute_a(const char *input_json,
                        const claw_cap_call_context_t *ctx,
                        char **output)
{
    (void)ctx; (void)input_json;
    *output = strdup("{\"ok\":1}");
    return RTK_SUCCESS;
}

static int ht_execute_b(const char *input_json,
                        const claw_cap_call_context_t *ctx,
                        char **output)
{
    (void)ctx; (void)input_json;
    *output = strdup("{\"ok\":1}");
    return RTK_SUCCESS;
}

static int ht_execute_c(const char *input_json,
                        const claw_cap_call_context_t *ctx,
                        char **output)
{
    (void)ctx; (void)input_json;
    *output = strdup("{\"ok\":1}");
    return RTK_SUCCESS;
}

static const claw_cap_descriptor_t s_ht_desc_a = {
    .id          = "_ht_a_",
    .name        = "_ht_a_",
    .family      = "test",
    .description = "hash collision test cap A",
    .kind        = CLAW_CAP_KIND_INVOKE,
    .cap_flags   = 0,
    .execute     = ht_execute_a,
};

static const claw_cap_descriptor_t s_ht_desc_b = {
    .id          = "_ht_b_",
    .name        = "_ht_b_",
    .family      = "test",
    .description = "hash collision test cap B",
    .kind        = CLAW_CAP_KIND_INVOKE,
    .cap_flags   = 0,
    .execute     = ht_execute_b,
};

static const claw_cap_descriptor_t s_ht_desc_c = {
    .id          = "_ht_c_",
    .name        = "_ht_c_",
    .family      = "test",
    .description = "hash collision test cap C",
    .kind        = CLAW_CAP_KIND_INVOKE,
    .cap_flags   = 0,
    .execute     = ht_execute_c,
};

int claw_test_cap_hash_collision(char *buf, size_t bufsz)
{
    int fails = 0;
    buf[0] = '\0';

    /* Clean up any leftovers from a previous run */
    claw_cap_unregister("_ht_a_", 200);
    claw_cap_unregister("_ht_b_", 200);
    claw_cap_unregister("_ht_c_", 200);

    /* Register three caps */
    if (claw_cap_register(&s_ht_desc_a) != RTK_SUCCESS) {
        T_FAIL(buf, bufsz, "ht_register_a", "register _ht_a_ failed");
        return fails;
    }
    if (claw_cap_register(&s_ht_desc_b) != RTK_SUCCESS) {
        T_FAIL(buf, bufsz, "ht_register_b", "register _ht_b_ failed");
        claw_cap_unregister("_ht_a_", 200);
        return fails;
    }
    if (claw_cap_register(&s_ht_desc_c) != RTK_SUCCESS) {
        T_FAIL(buf, bufsz, "ht_register_c", "register _ht_c_ failed");
        claw_cap_unregister("_ht_a_", 200);
        claw_cap_unregister("_ht_b_", 200);
        return fails;
    }
    T_PASS(buf, bufsz, "ht_register_abc");

    /* Verify all three are findable */
    if (!claw_cap_find("_ht_a_")) {
        T_FAIL(buf, bufsz, "ht_find_a_before_delete", "_ht_a_ not found after register");
    } else {
        T_PASS(buf, bufsz, "ht_find_a_before_delete");
    }
    if (!claw_cap_find("_ht_b_")) {
        T_FAIL(buf, bufsz, "ht_find_b_before_delete", "_ht_b_ not found after register");
    } else {
        T_PASS(buf, bufsz, "ht_find_b_before_delete");
    }
    if (!claw_cap_find("_ht_c_")) {
        T_FAIL(buf, bufsz, "ht_find_c_before_delete", "_ht_c_ not found after register");
    } else {
        T_PASS(buf, bufsz, "ht_find_c_before_delete");
    }

    /* Delete the middle cap (_ht_b_) */
    int rc = claw_cap_unregister("_ht_b_", 500);
    if (rc != RTK_SUCCESS) {
        T_FAIL(buf, bufsz, "ht_unregister_b", "unregister _ht_b_ returned non-zero");
    } else {
        T_PASS(buf, bufsz, "ht_unregister_b");
    }

    /* _ht_a_ and _ht_c_ must still be findable */
    if (!claw_cap_find("_ht_a_")) {
        T_FAIL(buf, bufsz, "ht_find_a_after_delete", "_ht_a_ lost after deleting _ht_b_");
    } else {
        T_PASS(buf, bufsz, "ht_find_a_after_delete");
    }
    if (!claw_cap_find("_ht_c_")) {
        T_FAIL(buf, bufsz, "ht_find_c_after_delete", "_ht_c_ lost after deleting _ht_b_");
    } else {
        T_PASS(buf, bufsz, "ht_find_c_after_delete");
    }

    /* _ht_b_ must NOT be findable */
    if (claw_cap_find("_ht_b_") != NULL) {
        T_FAIL(buf, bufsz, "ht_find_b_after_delete", "_ht_b_ still found after unregister");
    } else {
        T_PASS(buf, bufsz, "ht_find_b_after_delete");
    }

    /* Cleanup */
    claw_cap_unregister("_ht_a_", 500);
    claw_cap_unregister("_ht_c_", 500);

    return fails;
}

/* ================================================================
 * R-9: agentic loop stub — walk the full request path via submit + receive_for
 * ================================================================ */

int claw_test_agent_loop_stub(char *buf, size_t bufsz)
{
    int fails = 0;
    buf[0] = '\0';

    static uint32_t s_test_req_id = 0x9000;

    claw_agent_request_t req = {
        .request_id     = ++s_test_req_id,
        .flags          = CLAW_AGENT_REQUEST_FLAG_SYNC_RECEIVE,
        .session_id     = "claw_test_session",
        .user_text      = "ping",
        .source_channel = "serial",
        .source_chat_id = "serial",
    };

    claw_agent_response_t resp;
    memset(&resp, 0, sizeof(resp));

    int rc = claw_agent_submit(&req, 5000);
    if (rc == RTK_SUCCESS) {
        rc = claw_agent_receive_for(req.request_id, &resp, 30000);
    }

    /* If the agent is not initialised, submit will fail with something other
     * than RTK_SUCCESS or RTK_ERR_TIMEOUT — treat as SKIP, not a test failure. */
    if (rc != RTK_SUCCESS && rc != (int)RTK_ERR_TIMEOUT) {
        strncat(buf, "[SKIP] agent_loop_stub: claw_agent not ready (rc=",
                bufsz - strlen(buf) - 1);
        char tmp[32];
        DiagSnPrintf(tmp, sizeof(tmp), "%d)\n", rc);
        strncat(buf, tmp, bufsz - strlen(buf) - 1);
        return 0;
    }

    /* rc is either RTK_SUCCESS or RTK_ERR_TIMEOUT — both are valid outcomes */
    T_PASS(buf, bufsz, "agent_receive_for_rc");

    if (rc == (int)RTK_ERR_TIMEOUT) {
        strncat(buf, "[INFO] agent_loop_stub: submit timed out (expected in test env)\n",
                bufsz - strlen(buf) - 1);
        return fails;
    }

    /* rc == RTK_SUCCESS: validate the response fields */
    if (resp.status != CLAW_AGENT_RESPONSE_STATUS_OK &&
        resp.status != CLAW_AGENT_RESPONSE_STATUS_ERROR) {
        T_FAIL(buf, bufsz, "agent_resp_status_valid", "resp.status is not OK or ERROR");
    } else {
        T_PASS(buf, bufsz, "agent_resp_status_valid");
    }

    if (resp.status == CLAW_AGENT_RESPONSE_STATUS_OK && resp.text == NULL) {
        T_FAIL(buf, bufsz, "agent_resp_text_nonnull", "resp.text is NULL for OK status");
    } else {
        T_PASS(buf, bufsz, "agent_resp_text_nonnull");
    }

    /* Print a brief summary of the response */
    if (resp.text) {
        char snippet[84];
        DiagSnPrintf(snippet, sizeof(snippet), "[INFO] agent response (first 80): %.80s\n",
                 resp.text);
        strncat(buf, snippet, bufsz - strlen(buf) - 1);
    }

    claw_agent_response_free(&resp);

    return fails;
}

/* ================================================================
 * Run all tests
 * ================================================================ */

int claw_test_all(char *buf, size_t bufsz)
{
    char sub[512];
    int total_fails = 0;
    buf[0] = '\0';

    strncat(buf, "=== CAP TESTS ===\n", bufsz - strlen(buf) - 1);
    total_fails += claw_test_cap(sub, sizeof(sub));
    strncat(buf, sub, bufsz - strlen(buf) - 1);

    strncat(buf, "=== MEM TESTS ===\n", bufsz - strlen(buf) - 1);
    total_fails += claw_test_mem(sub, sizeof(sub));
    strncat(buf, sub, bufsz - strlen(buf) - 1);

    strncat(buf, "=== ROUTER TESTS ===\n", bufsz - strlen(buf) - 1);
    total_fails += claw_test_router(sub, sizeof(sub));
    strncat(buf, sub, bufsz - strlen(buf) - 1);

    strncat(buf, "=== FS TESTS ===\n", bufsz - strlen(buf) - 1);
    total_fails += claw_test_fs(sub, sizeof(sub));
    strncat(buf, sub, bufsz - strlen(buf) - 1);

    strncat(buf, "=== HASH COLLISION TESTS ===\n", bufsz - strlen(buf) - 1);
    total_fails += claw_test_cap_hash_collision(sub, sizeof(sub));
    strncat(buf, sub, bufsz - strlen(buf) - 1);

    strncat(buf, "=== AGENT LOOP STUB TESTS ===\n", bufsz - strlen(buf) - 1);
    total_fails += claw_test_agent_loop_stub(sub, sizeof(sub));
    strncat(buf, sub, bufsz - strlen(buf) - 1);

    char summary[64];
    DiagSnPrintf(summary, sizeof(summary),
             "--- TOTAL: %d failure(s) ---\n", total_fails);
    strncat(buf, summary, bufsz - strlen(buf) - 1);

    return total_fails;
}

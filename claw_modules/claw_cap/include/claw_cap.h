/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "claw_compat.h"
#include "ameba_soc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLAW_CAP_KIND_INVOKE = 0,
    CLAW_CAP_KIND_EMITTER = 1,
    CLAW_CAP_KIND_BOTH = 2,
} claw_cap_kind_t;

typedef enum {
    CLAW_CAP_CALLER_INTERNAL = 0,
    CLAW_CAP_CALLER_LLM = 1,
    CLAW_CAP_CALLER_MANUAL = 2,
} claw_cap_caller_t;

typedef enum {
    CLAW_CAP_FLAG_LLM_ACCESS    = 1 << 0,
    CLAW_CAP_FLAG_EVENTS_OUT       = 1 << 1,
    CLAW_CAP_FLAG_MANAGED = 1 << 2,
    CLAW_CAP_FLAG_GUARDED         = 1 << 3,
} claw_cap_flags_t;

typedef enum {
    CLAW_CAP_STATE_LOADED = 0,
    CLAW_CAP_STATE_ACTIVE    = 1,
    CLAW_CAP_STATE_INACTIVE   = 2,
    CLAW_CAP_STATE_STOPPING   = 3,
    CLAW_CAP_STATE_REMOVING  = 4,
} claw_cap_state_t;

typedef struct {
    uint32_t    request_id;
    const char *session_id;
    const char *channel;
    const char *chat_id;
    const char *source_cap;
    const char *correlation_id;
    claw_cap_caller_t caller;
} claw_cap_call_context_t;

typedef enum {
    CLAW_CAP_EVENT_ROUTE_PASS     = 0,
    CLAW_CAP_EVENT_ROUTE_CONSUMED = 1,
    CLAW_CAP_EVENT_ROUTE_ERROR    = 2,
} claw_cap_event_route_t;

typedef int (*claw_cap_lifecycle_fn)(void);
/*
 * claw_cap_execute_fn — capability execute callback.
 *
 * On success (RTK_SUCCESS) or failure, the capability must malloc a result
 * string and assign it to *output.  The caller (claw_cap_call) takes
 * ownership and must free() the returned pointer.
 *
 * If the capability cannot allocate memory it should leave *output NULL
 * and return RTK_ERR_NOMEM.
 */
typedef int (*claw_cap_execute_fn)(const char *input_json,
                                         const claw_cap_call_context_t *ctx,
                                         char **output);

typedef struct {
    const char *id;
    const char *name;
    const char *family;
    const char *description;
    claw_cap_kind_t kind;
    uint32_t cap_flags;
    const char *input_schema_json;
    claw_cap_lifecycle_fn init;
    claw_cap_lifecycle_fn start;
    claw_cap_lifecycle_fn stop;
    claw_cap_execute_fn execute;
} claw_cap_descriptor_t;

typedef struct {
    const claw_cap_descriptor_t *items;
    size_t count;
} claw_cap_list_t;

typedef struct {
    const char *group_id;
    const char *plugin_name;
    const char *version;
    const claw_cap_descriptor_t *descriptors;
    size_t descriptor_count;
    void *plugin_ctx;
    claw_cap_lifecycle_fn group_init;
    claw_cap_lifecycle_fn group_start;
    claw_cap_lifecycle_fn group_stop;
} claw_cap_group_t;

typedef struct {
    const char *group_id;
    const char *plugin_name;
    const char *version;
    claw_cap_state_t state;
    size_t descriptor_count;
} claw_cap_group_info_t;

typedef struct {
    const claw_cap_group_info_t *items;
    size_t count;
} claw_cap_group_list_t;

typedef struct {
    const char *id;
    const char *name;
    const char *group_id;
    claw_cap_state_t state;
    uint32_t active_calls;
} claw_cap_descriptor_info_t;

int claw_cap_init(void);
int claw_cap_register(const claw_cap_descriptor_t *descriptor);
int claw_cap_register_group(const claw_cap_group_t *group);
int claw_cap_start_all(void);
int claw_cap_stop_all(void);
int claw_cap_enable_group(const char *group_id);
int claw_cap_disable_group(const char *group_id);
int claw_cap_unregister_group(const char *group_id, uint32_t timeout_ms);
int claw_cap_unregister(const char *id_or_name, uint32_t timeout_ms);
int claw_cap_set_llm_visible_groups(const char *const *group_ids, size_t count);
/* Hide every group from the LLM (count=0 in set_llm_visible_groups means
 * "show all"; use this when you explicitly want none visible). */
void claw_cap_hide_all_groups(void);
int claw_cap_set_session_llm_visible_groups(const char *session_id,
                                                  const char *const *group_ids,
                                                  size_t count);
/* Drop EVERY per-session visibility scope, so all sessions fall back to the
 * global base visibility. Used when clearing all sessions at once (skill
 * deactivation on session,clear,all) — resetting each session's scope by id
 * would require enumerating live sids, which nothing tracks. Returns
 * RTK_SUCCESS, or RTK_FAIL if the registry is not initialized. */
int claw_cap_clear_all_session_visible_groups(void);
bool claw_cap_group_exists(const char *group_id);
int claw_cap_get_group_state(const char *group_id, claw_cap_state_t *state);
int claw_cap_get_descriptor_state(const char *id_or_name,
                                        claw_cap_descriptor_info_t *info);
const claw_cap_descriptor_t *claw_cap_find(const char *id_or_name);
claw_cap_list_t claw_cap_list(void);
claw_cap_group_list_t claw_cap_list_groups(void);
/*
 * claw_cap_call — invoke a registered capability by id or name.
 *
 * On return, *output points to a malloc'd string owned by the caller; the
 * caller must free() it.  *output may be NULL if the capability returns
 * RTK_ERR_NOMEM or if no capability matching id_or_name is found and the
 * internal strdup also fails.  Always check *output before using it.
 */
int claw_cap_call(const char *id_or_name,
                        const char *input_json,
                        const claw_cap_call_context_t *ctx,
                        char **output);
char *claw_cap_build_llm_tools_json(const claw_cap_call_context_t *ctx,
                                    bool wrap_for_responses_api);
char *claw_cap_build_catalog(void);
/* Returns the count of LLM-accessible tools in group_id.
 * Fills out_names[0..min(count,max)-1] with tool name pointers (no copy, valid
 * while the group is registered). Passing out_names=NULL just counts. */
size_t claw_cap_list_group_tools(const char *group_id,
                                  const char **out_names, size_t max);
const char *claw_cap_state_to_string(claw_cap_state_t state);

/*
 * claw_cap_set_output — helper for capability execute() implementations.
 *
 * Allocates a heap buffer large enough for the formatted string and assigns
 * it to *output (caller of claw_cap_call() takes ownership and frees it).
 *
 * Returns RTK_SUCCESS on success, RTK_ERR_NOMEM if the allocation fails
 * (in which case *output is left NULL).
 */
static inline int claw_cap_set_output(char **output, const char *fmt, ...)
{
    va_list ap, ap2;
    int     n;
    char   *buf;

    if (!output) {
        return RTK_ERR_BADARG;
    }
    *output = NULL;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    /* DiagVSNprintf(NULL, 0, ...) returns the formatted length (fixed at SDK
     * level); n < 0 means a real format/arg error, not a platform limitation. */
    n = DiagVSNprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n <= 0) {
        va_end(ap2);
        return RTK_ERR_NOMEM;
    }
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) {
        va_end(ap2);
        return RTK_ERR_NOMEM;
    }
    DiagVSNprintf(buf, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    *output = buf;
    return RTK_SUCCESS;
}

#ifdef __cplusplus
}
#endif

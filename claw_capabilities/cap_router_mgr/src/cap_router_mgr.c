/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cap_router_mgr.h"
#include "claw_cap.h"
#include "claw_event_dispatcher.h"
#include <cJSON.h>
#include "platform_stdlib.h"
#include "os_wrapper.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TAG "cap_router_mgr"

/* ---- Runtime state ---- */

static struct {
    char rules_file[128];
} s_rt;

/* ---- Helper: action kind string → enum ---- */

/*
 * RTK-specific serialization strings for action kinds.
 * Intentionally different from any other SDK's string conventions.
 */
static claw_event_dispatcher_action_kind_t action_kind_from_str(const char *s)
{
    if (!s) return CLAW_DISPATCHER_ACT_AGENT;
    if (strcmp(s, "rtk_agent")  == 0) return CLAW_DISPATCHER_ACT_AGENT;
    if (strcmp(s, "rtk_cap")    == 0) return CLAW_DISPATCHER_ACT_CAP;
    if (strcmp(s, "rtk_script") == 0) return CLAW_DISPATCHER_ACT_SCRIPT;
    if (strcmp(s, "rtk_send")   == 0) return CLAW_DISPATCHER_ACT_SEND;
    if (strcmp(s, "rtk_emit")   == 0) return CLAW_DISPATCHER_ACT_EMIT;
    if (strcmp(s, "rtk_drop")   == 0) return CLAW_DISPATCHER_ACT_DROP;
    return CLAW_DISPATCHER_ACT_AGENT;
}

static const char *action_kind_to_str(claw_event_dispatcher_action_kind_t kind)
{
    switch (kind) {
    case CLAW_DISPATCHER_ACT_AGENT:    return "rtk_agent";
    case CLAW_DISPATCHER_ACT_CAP:     return "rtk_cap";
    case CLAW_DISPATCHER_ACT_SCRIPT:   return "rtk_script";
    case CLAW_DISPATCHER_ACT_SEND: return "rtk_send";
    case CLAW_DISPATCHER_ACT_EMIT:   return "rtk_emit";
    case CLAW_DISPATCHER_ACT_DROP:         return "rtk_drop";
    default:                                    return "rtk_agent";
    }
}

/* ---- Helper: read entire file into heap buffer (caller frees) ---- */

static char *read_file_alloc(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    if (sz <= 0) {
        fclose(f);
        return NULL;
    }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

/* ---- Helper: write cJSON array to rules_file ---- */

static int write_rules_file(cJSON *jarr)
{
    char *s = cJSON_PrintUnformatted(jarr);
    if (!s) return RTK_ERR_NOMEM;

    FILE *f = fopen(s_rt.rules_file, "w");
    if (!f) {
        free(s);
        return RTK_FAIL;
    }
    fputs(s, f);
    fclose(f);
    free(s);
    return RTK_SUCCESS;
}

/* ---- Helper: build claw_event_dispatcher_rule_t from JSON object ---- */

static int rule_from_json(const cJSON *jrule,
                                claw_event_dispatcher_rule_t *rule,
                                claw_event_dispatcher_action_t **actions_out,
                                size_t *action_count_out)
{
    _memset(rule, 0, sizeof(*rule));
    *actions_out     = NULL;
    *action_count_out = 0;

    /* id */
    cJSON *jid = cJSON_GetObjectItem(jrule, "id");
    if (jid && cJSON_IsString(jid) && jid->valuestring) {
        strlcpy(rule->id, jid->valuestring, sizeof(rule->id));
    }

    /* enabled (default true) */
    cJSON *jenabled = cJSON_GetObjectItem(jrule, "enabled");
    rule->enabled = jenabled ? cJSON_IsTrue(jenabled) : true;

    /* consume_on_match */
    cJSON *jconsume = cJSON_GetObjectItem(jrule, "consume_on_match");
    rule->consume_on_match = jconsume ? cJSON_IsTrue(jconsume) : false;

    /* match */
    cJSON *jmatch = cJSON_GetObjectItem(jrule, "match");
    if (jmatch && cJSON_IsObject(jmatch)) {
        cJSON *jt = cJSON_GetObjectItem(jmatch, "event_type");
        if (jt && cJSON_IsString(jt) && jt->valuestring)
            strlcpy(rule->match.event_type, jt->valuestring, sizeof(rule->match.event_type));

        cJSON *js = cJSON_GetObjectItem(jmatch, "source_cap");
        if (js && cJSON_IsString(js) && js->valuestring)
            strlcpy(rule->match.source_cap, js->valuestring, sizeof(rule->match.source_cap));

        cJSON *jc = cJSON_GetObjectItem(jmatch, "channel");
        if (jc && cJSON_IsString(jc) && jc->valuestring)
            strlcpy(rule->match.channel, jc->valuestring, sizeof(rule->match.channel));

        cJSON *jchat = cJSON_GetObjectItem(jmatch, "chat_id");
        if (jchat && cJSON_IsString(jchat) && jchat->valuestring)
            strlcpy(rule->match.chat_id, jchat->valuestring, sizeof(rule->match.chat_id));

        cJSON *jtc = cJSON_GetObjectItem(jmatch, "text_contains");
        if (jtc && cJSON_IsString(jtc) && jtc->valuestring)
            strlcpy(rule->match.text_contains, jtc->valuestring, sizeof(rule->match.text_contains));
    }

    /* actions */
    cJSON *jactions = cJSON_GetObjectItem(jrule, "actions");
    if (jactions && cJSON_IsArray(jactions)) {
        int count = cJSON_GetArraySize(jactions);
        if (count > 0) {
            /* calloc: small management struct, no DMA/cache alignment needed.
             * free_actions() and claw_event_dispatcher_free_rule() both use
             * free() to release this array. */
            claw_event_dispatcher_action_t *acts =
                calloc((size_t)count, sizeof(claw_event_dispatcher_action_t));
            if (!acts) return RTK_ERR_NOMEM;

            int i = 0;
            cJSON *jact = NULL;
            cJSON_ArrayForEach(jact, jactions) {
                cJSON *jkind = cJSON_GetObjectItem(jact, "kind");
                if (jkind && cJSON_IsString(jkind) && jkind->valuestring) {
                    acts[i].kind = action_kind_from_str(jkind->valuestring);
                }

                cJSON *jcap = cJSON_GetObjectItem(jact, "cap");
                if (jcap && cJSON_IsString(jcap) && jcap->valuestring) {
                    strlcpy(acts[i].cap, jcap->valuestring, sizeof(acts[i].cap));
                }

                cJSON *jinput = cJSON_GetObjectItem(jact, "input_json");
                if (jinput && cJSON_IsString(jinput) && jinput->valuestring && jinput->valuestring[0]) {
                    acts[i].input_json = strdup(jinput->valuestring);
                }

                cJSON *jfailopen = cJSON_GetObjectItem(jact, "fail_open");
                acts[i].fail_open = jfailopen ? cJSON_IsTrue(jfailopen) : false;

                i++;
            }

            *actions_out     = acts;
            *action_count_out = (size_t)count;
        }
    }

    rule->actions      = *actions_out;
    rule->action_count = *action_count_out;
    return RTK_SUCCESS;
}

/* ---- Helper: build cJSON object from rule ---- */

static cJSON *rule_to_json(const claw_event_dispatcher_rule_t *rule)
{
    cJSON *jrule = cJSON_CreateObject();
    if (!jrule) return NULL;

    cJSON_AddStringToObject(jrule, "id", rule->id);
    cJSON_AddBoolToObject(jrule, "enabled", rule->enabled);
    cJSON_AddBoolToObject(jrule, "consume_on_match", rule->consume_on_match);

    /* match */
    cJSON *jmatch = cJSON_CreateObject();
    if (jmatch) {
        cJSON_AddStringToObject(jmatch, "event_type",   rule->match.event_type);
        cJSON_AddStringToObject(jmatch, "source_cap",   rule->match.source_cap);
        cJSON_AddStringToObject(jmatch, "channel",      rule->match.channel);
        cJSON_AddStringToObject(jmatch, "chat_id",      rule->match.chat_id);
        cJSON_AddStringToObject(jmatch, "text_contains",rule->match.text_contains);
        cJSON_AddItemToObject(jrule, "match", jmatch);
    }

    /* actions */
    cJSON *jactions = cJSON_CreateArray();
    if (jactions) {
        for (size_t i = 0; i < rule->action_count; i++) {
            cJSON *jact = cJSON_CreateObject();
            if (jact) {
                cJSON_AddStringToObject(jact, "kind",
                                        action_kind_to_str(rule->actions[i].kind));
                cJSON_AddStringToObject(jact, "cap", rule->actions[i].cap);
                cJSON_AddStringToObject(jact, "input_json",
                                        rule->actions[i].input_json
                                            ? rule->actions[i].input_json : "");
                cJSON_AddBoolToObject(jact, "fail_open", rule->actions[i].fail_open);
                cJSON_AddItemToArray(jactions, jact);
            }
        }
        cJSON_AddItemToObject(jrule, "actions", jactions);
    }

    return jrule;
}

/* ---- Helper: free heap-allocated action input_json ---- */

static void free_actions(claw_event_dispatcher_action_t *actions, size_t count)
{
    if (!actions) return;
    for (size_t i = 0; i < count; i++) {
        /* input_json is strdup'd with libc malloc — free with free() */
        free(actions[i].input_json);
        actions[i].input_json = NULL;
    }
    /* acts array was calloc'd — free with libc free */
    free(actions);
}

/* ---- execute: router_list_rules ---- */

static int cap_router_list_rules(const char *input_json,
                                       const claw_cap_call_context_t *ctx,
                                       char **output)
{
    (void)input_json;
    (void)ctx;

    char *raw = read_file_alloc(s_rt.rules_file);
    if (!raw) {
        return claw_cap_set_output(output, "[]");
    }

    cJSON *jarr = cJSON_Parse(raw);
    free(raw);

    if (!jarr || !cJSON_IsArray(jarr)) {
        if (jarr) cJSON_Delete(jarr);
        return claw_cap_set_output(output, "[]");
    }

    char *s = cJSON_PrintUnformatted(jarr);
    cJSON_Delete(jarr);

    if (!s) {
        claw_cap_set_output(output, "[]");
        return RTK_ERR_NOMEM;
    }

    *output = s;
    return RTK_SUCCESS;
}

/* ---- execute: router_add_rule ---- */

static int cap_router_add_rule(const char *input_json,
                                     const claw_cap_call_context_t *ctx,
                                     char **output)
{
    (void)ctx;

    if (!input_json) {
        claw_cap_set_output(output, "{\"error\":\"no input\"}");
        return RTK_FAIL;
    }

    cJSON *jrule = cJSON_Parse(input_json);
    if (!jrule || !cJSON_IsObject(jrule)) {
        if (jrule) cJSON_Delete(jrule);
        claw_cap_set_output(output, "{\"error\":\"invalid rule JSON\"}");
        return RTK_FAIL;
    }

    claw_event_dispatcher_rule_t rule;
    claw_event_dispatcher_action_t *actions = NULL;
    size_t action_count = 0;

    int err = rule_from_json(jrule, &rule, &actions, &action_count);
    if (err != RTK_SUCCESS) {
        cJSON_Delete(jrule);
        claw_cap_set_output(output, "{\"error\":\"failed to parse rule\"}");
        return err;
    }

    /* Add to running router */
    int add_err = claw_event_dispatcher_add_rule(&rule);
    if (add_err != RTK_SUCCESS) {
        free_actions(actions, action_count);
        cJSON_Delete(jrule);
        claw_cap_set_output(output,
                 "{\"error\":\"router add failed: %d\"}", (int)add_err);
        return add_err;
    }

    /* Persist: read existing array, append, write back */
    char *raw = read_file_alloc(s_rt.rules_file);
    cJSON *jarr = NULL;
    if (raw) {
        jarr = cJSON_Parse(raw);
        free(raw);
    }
    if (!jarr || !cJSON_IsArray(jarr)) {
        if (jarr) cJSON_Delete(jarr);
        jarr = cJSON_CreateArray();
    }

    if (jarr) {
        /* Avoid duplicate: remove old entry with same id if any */
        const char *new_id = rule.id;
        cJSON *existing = NULL;
        cJSON_ArrayForEach(existing, jarr) {
            cJSON *jid = cJSON_GetObjectItem(existing, "id");
            if (jid && cJSON_IsString(jid) && strcmp(jid->valuestring, new_id) == 0) {
                cJSON_DetachItemViaPointer(jarr, existing);
                cJSON_Delete(existing);
                break;
            }
        }

        cJSON *jnew = rule_to_json(&rule);
        if (jnew) {
            cJSON_AddItemToArray(jarr, jnew);
        }
        write_rules_file(jarr);
        cJSON_Delete(jarr);
    }

    char rule_id[64];
    strlcpy(rule_id, rule.id, sizeof(rule_id));

    free_actions(actions, action_count);
    cJSON_Delete(jrule);

    return claw_cap_set_output(output,
             "{\"status\":\"added\",\"id\":\"%s\"}", rule_id);
}

/* ---- execute: router_remove_rule ---- */

static int cap_router_remove_rule(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char **output)
{
    (void)ctx;

    if (!input_json) {
        claw_cap_set_output(output, "{\"error\":\"no input\"}");
        return RTK_FAIL;
    }

    cJSON *jreq = cJSON_Parse(input_json);
    if (!jreq) {
        claw_cap_set_output(output, "{\"error\":\"invalid JSON\"}");
        return RTK_FAIL;
    }

    cJSON *jid = cJSON_GetObjectItem(jreq, "id");
    if (!jid || !cJSON_IsString(jid) || !jid->valuestring || jid->valuestring[0] == '\0') {
        cJSON_Delete(jreq);
        claw_cap_set_output(output, "{\"error\":\"missing required field: id\"}");
        return RTK_FAIL;
    }

    const char *target_id = jid->valuestring;

    /* Read existing array */
    char *raw = read_file_alloc(s_rt.rules_file);
    cJSON *jarr = NULL;
    if (raw) {
        jarr = cJSON_Parse(raw);
        free(raw);
    }
    if (!jarr || !cJSON_IsArray(jarr)) {
        if (jarr) cJSON_Delete(jarr);
        cJSON_Delete(jreq);
        claw_cap_set_output(output, "{\"error\":\"rule not found\"}");
        return RTK_FAIL;
    }

    bool found = false;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, jarr) {
        cJSON *jrid = cJSON_GetObjectItem(item, "id");
        if (jrid && cJSON_IsString(jrid) && strcmp(jrid->valuestring, target_id) == 0) {
            cJSON_DetachItemViaPointer(jarr, item);
            cJSON_Delete(item);
            found = true;
            break;
        }
    }

    if (!found) {
        cJSON_Delete(jarr);
        cJSON_Delete(jreq);
        claw_cap_set_output(output, "{\"error\":\"rule not found\"}");
        return RTK_FAIL;
    }

    write_rules_file(jarr);
    cJSON_Delete(jarr);
    cJSON_Delete(jreq);

    return claw_cap_set_output(output,
             "{\"status\":\"removed\","
             "\"note\":\"removed from persistent storage, takes effect on restart\"}");
}

/* ---- execute: router_reload_rules ---- */

static int cap_router_reload_rules(const char *input_json,
                                         const claw_cap_call_context_t *ctx,
                                         char **output)
{
    (void)input_json;
    (void)ctx;

    char *raw = read_file_alloc(s_rt.rules_file);
    if (!raw) {
        return claw_cap_set_output(output, "{\"status\":\"reloaded\",\"count\":0}");
    }

    cJSON *jarr = cJSON_Parse(raw);
    free(raw);

    if (!jarr || !cJSON_IsArray(jarr)) {
        if (jarr) cJSON_Delete(jarr);
        return claw_cap_set_output(output, "{\"status\":\"reloaded\",\"count\":0}");
    }

    int loaded = 0;
    cJSON *jrule = NULL;
    cJSON_ArrayForEach(jrule, jarr) {
        claw_event_dispatcher_rule_t rule;
        claw_event_dispatcher_action_t *actions = NULL;
        size_t action_count = 0;

        if (rule_from_json(jrule, &rule, &actions, &action_count) == RTK_SUCCESS) {
            if (claw_event_dispatcher_add_rule(&rule) == RTK_SUCCESS) {
                loaded++;
            }
            free_actions(actions, action_count);
        }
    }

    cJSON_Delete(jarr);

    return claw_cap_set_output(output,
             "{\"status\":\"reloaded\",\"count\":%d}", loaded);
}

/* ---- Cap descriptors & group ---- */

static const claw_cap_descriptor_t s_desc[] = {
    {
        .id          = "router_list_rules",
        .name        = "router_list_rules",
        .family      = "router_mgr",
        .description = "List all persistent event router rules.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{}}",
        .execute     = cap_router_list_rules,
    },
    {
        .id          = "router_add_rule",
        .name        = "router_add_rule",
        .family      = "router_mgr",
        .description = "Add a new event router rule. Takes a rule JSON object.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"id\":{\"type\":\"string\",\"description\":\"Unique rule ID\"},"
            "\"enabled\":{\"type\":\"boolean\",\"description\":\"Whether the rule is active\"},"
            "\"consume_on_match\":{\"type\":\"boolean\",\"description\":\"Stop processing further rules on match\"},"
            "\"match\":{\"type\":\"object\",\"description\":\"Match criteria (event_type, source_cap, channel, chat_id, text_contains)\"},"
            "\"actions\":{\"type\":\"array\",\"description\":\"List of actions (kind, cap, input_json)\"}"
            "},"
            "\"required\":[\"id\"]}",
        .execute     = cap_router_add_rule,
    },
    {
        .id          = "router_remove_rule",
        .name        = "router_remove_rule",
        .family      = "router_mgr",
        .description = "Remove a router rule by ID (takes effect on restart).",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"id\":{\"type\":\"string\",\"description\":\"Rule ID to remove\"}"
            "},"
            "\"required\":[\"id\"]}",
        .execute     = cap_router_remove_rule,
    },
    {
        .id          = "router_reload_rules",
        .name        = "router_reload_rules",
        .family      = "router_mgr",
        .description = "Reload all persistent rules into the running router.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{}}",
        .execute     = cap_router_reload_rules,
    },
};

static const claw_cap_group_t s_group = {
    .group_id         = "router_mgr",
    .plugin_name      = "cap_router_mgr",
    .version          = "1",
    .descriptors      = s_desc,
    .descriptor_count = 4,
};

/* ---- Public init ---- */

int cap_router_mgr_init(const cap_router_mgr_config_t *config)
{
    if (!config || !config->rules_dir) {
        return RTK_ERR_BADARG;
    }

    /* Create rules directory */
    mkdir(config->rules_dir, 0777);

    /* Build rules file path */
    DiagSnPrintf(s_rt.rules_file, sizeof(s_rt.rules_file),
             "%s/router_rules.json", config->rules_dir);

    /* Load existing rules on start */
    int loaded_count = 0;
    char *raw = read_file_alloc(s_rt.rules_file);
    if (raw) {
        cJSON *jarr = cJSON_Parse(raw);
        free(raw);
        if (jarr && cJSON_IsArray(jarr)) {
            cJSON *jrule = NULL;
            cJSON_ArrayForEach(jrule, jarr) {
                claw_event_dispatcher_rule_t rule;
                claw_event_dispatcher_action_t *actions = NULL;
                size_t action_count = 0;

                if (rule_from_json(jrule, &rule, &actions, &action_count) == RTK_SUCCESS) {
                    if (claw_event_dispatcher_add_rule(&rule) == RTK_SUCCESS) {
                        loaded_count++;
                    }
                    free_actions(actions, action_count);
                }
            }
            cJSON_Delete(jarr);
        } else if (jarr) {
            cJSON_Delete(jarr);
        }
    }

    int err = claw_cap_register_group(&s_group);
    if (err != RTK_SUCCESS) {
        RTK_LOGE(TAG, "Failed to register group: %d\n", (int)err);
        return err;
    }

    RTK_LOGI(TAG, "Initialized (rules_dir=%s, loaded %d rules)\n",
             config->rules_dir, loaded_count);
    return RTK_SUCCESS;
}

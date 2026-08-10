/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cap_router_mgr.h"
#include "claw_cap.h"
#include "claw_cap_registry.h"
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

/* ---- Enum <-> string helpers (RTK-specific serialization strings) ---- */

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
    case CLAW_DISPATCHER_ACT_AGENT:  return "rtk_agent";
    case CLAW_DISPATCHER_ACT_CAP:    return "rtk_cap";
    case CLAW_DISPATCHER_ACT_SCRIPT: return "rtk_script";
    case CLAW_DISPATCHER_ACT_SEND:   return "rtk_send";
    case CLAW_DISPATCHER_ACT_EMIT:   return "rtk_emit";
    case CLAW_DISPATCHER_ACT_DROP:   return "rtk_drop";
    default:                         return "rtk_agent";
    }
}

static claw_event_dispatcher_text_match_t text_match_from_str(const char *s)
{
    if (!s) return CLAW_DISPATCHER_TEXT_MATCH_NONE;
    if (strcmp(s, "exact")  == 0) return CLAW_DISPATCHER_TEXT_MATCH_EXACT;
    if (strcmp(s, "prefix") == 0) return CLAW_DISPATCHER_TEXT_MATCH_PREFIX;
    return CLAW_DISPATCHER_TEXT_MATCH_NONE;
}

static const char *text_match_to_str(claw_event_dispatcher_text_match_t m)
{
    switch (m) {
    case CLAW_DISPATCHER_TEXT_MATCH_EXACT:  return "exact";
    case CLAW_DISPATCHER_TEXT_MATCH_PREFIX: return "prefix";
    default:                                return "none";
    }
}

static claw_event_dispatcher_on_error_t on_error_from_str(const char *s)
{
    if (s && strcmp(s, "stop") == 0) return CLAW_DISPATCHER_ON_ERROR_STOP;
    return CLAW_DISPATCHER_ON_ERROR_CONTINUE;
}

static const char *on_error_to_str(claw_event_dispatcher_on_error_t e)
{
    return e == CLAW_DISPATCHER_ON_ERROR_STOP ? "stop" : "continue";
}

static claw_event_dispatcher_cmp_op_t op_from_str(const char *s)
{
    if (!s) return CLAW_DISPATCHER_OP_NONE;
    if (strcmp(s, "eq")       == 0) return CLAW_DISPATCHER_OP_EQ;
    if (strcmp(s, "ne")       == 0) return CLAW_DISPATCHER_OP_NE;
    if (strcmp(s, "gt")       == 0) return CLAW_DISPATCHER_OP_GT;
    if (strcmp(s, "lt")       == 0) return CLAW_DISPATCHER_OP_LT;
    if (strcmp(s, "ge")       == 0) return CLAW_DISPATCHER_OP_GE;
    if (strcmp(s, "le")       == 0) return CLAW_DISPATCHER_OP_LE;
    if (strcmp(s, "contains") == 0) return CLAW_DISPATCHER_OP_CONTAINS;
    if (strcmp(s, "exists")   == 0) return CLAW_DISPATCHER_OP_EXISTS;
    return CLAW_DISPATCHER_OP_NONE;
}

static const char *op_to_str(claw_event_dispatcher_cmp_op_t op)
{
    switch (op) {
    case CLAW_DISPATCHER_OP_EQ:       return "eq";
    case CLAW_DISPATCHER_OP_NE:       return "ne";
    case CLAW_DISPATCHER_OP_GT:       return "gt";
    case CLAW_DISPATCHER_OP_LT:       return "lt";
    case CLAW_DISPATCHER_OP_GE:       return "ge";
    case CLAW_DISPATCHER_OP_LE:       return "le";
    case CLAW_DISPATCHER_OP_CONTAINS: return "contains";
    case CLAW_DISPATCHER_OP_EXISTS:   return "exists";
    default:                          return "none";
    }
}

/* ---- File helpers ---- */

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

/* Atomic write: write to <file>.tmp then rename over the target, so a crash
 * or power loss mid-write cannot corrupt the existing ruleset. */
static int write_rules_file(cJSON *jarr)
{
    char *s = cJSON_PrintUnformatted(jarr);
    if (!s) return RTK_ERR_NOMEM;

    char tmp[160];
    DiagSnPrintf(tmp, sizeof(tmp), "%s.tmp", s_rt.rules_file);

    FILE *f = fopen(tmp, "w");
    if (!f) {
        free(s);
        return RTK_FAIL;
    }
    size_t len = strlen(s);
    size_t wr = fwrite(s, 1, len, f);
    fclose(f);
    free(s);

    if (wr != len) {
        remove(tmp);
        return RTK_FAIL;
    }
    if (rename(tmp, s_rt.rules_file) != 0) {
        /* Fallback: some VFS builds may not support rename — write directly. */
        remove(tmp);
        char *s2 = cJSON_PrintUnformatted(jarr);
        if (!s2) return RTK_ERR_NOMEM;
        FILE *df = fopen(s_rt.rules_file, "w");
        if (!df) { free(s2); return RTK_FAIL; }
        fputs(s2, df);
        fclose(df);
        free(s2);
    }
    return RTK_SUCCESS;
}

/* ---- JSON <-> rule ---- */

static void match_from_json(const cJSON *jmatch, claw_event_dispatcher_match_t *m)
{
    if (!jmatch || !cJSON_IsObject(jmatch)) return;
    cJSON *j;

#define COPY_STR(field, key) \
    j = cJSON_GetObjectItem(jmatch, key); \
    if (j && cJSON_IsString(j) && j->valuestring) \
        strlcpy(m->field, j->valuestring, sizeof(m->field));

    COPY_STR(event_type,    "event_type");
    COPY_STR(source_cap,    "source_cap");
    COPY_STR(channel,       "channel");
    COPY_STR(chat_id,       "chat_id");
    COPY_STR(text_contains, "text_contains");
    COPY_STR(event_key,     "event_key");
    COPY_STR(text,          "text");
#undef COPY_STR

    j = cJSON_GetObjectItem(jmatch, "text_match_rule");
    if (j && cJSON_IsString(j))
        m->text_match_rule = text_match_from_str(j->valuestring);
}

static void action_from_json(const cJSON *jact, claw_event_dispatcher_action_t *a)
{
    cJSON *j;

    j = cJSON_GetObjectItem(jact, "kind");
    if (j && cJSON_IsString(j) && j->valuestring)
        a->kind = action_kind_from_str(j->valuestring);

    j = cJSON_GetObjectItem(jact, "cap");
    if (j && cJSON_IsString(j) && j->valuestring)
        strlcpy(a->cap, j->valuestring, sizeof(a->cap));

    /* SCRIPT: absolute .lua path + launch mode. "mode":"sync" blocks until the
     * script returns (so its result can chain via @{last.output}); anything
     * else — including omitted — is async fire-and-forget (the default). */
    j = cJSON_GetObjectItem(jact, "script");
    if (j && cJSON_IsString(j) && j->valuestring)
        strlcpy(a->script, j->valuestring, sizeof(a->script));

    j = cJSON_GetObjectItem(jact, "mode");
    a->script_sync = (j && cJSON_IsString(j) && j->valuestring &&
                      strcmp(j->valuestring, "sync") == 0);

    /* input: object/array (CAP/EMIT) serialized to a template string, or a
     * plain string (SEND). Legacy "input_json" string still accepted. */
    j = cJSON_GetObjectItem(jact, "input");
    if (j) {
        if (cJSON_IsString(j) && j->valuestring && j->valuestring[0]) {
            a->input_json = strdup(j->valuestring);
        } else if (cJSON_IsObject(j) || cJSON_IsArray(j)) {
            char *s = cJSON_PrintUnformatted(j);
            if (s) { a->input_json = strdup(s); cJSON_free(s); }
        }
    } else {
        j = cJSON_GetObjectItem(jact, "input_json");
        if (j && cJSON_IsString(j) && j->valuestring && j->valuestring[0])
            a->input_json = strdup(j->valuestring);
    }

    /* on_error (default continue). Legacy rules used fail_open, which never
     * affected control flow — so any legacy rule maps to the continue default. */
    j = cJSON_GetObjectItem(jact, "on_error");
    a->on_error = (j && cJSON_IsString(j)) ? on_error_from_str(j->valuestring)
                                           : CLAW_DISPATCHER_ON_ERROR_CONTINUE;

    j = cJSON_GetObjectItem(jact, "capture_output");
    a->capture_output = (j && cJSON_IsTrue(j));

    /* only_if guard { left, op, right } */
    cJSON *jg = cJSON_GetObjectItem(jact, "only_if");
    if (jg && cJSON_IsObject(jg)) {
        cJSON *jop = cJSON_GetObjectItem(jg, "op");
        a->only_if.op = op_from_str(jop && cJSON_IsString(jop) ? jop->valuestring : NULL);
        cJSON *jl = cJSON_GetObjectItem(jg, "left");
        if (jl && cJSON_IsString(jl) && jl->valuestring)
            strlcpy(a->only_if.left, jl->valuestring, sizeof(a->only_if.left));
        cJSON *jr = cJSON_GetObjectItem(jg, "right");
        if (jr) {
            if (cJSON_IsString(jr) && jr->valuestring) {
                strlcpy(a->only_if.right, jr->valuestring, sizeof(a->only_if.right));
            } else if (cJSON_IsNumber(jr)) {
                DiagSnPrintf(a->only_if.right, sizeof(a->only_if.right), "%d", (int)jr->valuedouble);
            }
        }
    }
}

/* Build a dispatcher rule (+ heap actions array) from JSON. */
static int rule_from_json(const cJSON *jrule,
                          claw_event_dispatcher_rule_t *rule,
                          claw_event_dispatcher_action_t **actions_out,
                          size_t *action_count_out)
{
    _memset(rule, 0, sizeof(*rule));
    *actions_out      = NULL;
    *action_count_out = 0;

    cJSON *j = cJSON_GetObjectItem(jrule, "id");
    if (j && cJSON_IsString(j) && j->valuestring)
        strlcpy(rule->id, j->valuestring, sizeof(rule->id));

    j = cJSON_GetObjectItem(jrule, "enabled");
    rule->enabled = j ? cJSON_IsTrue(j) : true;

    j = cJSON_GetObjectItem(jrule, "consume_on_match");
    rule->consume_on_match = j ? cJSON_IsTrue(j) : false;

    j = cJSON_GetObjectItem(jrule, "ack");
    if (j && cJSON_IsString(j) && j->valuestring)
        strlcpy(rule->ack, j->valuestring, sizeof(rule->ack));

    j = cJSON_GetObjectItem(jrule, "cooldown_ms");
    if (j && cJSON_IsNumber(j) && j->valuedouble > 0)
        rule->cooldown_ms = (uint32_t)j->valuedouble;

    /* vars: object serialized to a string; legacy "vars_json" string accepted. */
    j = cJSON_GetObjectItem(jrule, "vars");
    if (j && (cJSON_IsObject(j) || cJSON_IsArray(j))) {
        char *s = cJSON_PrintUnformatted(j);
        if (s) { rule->vars_json = strdup(s); cJSON_free(s); }
    } else {
        j = cJSON_GetObjectItem(jrule, "vars_json");
        if (j && cJSON_IsString(j) && j->valuestring && j->valuestring[0])
            rule->vars_json = strdup(j->valuestring);
    }

    match_from_json(cJSON_GetObjectItem(jrule, "match"), &rule->match);

    cJSON *jactions = cJSON_GetObjectItem(jrule, "actions");
    if (jactions && cJSON_IsArray(jactions)) {
        int count = cJSON_GetArraySize(jactions);
        if (count > 0) {
            claw_event_dispatcher_action_t *acts =
                calloc((size_t)count, sizeof(claw_event_dispatcher_action_t));
            if (!acts) {
                free(rule->vars_json);
                rule->vars_json = NULL;
                return RTK_ERR_NOMEM;
            }
            int i = 0;
            cJSON *jact = NULL;
            cJSON_ArrayForEach(jact, jactions) {
                action_from_json(jact, &acts[i]);
                i++;
            }
            *actions_out      = acts;
            *action_count_out = (size_t)count;
        }
    }

    rule->actions      = *actions_out;
    rule->action_count = *action_count_out;
    return RTK_SUCCESS;
}

static cJSON *action_to_json(const claw_event_dispatcher_action_t *a)
{
    cJSON *jact = cJSON_CreateObject();
    if (!jact) return NULL;

    cJSON_AddStringToObject(jact, "kind", action_kind_to_str(a->kind));
    if (a->cap[0]) cJSON_AddStringToObject(jact, "cap", a->cap);
    if (a->script[0]) cJSON_AddStringToObject(jact, "script", a->script);
    /* Only emit mode when it deviates from the async default, keeping stored
     * rules minimal (matches how on_error only serializes when non-default). */
    if (a->script_sync) cJSON_AddStringToObject(jact, "mode", "sync");

    if (a->input_json && a->input_json[0]) {
        cJSON *pj = cJSON_Parse(a->input_json);
        if (pj && (cJSON_IsObject(pj) || cJSON_IsArray(pj))) {
            cJSON_AddItemToObject(jact, "input", pj);
        } else {
            if (pj) cJSON_Delete(pj);
            cJSON_AddStringToObject(jact, "input", a->input_json);
        }
    }

    if (a->on_error == CLAW_DISPATCHER_ON_ERROR_STOP)
        cJSON_AddStringToObject(jact, "on_error", on_error_to_str(a->on_error));
    if (a->capture_output)
        cJSON_AddBoolToObject(jact, "capture_output", true);

    if (a->only_if.op != CLAW_DISPATCHER_OP_NONE) {
        cJSON *jg = cJSON_CreateObject();
        if (jg) {
            cJSON_AddStringToObject(jg, "left", a->only_if.left);
            cJSON_AddStringToObject(jg, "op",   op_to_str(a->only_if.op));
            cJSON_AddStringToObject(jg, "right", a->only_if.right);
            cJSON_AddItemToObject(jact, "only_if", jg);
        }
    }
    return jact;
}

static cJSON *rule_to_json(const claw_event_dispatcher_rule_t *rule)
{
    cJSON *jrule = cJSON_CreateObject();
    if (!jrule) return NULL;

    cJSON_AddStringToObject(jrule, "id", rule->id);
    cJSON_AddBoolToObject(jrule, "enabled", rule->enabled);
    if (rule->consume_on_match)
        cJSON_AddBoolToObject(jrule, "consume_on_match", true);
    if (rule->ack[0])
        cJSON_AddStringToObject(jrule, "ack", rule->ack);
    if (rule->cooldown_ms > 0)
        cJSON_AddNumberToObject(jrule, "cooldown_ms", rule->cooldown_ms);
    if (rule->vars_json && rule->vars_json[0]) {
        cJSON *v = cJSON_Parse(rule->vars_json);
        if (v) cJSON_AddItemToObject(jrule, "vars", v);
    }

    cJSON *jmatch = cJSON_CreateObject();
    if (jmatch) {
        const claw_event_dispatcher_match_t *m = &rule->match;
        if (m->event_type[0])    cJSON_AddStringToObject(jmatch, "event_type",    m->event_type);
        if (m->source_cap[0])    cJSON_AddStringToObject(jmatch, "source_cap",    m->source_cap);
        if (m->channel[0])       cJSON_AddStringToObject(jmatch, "channel",       m->channel);
        if (m->chat_id[0])       cJSON_AddStringToObject(jmatch, "chat_id",       m->chat_id);
        if (m->text_contains[0]) cJSON_AddStringToObject(jmatch, "text_contains", m->text_contains);
        if (m->event_key[0])     cJSON_AddStringToObject(jmatch, "event_key",     m->event_key);
        if (m->text[0])          cJSON_AddStringToObject(jmatch, "text",          m->text);
        if (m->text_match_rule != CLAW_DISPATCHER_TEXT_MATCH_NONE)
            cJSON_AddStringToObject(jmatch, "text_match_rule", text_match_to_str(m->text_match_rule));
        cJSON_AddItemToObject(jrule, "match", jmatch);
    }

    cJSON *jactions = cJSON_CreateArray();
    if (jactions) {
        for (size_t i = 0; i < rule->action_count; i++) {
            cJSON *ja = action_to_json(&rule->actions[i]);
            if (ja) cJSON_AddItemToArray(jactions, ja);
        }
        cJSON_AddItemToObject(jrule, "actions", jactions);
    }
    return jrule;
}

static void free_actions(claw_event_dispatcher_action_t *actions, size_t count)
{
    if (!actions) return;
    for (size_t i = 0; i < count; i++) {
        free(actions[i].input_json);
        actions[i].input_json = NULL;
    }
    free(actions);
}

static void free_parsed_rule(claw_event_dispatcher_rule_t *rule,
                             claw_event_dispatcher_action_t *actions, size_t count)
{
    free_actions(actions, count);
    if (rule->vars_json) { free(rule->vars_json); rule->vars_json = NULL; }
}

/* ---- Persist helpers: load array, upsert/remove by id, write back ---- */

static cJSON *load_rules_array(void)
{
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
    return jarr;
}

static void array_remove_id(cJSON *jarr, const char *id)
{
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, jarr) {
        cJSON *jid = cJSON_GetObjectItem(item, "id");
        if (jid && cJSON_IsString(jid) && jid->valuestring &&
                strcmp(jid->valuestring, id) == 0) {
            cJSON_DetachItemViaPointer(jarr, item);
            cJSON_Delete(item);
            return;
        }
    }
}

/* ---- execute: router_list_rules ---- */

static int cap_router_list_rules(const char *input_json,
                                 const claw_cap_call_context_t *ctx, char **output)
{
    (void)input_json;
    (void)ctx;

    char *raw = read_file_alloc(s_rt.rules_file);
    if (!raw) return claw_cap_set_output(output, "[]");

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

/* ---- execute: router_get_rule (reads the RUNNING rule + live stats) ---- */

static int cap_router_get_rule(const char *input_json,
                               const claw_cap_call_context_t *ctx, char **output)
{
    (void)ctx;
    if (!input_json) return claw_cap_set_output(output, "{\"error\":\"no input\"}");

    cJSON *jreq = cJSON_Parse(input_json);
    cJSON *jid = jreq ? cJSON_GetObjectItem(jreq, "id") : NULL;
    if (!jid || !cJSON_IsString(jid) || !jid->valuestring || !jid->valuestring[0]) {
        if (jreq) cJSON_Delete(jreq);
        return claw_cap_set_output(output, "{\"error\":\"missing required field: id\"}");
    }

    claw_event_dispatcher_rule_t rule;
    int rc = claw_event_dispatcher_get_rule(jid->valuestring, &rule);
    cJSON_Delete(jreq);
    if (rc != RTK_SUCCESS) return claw_cap_set_output(output, "{\"error\":\"rule not found\"}");

    cJSON *jrule = rule_to_json(&rule);
    if (jrule) {
        cJSON_AddNumberToObject(jrule, "fire_count", rule.fire_count);
        cJSON_AddNumberToObject(jrule, "last_fired_ts", rule.last_fired_ts);
    }
    claw_event_dispatcher_free_rule(&rule);

    char *s = jrule ? cJSON_PrintUnformatted(jrule) : NULL;
    if (jrule) cJSON_Delete(jrule);
    if (!s) return claw_cap_set_output(output, "{\"error\":\"serialize failed\"}");
    *output = s;
    return RTK_SUCCESS;
}

/* ---- execute: router_add_rule / router_update_rule (shared body) ---- */

static int add_or_update(const char *input_json, char **output, bool require_existing)
{
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

    if (rule_from_json(jrule, &rule, &actions, &action_count) != RTK_SUCCESS) {
        cJSON_Delete(jrule);
        claw_cap_set_output(output, "{\"error\":\"failed to parse rule\"}");
        return RTK_FAIL;
    }

    if (!rule.id[0]) {
        free_parsed_rule(&rule, actions, action_count);
        cJSON_Delete(jrule);
        claw_cap_set_output(output, "{\"error\":\"missing required field: id\"}");
        return RTK_FAIL;
    }

    /* Light validation: reject a wildcard match with no effective action, which
     * would swallow every message and suppress the agent fall-through. */
    bool wildcard = (rule.match.event_type[0] == '\0' && rule.match.source_cap[0] == '\0' &&
                     rule.match.channel[0] == '\0' && rule.match.chat_id[0] == '\0' &&
                     rule.match.text_contains[0] == '\0' && rule.match.event_key[0] == '\0' &&
                     rule.match.text_match_rule == CLAW_DISPATCHER_TEXT_MATCH_NONE);
    if (wildcard && action_count == 0) {
        free_parsed_rule(&rule, actions, action_count);
        cJSON_Delete(jrule);
        claw_cap_set_output(output,
            "{\"error\":\"rule matches everything but has no actions (would silently swallow all events)\"}");
        return RTK_FAIL;
    }

    int rc = require_existing ? claw_event_dispatcher_update_rule(&rule)
                              : claw_event_dispatcher_add_rule(&rule);
    if (rc != RTK_SUCCESS) {
        free_parsed_rule(&rule, actions, action_count);
        cJSON_Delete(jrule);
        if (require_existing && rc == RTK_FAIL)
            claw_cap_set_output(output, "{\"error\":\"rule not found (use router_add_rule to create)\"}");
        else
            claw_cap_set_output(output, "{\"error\":\"router rejected rule: %d\"}", (int)rc);
        return rc;
    }

    /* Persist: upsert into the on-disk array (atomic write). */
    cJSON *jarr = load_rules_array();
    if (jarr) {
        array_remove_id(jarr, rule.id);
        cJSON *jnew = rule_to_json(&rule);
        if (jnew) cJSON_AddItemToArray(jarr, jnew);
        write_rules_file(jarr);
        cJSON_Delete(jarr);
    }

    char rid[64];
    strlcpy(rid, rule.id, sizeof(rid));
    free_parsed_rule(&rule, actions, action_count);
    cJSON_Delete(jrule);

    return claw_cap_set_output(output, "{\"status\":\"%s\",\"id\":\"%s\"}",
                               require_existing ? "updated" : "added", rid);
}

static int cap_router_add_rule(const char *input_json,
                               const claw_cap_call_context_t *ctx, char **output)
{
    (void)ctx;
    return add_or_update(input_json, output, false);
}

static int cap_router_update_rule(const char *input_json,
                                  const claw_cap_call_context_t *ctx, char **output)
{
    (void)ctx;
    return add_or_update(input_json, output, true);
}

/* ---- execute: router_remove_rule (hot delete, immediate) ---- */

static int cap_router_remove_rule(const char *input_json,
                                  const claw_cap_call_context_t *ctx, char **output)
{
    (void)ctx;
    if (!input_json) return claw_cap_set_output(output, "{\"error\":\"no input\"}");

    cJSON *jreq = cJSON_Parse(input_json);
    cJSON *jid = jreq ? cJSON_GetObjectItem(jreq, "id") : NULL;
    if (!jid || !cJSON_IsString(jid) || !jid->valuestring || !jid->valuestring[0]) {
        if (jreq) cJSON_Delete(jreq);
        return claw_cap_set_output(output, "{\"error\":\"missing required field: id\"}");
    }
    const char *id = jid->valuestring;

    /* Hot-delete from the running dispatcher (takes effect immediately). */
    int hot = claw_event_dispatcher_delete_rule(id);

    /* Remove from persistent storage. */
    cJSON *jarr = load_rules_array();
    bool in_file = false;
    if (jarr) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, jarr) {
            cJSON *ji = cJSON_GetObjectItem(item, "id");
            if (ji && cJSON_IsString(ji) && ji->valuestring && strcmp(ji->valuestring, id) == 0) {
                in_file = true;
                break;
            }
        }
        if (in_file) {
            array_remove_id(jarr, id);
            write_rules_file(jarr);
        }
        cJSON_Delete(jarr);
    }

    cJSON_Delete(jreq);

    if (hot != RTK_SUCCESS && !in_file)
        return claw_cap_set_output(output, "{\"error\":\"rule not found\"}");
    return claw_cap_set_output(output, "{\"status\":\"removed\",\"id\":\"%s\"}", id);
}

/* ---- execute: router_reload_rules ---- */

static int cap_router_reload_rules(const char *input_json,
                                   const claw_cap_call_context_t *ctx, char **output)
{
    (void)input_json;
    (void)ctx;

    char *raw = read_file_alloc(s_rt.rules_file);
    if (!raw) return claw_cap_set_output(output, "{\"status\":\"reloaded\",\"count\":0}");

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
            if (claw_event_dispatcher_add_rule(&rule) == RTK_SUCCESS) loaded++;
            free_parsed_rule(&rule, actions, action_count);
        }
    }
    cJSON_Delete(jarr);
    return claw_cap_set_output(output, "{\"status\":\"reloaded\",\"count\":%d}", loaded);
}

/* ---- Cap descriptors & group ---- */

/* Full rule schema, inlined so the LLM sees every field + the action-kind
 * enum + a complete copyable example on every call. */
#define RULE_SCHEMA_PROPS \
    "\"id\":{\"type\":\"string\",\"description\":\"Unique rule id (also the key for update/remove).\"}," \
    "\"enabled\":{\"type\":\"boolean\",\"description\":\"Default true.\"}," \
    "\"consume_on_match\":{\"type\":\"boolean\",\"description\":\"Stop evaluating later rules once this one matches.\"}," \
    "\"ack\":{\"type\":\"string\",\"description\":\"Instant reply sent to the source channel the moment the rule matches, BEFORE actions run. May use @{event.*}/@{vars.*}/@{match.*} but NOT @{last.*}. Omit for silent (drop/send-only) rules.\"}," \
    "\"cooldown_ms\":{\"type\":\"integer\",\"description\":\"Minimum milliseconds between fires; extra triggers are skipped. Use to debounce noisy sensors / avoid flooding the LLM.\"}," \
    "\"vars\":{\"type\":\"object\",\"description\":\"Rule-level constants, referenced in templates as @{vars.key}.\"}," \
    "\"match\":{\"type\":\"object\",\"description\":\"All criteria AND together; empty/omitted = wildcard. Fields: event_type, source_cap, channel, chat_id (exact); event_key (exact vs the event correlation id); text_contains (substring); text + text_match_rule ('exact'|'prefix'; prefix exposes the trailing part as @{match.remainder}).\"}," \
    "\"actions\":{\"type\":\"array\",\"description\":\"Run in order when the rule matches. Each action: {kind, ...}. kind is one of: 'rtk_agent' (hand the message to the LLM agent), 'rtk_cap' (call a capability: set cap=<capability id>, input=<JSON object args>), 'rtk_send' (send an IM message: set cap=<target channel> or omit to reply to source, input=<plain text string>), 'rtk_emit' (publish a derived event: input=<JSON payload>), 'rtk_drop' (swallow the event), 'rtk_script' (run a Lua script with NO LLM in the loop — the offline sensor->actuator path: set script=<absolute .lua path, e.g. vfs:/skills/fan/run.lua; vfs:/tmp/ is rejected>, input=<JSON object passed to the script as its args>, and optional mode ('async' default = fire-and-forget background job; 'sync' = block until it returns so its result chains via @{last.output}). A given rule will not stack overlapping async runs of its own script; debounce noisy sensors with the rule-level cooldown_ms). Optional per-action fields: on_error ('continue' default | 'stop' remaining actions), capture_output (bool; feed this action's output into @{last.output} for the next action — for rtk_script this needs mode='sync', otherwise you capture only the job-start info), only_if ({left,op,right} single guard; op one of eq/ne/gt/lt/ge/le/contains/exists; numeric compare when both sides are numbers).\"}"

#define RULE_EXAMPLE \
    " Example (chat command): {\"id\":\"weather_cmd\",\"match\":{\"text\":\"/weather\",\"text_match_rule\":\"prefix\"}," \
    "\"actions\":[{\"kind\":\"rtk_cap\",\"cap\":\"web_search\",\"input\":{\"query\":\"weather @{match.remainder}\"},\"capture_output\":true}," \
    "{\"kind\":\"rtk_send\",\"input\":\"Weather for @{match.remainder}: @{last.output}\"}]}." \
    " Example (offline sensor->actuator, no LLM): {\"id\":\"fan_on_hot\",\"cooldown_ms\":10000," \
    "\"match\":{\"event_type\":\"sensor.temp\"}," \
    "\"actions\":[{\"kind\":\"rtk_script\",\"script\":\"vfs:/skills/fan/run.lua\"," \
    "\"only_if\":{\"left\":\"@{event.payload.temp}\",\"op\":\"gt\",\"right\":30}," \
    "\"input\":{\"temp\":\"@{event.payload.temp}\",\"action\":\"on\"}}]}"

static const claw_cap_descriptor_t s_desc[] = {
    {
        .id          = "router_list_rules",
        .name        = "router_list_rules",
        .family      = "router_mgr",
        .description = "List all persistent event router rules.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute     = cap_router_list_rules,
    },
    {
        .id          = "router_get_rule",
        .name        = "router_get_rule",
        .family      = "router_mgr",
        .description = "Get one running rule by id, including live stats (fire_count, last_fired_ts). Use this to debug why a rule did or did not fire.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"id\":{\"type\":\"string\",\"description\":\"Rule id to fetch\"}},"
            "\"required\":[\"id\"]}",
        .execute     = cap_router_get_rule,
    },
    {
        .id          = "router_add_rule",
        .name        = "router_add_rule",
        .family      = "router_mgr",
        .description = "Add (or replace by id) an event router rule; takes effect immediately." RULE_EXAMPLE,
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{" RULE_SCHEMA_PROPS "},\"required\":[\"id\",\"match\",\"actions\"]}",
        .execute     = cap_router_add_rule,
    },
    {
        .id          = "router_update_rule",
        .name        = "router_update_rule",
        .family      = "router_mgr",
        .description = "Update an existing rule by id (hot replace, no restart). Same schema as router_add_rule; errors if the id does not exist.",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{" RULE_SCHEMA_PROPS "},\"required\":[\"id\",\"match\",\"actions\"]}",
        .execute     = cap_router_update_rule,
    },
    {
        .id          = "router_remove_rule",
        .name        = "router_remove_rule",
        .family      = "router_mgr",
        .description = "Remove a router rule by id (hot delete, takes effect immediately).",
        .kind        = CLAW_CAP_KIND_INVOKE,
        .cap_flags   = CLAW_CAP_FLAG_LLM_ACCESS,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"id\":{\"type\":\"string\",\"description\":\"Rule id to remove\"}},"
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
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute     = cap_router_reload_rules,
    },
};

static const claw_cap_group_t s_group = {
    .group_id         = "router_mgr",
    .plugin_name      = "cap_router_mgr",
    .version          = "1",
    .descriptors      = s_desc,
    .descriptor_count = sizeof(s_desc) / sizeof(s_desc[0]),
};

/* ---- Public init ---- */

int cap_router_mgr_init(const cap_router_mgr_config_t *config)
{
    if (!config || !config->rules_dir) return RTK_ERR_BADARG;

    mkdir(config->rules_dir, 0777);
    DiagSnPrintf(s_rt.rules_file, sizeof(s_rt.rules_file),
                 "%s/router_rules.json", config->rules_dir);

    /* Load existing rules on start; each rule is parsed independently so one
     * bad entry is skipped rather than aborting the whole ruleset. */
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
                    if (claw_event_dispatcher_add_rule(&rule) == RTK_SUCCESS) loaded_count++;
                    free_parsed_rule(&rule, actions, action_count);
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

/* ---- Lifecycle registration (claw_cap_registry): IO phase ----
 * High order so the group registers AFTER phase_agent's base-visibility
 * snapshot, keeping these tools hidden from the LLM (as the historical late
 * cap_router_mgr_init() call did). */
static void router_mgr_on_io(const claw_config_t *cfg)
{
    (void)cfg;
    const cap_router_mgr_config_t c = { .rules_dir = "vfs:/router_rules", .max_rules = 32 };
    cap_router_mgr_init(&c);
}
CLAW_CAP_REGISTER(router_mgr, {
    .group = "router_mgr",
    .order = 200,
    .on_io = router_mgr_on_io,
});

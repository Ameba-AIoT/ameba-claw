/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ameba_soc.h"
#include "atcmd_service.h"
#include "claw_cap.h"
#include <string.h>
#include <stdlib.h>
#include <cJSON.h>

/* ---- Background tasks ---- */

/* Returns 1 if file exists. */
static int skill_file_exists(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void skill_at_task(void *p)
{
    typedef struct { char name[64]; char args[256]; } skill_at_args_t;
    skill_at_args_t *a = (skill_at_args_t *)p;

    /* Inc 3: skill_run is gone — resolve <name> to the script path
     * (rolfs: built-in first, then user vfs:) and call lua_run(path, args).
     * Inc 4: if <name> already ends in ".lua" it is treated as a relative
     * script path under the skills root (e.g. "skill_authoring/scripts/x.lua"),
     * so non-main scripts can be exercised directly. */
    char path[160];
    size_t nlen = strlen(a->name);
    bool is_rel_lua = (nlen >= 4 && strcmp(a->name + nlen - 4, ".lua") == 0);
    if (is_rel_lua) {
        snprintf(path, sizeof(path), "rolfs:/skills/%s", a->name);
        if (!skill_file_exists(path)) {
            snprintf(path, sizeof(path), "vfs:/skills/%s", a->name);
        }
    } else {
        snprintf(path, sizeof(path), "rolfs:/skills/%s/scripts/main.lua", a->name);
        if (!skill_file_exists(path)) {
            snprintf(path, sizeof(path), "vfs:/skills/%s/scripts/main.lua", a->name);
        }
    }

    cJSON *jinput = cJSON_CreateObject();
    if (!jinput) {
        at_printf("\r\n+CLAW:skill,error=oom\r\n");
        at_printf(ATCMD_ERROR_END_STR, 2);
        free(a);
        rtos_task_delete(NULL);
        return;
    }
    cJSON_AddStringToObject(jinput, "path", path);
    /* args (reconstructed JSON) becomes the lua_run args OBJECT (#6 Lua table). */
    cJSON *jargs = cJSON_Parse(a->args);
    if (jargs && cJSON_IsObject(jargs)) {
        cJSON_AddItemToObject(jinput, "args", jargs);
    } else {
        if (jargs) cJSON_Delete(jargs);
        cJSON_AddItemToObject(jinput, "args", cJSON_CreateObject());
    }
    char *input_str = cJSON_PrintUnformatted(jinput);
    cJSON_Delete(jinput);
    if (!input_str) {
        at_printf("\r\n+CLAW:skill,error=oom\r\n");
        at_printf(ATCMD_ERROR_END_STR, 3);
        free(a);
        rtos_task_delete(NULL);
        return;
    }

    at_printf("\r\n+CLAW:skill,running=%s,path=%s,args=%s\r\n", a->name, path, a->args);
    free(a);

    claw_cap_call_context_t ctx = {0};
    ctx.caller = CLAW_CAP_CALLER_MANUAL;
    char *output = NULL;
    int rc = claw_cap_call("lua_run", input_str, &ctx, &output);
    free(input_str);

    if (rc == RTK_SUCCESS && output) {
        at_printf("+CLAW:skill,result=%s\r\n", output);
        at_printf(ATCMD_OK_END_STR);
    } else {
        at_printf("+CLAW:skill,rc=%d\r\n", rc);
        if (output) at_printf("+CLAW:skill,msg=%s\r\n", output);
        at_printf(ATCMD_ERROR_END_STR, rc ? rc : -1);
    }
    free(output);
    rtos_task_delete(NULL);
}

static void tools_list_task(void *p)
{
    char *sid = (char *)p;

    claw_cap_call_context_t ctx = {0};
    ctx.session_id = sid;
    ctx.caller     = CLAW_CAP_CALLER_LLM;

    char *tools_json = claw_cap_build_llm_tools_json(&ctx, false);
    if (!tools_json) {
        at_printf("\r\n+CLAW:tools,session=%s,error=build_failed\r\n", sid);
        free(sid);
        rtos_task_delete(NULL);
        return;
    }

    cJSON *arr = cJSON_Parse(tools_json);
    free(tools_json);
    if (!arr || !cJSON_IsArray(arr)) {
        at_printf("\r\n+CLAW:tools,session=%s,error=parse\r\n", sid);
        cJSON_Delete(arr);
        free(sid);
        rtos_task_delete(NULL);
        return;
    }

    int count = cJSON_GetArraySize(arr);
    at_printf("\r\n+CLAW:tools,session=%s,count=%d\r\n", sid, count);
    int idx = 0;
    const cJSON *tool;
    cJSON_ArrayForEach(tool, arr) {
        const cJSON *fn = cJSON_GetObjectItem(tool, "function");
        const cJSON *nm = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
        const char *name = (nm && cJSON_IsString(nm) && nm->valuestring)
                           ? nm->valuestring : "?";
        at_printf("+CLAW:tools,#%d=%s\r\n", idx++, name);
    }
    cJSON_Delete(arr);
    at_printf(ATCMD_OK_END_STR);
    free(sid);
    rtos_task_delete(NULL);
}

/* ---- Handlers ---- */

void handle_cmd_skill(u16 argc, char **argv, const char *arg2, const char *arg3)
{
    if (arg2[0] == '\0') {
        at_printf("\r\n+CLAW:usage: AT+CLAW=skill,<name>[,<args_json>]\r\n");
        at_printf(ATCMD_ERROR_END_STR, 1);
        return;
    }
    const char *skill_name = arg2;

    /* Reconstruct args JSON from comma-split argv[3..] */
    char args_json[256];
    if (argc >= 4 && argv[3] && argv[3][0]) {
        strlcpy(args_json, argv[3], sizeof(args_json));
        for (int i = 4; i < argc && argv[i]; i++) {
            strlcat(args_json, ",", sizeof(args_json));
            strlcat(args_json, argv[i], sizeof(args_json));
        }
    } else {
        strlcpy(args_json, "{}", sizeof(args_json));
    }

    /* skill_at_task resolves <name> to a script path and calls lua_run.
     * lua_run does its Lua setup inside its own spawned worker, but we still
     * run the cJSON marshalling on a dedicated 8 KB task rather than the
     * small AT task. */
    typedef struct { char name[64]; char args[256]; } skill_at_args_t;
    skill_at_args_t *sa = (skill_at_args_t *)malloc(sizeof(*sa));
    if (!sa) { at_printf(ATCMD_ERROR_END_STR, 2); return; }
    strlcpy(sa->name, skill_name, sizeof(sa->name));
    strlcpy(sa->args, args_json,  sizeof(sa->args));
    if (rtos_task_create(NULL, "skill_at", skill_at_task,
                         sa, 8192, 1) != RTK_SUCCESS) {
        free(sa);
        at_printf(ATCMD_ERROR_END_STR, 3);
        return;
    }
    at_printf("\r\n+CLAW:skill,queued=%s\r\n", skill_name);
    at_printf(ATCMD_OK_END_STR);

    (void)arg3;
}

void handle_cmd_tools(const char *arg2)
{
    char *sid = (char *)malloc(64);
    if (!sid) { at_printf(ATCMD_ERROR_END_STR, 1); return; }
    strlcpy(sid, (arg2[0] != '\0') ? arg2 : "serial", 64);
    if (rtos_task_create(NULL, "tools_ls", tools_list_task,
                         sid, 8192, 1) != RTK_SUCCESS) {
        free(sid);
        at_printf(ATCMD_ERROR_END_STR, 2);
        return;
    }
    at_printf(ATCMD_OK_END_STR);
}

void handle_cmd_cap(u16 argc, char **argv, const char *arg2, const char *arg3)
{
    if (arg2[0] == '\0') {
        /* AT+CLAW=cap  →  list all registered capabilities */
        claw_cap_list_t caps = claw_cap_list();
        at_printf("\r\n+CLAW:cap,count=%u\r\n", (unsigned)caps.count);
        for (size_t i = 0; i < caps.count; i++) {
            const claw_cap_descriptor_t *d = &caps.items[i];
            at_printf("+CLAW:cap,[%u],id=%s,family=%s\r\n",
                      (unsigned)i, d->id ? d->id : "?", d->family ? d->family : "?");
        }
        at_printf(ATCMD_OK_END_STR);
    } else {
        /* AT+CLAW=cap,<cap_name>[,<json>][,sid,<session_id>]  →  call cap.
         * The optional trailing ",sid,<id>" lets a caller target a specific
         * session so per-session cap_groups gating (Inc 6) can be exercised
         * from serial. JSON reconstruction stops at the "sid" keyword. */
        const char *session_id = NULL;
        int json_end = argc;
        for (int i = 3; i < argc - 1 && argv[i]; i++) {
            if (strcmp(argv[i], "sid") == 0 && argv[i + 1]) {
                session_id = argv[i + 1];
                json_end = i;
                break;
            }
        }

        char json_buf[512] = "{}";
        if (arg3[0] != '\0' && json_end > 3) {
            size_t pos = 0;
            for (int i = 3; i < json_end && argv[i] && pos < sizeof(json_buf) - 2; i++) {
                if (i > 3) json_buf[pos++] = ',';
                size_t l = strlen(argv[i]);
                if (pos + l >= sizeof(json_buf) - 1) l = sizeof(json_buf) - 1 - pos;
                memcpy(json_buf + pos, argv[i], l);
                pos += l;
            }
            json_buf[pos] = '\0';
        }
        claw_cap_call_context_t ctx = {0};
        ctx.caller = CLAW_CAP_CALLER_MANUAL;
        ctx.session_id = session_id;
        char *output = NULL;
        int rc = claw_cap_call(arg2, json_buf, &ctx, &output);
        if (rc == RTK_SUCCESS && output) {
            at_printf("\r\n+CLAW:cap,%s=%s\r\n", arg2, output);
            at_printf(ATCMD_OK_END_STR);
        } else {
            at_printf("\r\n+CLAW:cap,%s,rc=%d\r\n", arg2, rc);
            if (output && output[0]) at_printf("+CLAW:cap,msg=%s\r\n", output);
            at_printf(ATCMD_ERROR_END_STR, rc ? rc : -1);
        }
        free(output);
    }
}

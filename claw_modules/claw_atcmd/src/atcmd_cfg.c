/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ameba_soc.h"
#include "wifi_fast_connect.h"
#include "atcmd_service.h"
#include "claw_config.h"
#include "claw_wifi_mgr.h"
#include "ameba_claw_defs.h"
#include "atcmd_handlers.h"
#include <string.h>
#include <stdlib.h>

#define TAG "claw_cmd"

/* ---- Config save helper (cJSON+VFS needs more stack than AT task) ---- */

typedef struct {
    char     api_key[256];      /* llm api_key, or (when save_search) the Tavily key */
    char     model[64];
    char     url[256];
    uint8_t  backend;           /* 0xFF = keep current */
    uint32_t compact_tokens;    /* 0 = keep current */
    uint32_t window_tokens;     /* 0 = keep current */
    uint8_t  save_search;       /* 1 = save web-search (Tavily) config, not llm */
    uint8_t  search_max_results;/* web search only; 0 = keep current */
} cfg_save_args_t;

static void cfg_save_task(void *param)
{
    cfg_save_args_t *a = (cfg_save_args_t *)param;
    int rc;

    if (a->save_search) {
        /* api_key holds the Tavily key ("" = clear/disable); max_results 0 = keep */
        rc = claw_config_set_search(a->api_key, a->search_max_results);
    } else {
        int backend = (a->backend == 0xFF) ? -1 : (int)a->backend;
        rc = claw_config_set_llm(a->api_key[0] ? a->api_key : NULL,
                                 a->model[0]   ? a->model   : NULL,
                                 a->url[0]     ? a->url     : NULL,
                                 0, 0, backend,
                                 -1 /* keep thinking_enabled */,
                                 -1 /* keep stream_enabled */,
                                 a->compact_tokens,
                                 a->window_tokens);
    }

    if (rc == 0) {
        RTK_LOGI(TAG, "config saved — reboot to apply\n");
    } else {
        RTK_LOGE(TAG, "config save failed: %d\n", rc);
    }
    free(a);
    rtos_task_delete(NULL);
}

static int cfg_spawn(cfg_save_args_t *a)
{
    if (rtos_task_create(NULL, "cfg_save", cfg_save_task, a,
                         CLAW_ATCMD_CFG_SAVE_STACK, 1) != RTK_SUCCESS) {
        free(a);
        return -1;
    }
    return 0;
}

/* ---- WiFi clear + reboot task ----
 * Must run off the AT task: claw_config_save() uses sscanf() whose frame is
 * ~416 bytes (measured); running on a 4 KB task gives ~1.9 KB headroom.
 */
static void wifi_clr_task(void *arg)
{
    (void)arg;
    wifi_fast_connect_enable(0);
    claw_config_clear_wifi();
    rtos_time_delay_ms(200);
    System_Reset();
    rtos_task_delete(NULL);
}

/* ---- WiFi connect task (cJSON+VFS needs more stack than AT task) ---- */

static void wifi_connect_task(void *p)
{
    char *args = (char *)p;
    const char *ssid = args;
    const char *pass = args + 128;
    claw_config_set_wifi(ssid, pass, NULL);
    int rc = claw_wifi_mgr_connect_sta(ssid, pass);
    RTK_LOGI(TAG, "wifi connect rc=%d\n", rc);
    free(args);
    rtos_task_delete(NULL);
}

/* ---- Handlers ---- */

void handle_cmd_cfg(u16 argc, char **argv, const char *arg2, const char *arg3)
{
    if (arg2[0] == '\0') {
        /* Show config */
        claw_config_t *cfg = claw_config_get();
        at_printf("\r\n+CLAW:cfg,key=%s\r\n",
                  cfg->llm.api_key[0] ? "(set)" : "(empty)");
        at_printf("+CLAW:cfg,model=%s\r\n",
                  cfg->llm.model[0]   ? cfg->llm.model  : "(empty)");
        at_printf("+CLAW:cfg,url=%s\r\n",
                  cfg->llm.api_url[0] ? cfg->llm.api_url : "(empty)");
        at_printf("+CLAW:cfg,backend=%d,max_tokens=%lu\r\n",
                  cfg->llm.backend, (unsigned long)cfg->llm.max_tokens);
        at_printf("+CLAW:cfg,compact_tokens=%lu,window_tokens=%lu\r\n",
                  (unsigned long)cfg->llm.compact_tokens,
                  (unsigned long)cfg->llm.window_tokens);
        at_printf("+CLAW:cfg,search=%s,search_max_results=%u\r\n",
                  cfg->web_search.api_key[0] ? "(set)" : "(empty)",
                  (unsigned)cfg->web_search.max_results);
        at_printf(ATCMD_OK_END_STR);
        return;
    }

    if (arg3[0] == '\0') {
        at_printf("\r\n+CLAW:usage: AT+CLAW=cfg,<key|model|url|backend|search>,<val>\r\n");
        at_printf(ATCMD_ERROR_END_STR, 1);
        return;
    }

    cfg_save_args_t *a = (cfg_save_args_t *)calloc(1, sizeof(*a));
    if (!a) { at_printf(ATCMD_ERROR_END_STR, 2); return; }
    a->backend = 0xFF;

    if (strcmp(arg2, "key") == 0) {
        strlcpy(a->api_key, arg3, sizeof(a->api_key));
    } else if (strcmp(arg2, "model") == 0) {
        strlcpy(a->model, arg3, sizeof(a->model));
    } else if (strcmp(arg2, "url") == 0) {
        strlcpy(a->url, arg3, sizeof(a->url));
    } else if (strcmp(arg2, "backend") == 0) {
        int bd = atoi(arg3);
        if (bd < 0 || bd > 2) {
            free(a);
            at_printf("\r\n+CLAW:backend must be 0, 1, or 2\r\n");
            at_printf(ATCMD_ERROR_END_STR, 4);
            return;
        }
        a->backend = (uint8_t)bd;
    } else if (strcmp(arg2, "compact_tokens") == 0) {
        a->compact_tokens = (uint32_t)strtoul(arg3, NULL, 10);
        if (a->compact_tokens == 0) {
            free(a);
            at_printf("\r\n+CLAW:compact_tokens must be > 0\r\n");
            at_printf(ATCMD_ERROR_END_STR, 4);
            return;
        }
    } else if (strcmp(arg2, "window_tokens") == 0) {
        a->window_tokens = (uint32_t)strtoul(arg3, NULL, 10);
        if (a->window_tokens == 0) {
            free(a);
            at_printf("\r\n+CLAW:window_tokens must be > 0\r\n");
            at_printf(ATCMD_ERROR_END_STR, 4);
            return;
        }
    } else if (strcmp(arg2, "search") == 0) {
        /* AT+CLAW=cfg,search,<tavily_key>[,<max_results>]  ("clear" key disables) */
        a->save_search = 1;
        if (strcmp(arg3, "clear") != 0)
            strlcpy(a->api_key, arg3, sizeof(a->api_key));
        /* else leave api_key empty → claw_config_set_search clears it */
        if (argc >= 5 && argv[4] && argv[4][0]) {
            int mr = atoi(argv[4]);
            if (mr < 1 || mr > 5) {
                free(a);
                at_printf("\r\n+CLAW:search max_results must be 1-5\r\n");
                at_printf(ATCMD_ERROR_END_STR, 4);
                return;
            }
            a->search_max_results = (uint8_t)mr;
        }
    } else if (strcmp(arg2, "wifi") == 0) {
        /* AT+CLAW=cfg,wifi,<ssid>,<password>
         * Connect WiFi immediately using in-memory credentials.
         * claw_config_set_wifi uses cJSON+VFS and needs more stack than the
         * AT task provides, so we spawn a dedicated task (8 KB stack). */
        free(a);
        const char *ssid = arg3;
        const char *pass = (argc >= 5 && argv[4]) ? argv[4] : "";
        if (!ssid || ssid[0] == '\0') {
            at_printf("\r\n+CLAW:usage: AT+CLAW=cfg,wifi,<ssid>,<password>\r\n");
            at_printf(ATCMD_ERROR_END_STR, 1);
            return;
        }
        /* Pass ssid+password via heap; task frees after use */
        char *wifi_args = (char *)malloc(128 + 64);
        if (!wifi_args) { at_printf(ATCMD_ERROR_END_STR, 2); return; }
        strlcpy(wifi_args,       ssid, 128);
        strlcpy(wifi_args + 128, pass, 64);
        if (rtos_task_create(NULL, "wifi_conn", wifi_connect_task,
                             wifi_args, CLAW_ATCMD_CFG_SAVE_STACK, 1) != RTK_SUCCESS) {
            free(wifi_args);
            at_printf(ATCMD_ERROR_END_STR, 3);
            return;
        }
        at_printf("\r\n+CLAW:cfg,wifi,connecting ssid=%s...\r\n", ssid);
        at_printf(ATCMD_OK_END_STR);
        return;
    } else {
        free(a);
        at_printf("\r\n+CLAW:unknown cfg field: %s\r\n", arg2);
        at_printf(ATCMD_ERROR_END_STR, 5);
        return;
    }

    if (cfg_spawn(a) != 0) { at_printf(ATCMD_ERROR_END_STR, 3); return; }
    at_printf(ATCMD_OK_END_STR);
}

void handle_cmd_wifi(const char *arg2)
{
    if (strcmp(arg2, "clear") == 0) {
        RTK_LOGA(NOTAG, "[claw] clearing WiFi config (same as long-press)...\r\n");
        RRAM_DEV->RRAM_USER_RSVD[0] = 0;
        if (rtos_task_create(NULL, "wifi_clr", wifi_clr_task,
                             NULL, CLAW_ATCMD_WIFI_CLR_STACK, 1) != RTK_SUCCESS) {
            at_printf(ATCMD_ERROR_END_STR, 1);
            return;
        }
        at_printf(ATCMD_OK_END_STR);
    } else {
        claw_wifi_state_t state = claw_wifi_mgr_get_state();
        const char *ip = claw_wifi_mgr_get_sta_ip();
        bool ap_on = claw_wifi_mgr_is_softap_running();
        claw_config_t *cfg = claw_config_get();
        at_printf("\r\n+CLAW:wifi,state=%d,ssid=%s,ip=%s,softap=%s\r\n",
                  (int)state,
                  cfg->wifi.ssid[0] ? cfg->wifi.ssid : "(none)",
                  ip ? ip : "0.0.0.0",
                  ap_on ? "ON" : "OFF");
        at_printf(ATCMD_OK_END_STR);
    }
}

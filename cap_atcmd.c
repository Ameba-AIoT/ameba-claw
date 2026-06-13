/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AT+CLAW — ameba_claw serial command interface
 *
 *   AT+CLAW=ask,<message>              Submit message to LLM (serial session)
 *   AT+CLAW=ask,<message>,sid,<id>     Submit with custom session_id (isolation test)
 *   AT+CLAW=lua                        Enter Lua REPL (exit() to return)
 *   AT+CLAW=cfg                        Show LLM configuration
 *   AT+CLAW=cfg,key,<val>              Set API key
 *   AT+CLAW=cfg,model,<val>            Set model
 *   AT+CLAW=cfg,url,<val>              Set API URL
 *   AT+CLAW=cfg,backend,<0|1|2>        Set backend (0=bearer 1=x-api-key 2=anthropic)
 *   AT+CLAW=wifi                       Show WiFi status and IP
 *   AT+CLAW=wifi,clear                 Clear WiFi config and reboot
 *   AT+CLAW=wechat,reset               Reset WeChat, trigger QR re-login
 *   AT+CLAW=cap                        List all registered capabilities
 *   AT+CLAW=cap,<name>[,<json>][,sid,<id>] Call a cap directly (optional session)
 *   AT+CLAW=tools[,<session_id>]       List LLM-visible tool names for a session
 *   AT+CLAW=cfg,wifi,<ssid>,<password> Connect WiFi immediately (in-memory, no VFS needed)
 *   AT+CLAW=skill,<name>[,<args_json>] Run a Lua skill directly (no LLM required)
 *   AT+CLAW=session,list               List all session history files
 *   AT+CLAW=session,clear              Clear serial session history
 *   AT+CLAW=session,clear,all          Clear ALL session history files
 *   AT+CLAW=session,reset[,ch,id]      Soft reset: bump version, keep history (default ch=serial id=atcmd)
 *   AT+CLAW=memory,list                List all long-term memories
 *   AT+CLAW=memory,clear               Clear all long-term memories
 *   AT+CLAW=fs,list                    List all files in VFS root
 *   AT+CLAW=fs,write                   Write test file
 *   AT+CLAW=fs,read                    Read test file
 *   AT+CLAW=fs,test                    Full write+read+verify cycle
 *   AT+CLAW=fs,delete,<path>           Delete file at path
 *   AT+CLAW=i2c,sh1106                 Run SH1106 OLED I2C test
 *   AT+CLAW=spi,<poll|intr|dma>        Run SPI hardware test
 *   AT+CLAW=ir,<tx|rx>                 Run IR hardware test
 *   AT+CLAW=rtc[,test]                Run RTC set_time/alarm test
 *   AT+CLAW=pwm                        Run PWM test (PA_6, TIM4 ch0)
 *   AT+CLAW=adc                        Run ADC loopback test (PA_13 <-> PA_25 wired)
 *   AT+CLAW=adc,ext                    Run ADC external supply test (supply on PA_13)
 *   AT+CLAW=thermal                    Run on-chip thermal sensor test
 *   AT+CLAW=touch                      Run capacitive touch interactive test
 *   AT+CLAW=touch,ext                  Run capacitive touch external trigger test
 *   AT+CLAW=gpio                       Run GPIO interrupt test (PA30->PA31)
 *   AT+CLAW=lcdc,rgb,st7262            Run LCDC RGB st7262 (800x480) colour-fill test
 *   AT+CLAW=lcdc,srgb,st7272a          Run LCDC SRGB st7272a (320x240) colour-fill test
 *   AT+CLAW=lcdc,mcu,ili9806           Run LCDC MCU ILI9806 (480x800) colour-fill test
 *   AT+CLAW=speaker[,sine|nokia]        Run speaker test; sine=1kHz only, nokia=wav only, default=allv via MAX98357A)
 *   AT+CLAW=dmic[,<vol>]               Run DMIC SNR/THD test; vol=speaker volume 0.0-1.0 (default 0.4)
 *   AT+CLAW=rec[,<path>[,<ms>]]        Record DMIC to WAV; default path=vfs:rec.wav, duration=5000ms
 *   AT+CLAW=play[,<path>]              Play WAV file; default path=vfs:rec.wav
 *   AT+CLAW=usb,uvc                    Capture one JPEG frame from USB UVC camera, save to vfs:capture.jpg
 *   AT+CLAW=usb,list[,<path>]          List U-disk directory (default: root)
 *   AT+CLAW=usb,write,<path>,<data>    Write data to file on U-disk (create/overwrite)
 *   AT+CLAW=usb,read,<path>            Read and print file content from U-disk
 *   AT+CLAW=usb,delete,<path>          Delete file from U-disk
 *   AT+CLAW=sys,tasks                  List all FreeRTOS tasks with state, priority and stack watermark
 *   AT+CLAW=test[,<cap|mem|router|fs>] Run unit tests (CLAW_BUILD_TESTS)
 */

#include "ameba_soc.h"
#include "wifi_fast_connect.h"
#include "atcmd_service.h"
#include "claw_agent.h"
#include "claw_im_dispatch.h"
#include "claw_config.h"
#include "claw_cap.h"
#include "ameba_claw_defs.h"
#include "claw_memory.h"
#include "claw_wifi_mgr.h"
#include "lua_modules_config.h"   /* LUA_MOD_ENABLE_USB_UVC / _USB_MSC */

#define LUA_USB_ENABLED (LUA_MOD_ENABLE_USB_UVC || LUA_MOD_ENABLE_USB_MSC)
#include "cap_session_mgr.h"
#include "cap_im_wechat.h"
#ifdef CLAW_BUILD_TESTS
#include "claw_test.h"
#endif
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "vfs.h"
#include <cJSON.h>
#include "FreeRTOS.h"
#include "task.h"

static void skill_at_task(void *p);

#define TAG "claw_cmd"

extern void lua_run_repl_once(void);
#if LUA_DRIVER_TESTS_ENABLED
extern void lua_i2c_run_sh1106(int sx, int sy);
extern void lua_i2c_run_rw(void);
extern void lua_i2c_run_slave(void);
extern void lua_spi_run(const char *mode);
extern void lua_ir_run(const char *mode);
extern void lua_rtc_run(const char *mode);
extern void lua_pwm_run(void);
extern void lua_gpio_run(void);
extern void lua_speaker_run(const char *mode, const char *vol);
extern void lua_lcdc_run(const char *if_mode, const char *panel);
extern void lua_dmic_run(const char *vol);
extern void lua_audio_rec_run(const char *path, int duration_ms);
extern void lua_audio_play_run(const char *path);
extern void lua_adc_run(const char *mode);
extern void lua_thermal_run(void);
extern void lua_touch_run(const char *mode);
#if LUA_USB_ENABLED
extern void lua_uvc_run(void);
extern void lua_msc_run(void);
extern void lua_msc_list_run(const char *path);
extern void lua_msc_write_run(const char *path, const char *data);
extern void lua_msc_read_run(const char *path);
extern void lua_msc_delete_run(const char *path);
#endif
#endif /* LUA_DRIVER_TESTS_ENABLED */
extern void swtimer_stop_all(void);

static uint32_t s_serial_req_id = 0;

/* ---- Serial IM channel ---- */

static void serial_im_send(const char *chat_id, const char *text)
{
    (void)chat_id;
    if (text) {
        at_printf("\r\n+CLAW:%s\r\n", text);
    }
}

/* ---- Config save helper (cJSON+VFS needs more stack than AT task) ---- */

typedef struct {
    char     api_key[256];
    char     model[64];
    char     url[256];
    uint8_t  backend;           /* 0xFF = keep current */
    uint32_t compact_tokens;    /* 0 = keep current */
    uint32_t window_tokens;     /* 0 = keep current */
} cfg_save_args_t;

static void cfg_save_task(void *param)
{
    cfg_save_args_t *a = (cfg_save_args_t *)param;
    int backend = (a->backend == 0xFF) ? -1 : (int)a->backend;
    int rc = claw_config_set_llm(a->api_key[0] ? a->api_key : NULL,
                                 a->model[0]   ? a->model   : NULL,
                                 a->url[0]     ? a->url     : NULL,
                                 0, 0, backend,
                                 -1 /* keep thinking_enabled */,
                                 -1 /* keep stream_enabled */,
                                 a->compact_tokens,
                                 a->window_tokens);

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
    if (rtos_task_create(NULL, "cfg_save", cfg_save_task, a, 8192, 1) != RTK_SUCCESS) {
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

/* ---- WeChat reset background task (needs ~16 KB for TLS) ---- */

static void wechat_reset_task(void *arg)
{
    (void)arg;
    char qr_url[256] = {0};

    int rc = cap_im_wechat_get_qr(qr_url, sizeof(qr_url));
    if (rc == 0 && qr_url[0]) {
        RTK_LOGI(TAG, "[wechat] QR ready — scan to login:\n%s\n", qr_url);
        at_printf("\r\n+CLAW:wechat,qr=%s\r\n", qr_url);
    } else {
        RTK_LOGE(TAG, "[wechat] get_qr failed: %d\n", rc);
        at_printf("\r\n+CLAW:wechat,error=%d\r\n", rc);
    }
    rtos_task_delete(NULL);
}

/* ---- WiFi connect task (cJSON+VFS needs more stack than AT task) ---- */

void wifi_connect_task(void *p)
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

/* ---- AT+CLAW main handler ---- */

static void at_claw(u16 argc, char **argv)
{
    const char *sub  = (argc >= 2 && argv[1]) ? argv[1] : "";
    const char *arg2 = (argc >= 3 && argv[2]) ? argv[2] : "";
    const char *arg3 = (argc >= 4 && argv[3]) ? argv[3] : "";
    const char *arg4 = (argc >= 5 && argv[4]) ? argv[4] : "";
    (void)arg4; /* suppress unused warning when USB modules not enabled */

    /* ---- ask ---- */
    if (strcmp(sub, "ask") == 0) {
        if (arg2[0] == '\0') {
            at_printf("\r\n+CLAW:usage: AT+CLAW=ask,<message>[,sid,<session_id>]\r\n");
            at_printf(ATCMD_ERROR_END_STR, 1);
            return;
        }

        /* Reconstruct message from argv[2..n], restoring commas that
         * parse_param_advance replaced with '\0'.  argv pointers are
         * into a single contiguous buffer, so the original comma sat at
         * (argv[i+1] - 1) — just write it back.
         *
         * Also detect optional trailing ",sid,<id>" suffix: walk from
         * the end looking for the "sid" keyword and strip it off. */
        const char *session_id = "serial";

        /* Patch '\0' separators back to ',' in the argv buffer so that
         * argv[2] .. argv[argc-1] form one continuous C-string again. */
        for (int i = 2; i < argc - 1 && argv[i] && argv[i + 1]; i++) {
            /* The char between argv[i]'s end and argv[i+1]'s start was ','. */
            char *sep = argv[i] + strlen(argv[i]);
            *sep = ',';
        }
        /* Now argv[2] is the whole message (commas restored).
         * Check for trailing ,sid,<id> and strip it. */
        char *raw = (char *)arg2;   /* arg2 == argv[2] */
        char *sid_pos = NULL;
        {
            /* Search backwards for ",sid," pattern */
            size_t rlen = strlen(raw);
            if (rlen > 5) {
                char *p = raw + rlen - 1;
                while (p > raw + 4) {
                    if (strncmp(p - 4, ",sid,", 5) == 0) {
                        sid_pos = p - 4;
                        break;
                    }
                    p--;
                }
            }
        }
        if (sid_pos) {
            session_id = sid_pos + 5;  /* skip ",sid," */
            *sid_pos = '\0';            /* truncate message */
        }

        char *msg = malloc(1024);
        if (!msg) {
            at_printf(ATCMD_ERROR_END_STR, 1);
            return;
        }
        strlcpy(msg, raw, 1024);

        claw_agent_response_t resp = {0};
        claw_agent_request_t req = {
            .request_id     = ++s_serial_req_id,
            .flags          = CLAW_AGENT_REQUEST_FLAG_SYNC_RECEIVE,
            .session_id     = session_id,
            .user_text      = msg,
            .source_channel = "serial",
            .source_chat_id = session_id,
        };

        at_printf("\r\n+CLAW:ask,session=%s\r\n", session_id);
        at_printf("+CLAW:%s\r\n", CLAW_IM_ACK_MSG);
        int rc = claw_agent_submit(&req, 5000);
        free(msg);
        if (rc == RTK_SUCCESS) {
            rc = claw_agent_receive_for(req.request_id, &resp, 300000);
        }
        if (rc != RTK_SUCCESS) {
            RTK_LOGA(NOTAG, "[claw] ask submit failed: %d\r\n", rc);
            at_printf(ATCMD_ERROR_END_STR, 2);
            return;
        }
        if (resp.status == CLAW_AGENT_RESPONSE_STATUS_OK && resp.text) {
            at_printf("+CLAW:%s\r\n", resp.text);
            at_printf(ATCMD_OK_END_STR);
        } else {
            RTK_LOGA(NOTAG, "[claw] ask error: %s\r\n",
                     resp.error_message ? resp.error_message : "unknown");
            at_printf(ATCMD_ERROR_END_STR, 3);
        }
        claw_agent_response_free(&resp);
        return;
    }

    /* ---- lua ---- */
    if (strcmp(sub, "lua") == 0) {
        RTK_LOGA(NOTAG, "[claw] entering Lua REPL — type exit() to return\r\n");
        lua_run_repl_once();
        RTK_LOGA(NOTAG, "[claw] Lua REPL exited\r\n");
        at_printf(ATCMD_OK_END_STR);
        return;
    }

    /* ---- cfg ---- */
    if (strcmp(sub, "cfg") == 0) {
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
            at_printf(ATCMD_OK_END_STR);
            return;
        }

        if (arg3[0] == '\0') {
            at_printf("\r\n+CLAW:usage: AT+CLAW=cfg,<key|model|url|backend>,<val>\r\n");
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
                                 wifi_args, 8192, 1) != RTK_SUCCESS) {
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
        return;
    }

    /* ---- skill — run a Lua skill directly without the LLM agent ---- */
    if (strcmp(sub, "skill") == 0) {
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
        return;
    }

    /* ---- wifi ---- */
    if (strcmp(sub, "wifi") == 0) {
        if (strcmp(arg2, "clear") == 0) {
            RTK_LOGA(NOTAG, "[claw] clearing WiFi config (same as long-press)...\r\n");
            RRAM_DEV->RRAM_USER_RSVD[0] = 0;
            if (rtos_task_create(NULL, "wifi_clr", wifi_clr_task,
                                 NULL, 4096, 1) != RTK_SUCCESS) {
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
        return;
    }

    /* ---- wechat ---- */
    if (strcmp(sub, "wechat") == 0) {
        if (strcmp(arg2, "reset") != 0) {
            at_printf("\r\n+CLAW:usage: AT+CLAW=wechat,reset\r\n");
            at_printf(ATCMD_ERROR_END_STR, 1);
            return;
        }
        if (rtos_task_create(NULL, "wx_reset", wechat_reset_task,
                             NULL, 16384, 1) != RTK_SUCCESS) {
            at_printf(ATCMD_ERROR_END_STR, 2);
            return;
        }
        at_printf("\r\n+CLAW:wechat,triggered\r\n");
        at_printf(ATCMD_OK_END_STR);
        return;
    }

    /* ---- session ---- */
    if (strcmp(sub, "session") == 0) {
        if (strcmp(arg2, "list") == 0) {
            /* List session files — spawn task (VFS opendir needs more stack) */
            void list_session_task(void *p);
            if (rtos_task_create(NULL, "ses_list", list_session_task,
                                 NULL, 4096, 1) != RTK_SUCCESS) {
                at_printf(ATCMD_ERROR_END_STR, 1);
            } else {
                at_printf(ATCMD_OK_END_STR);
            }
        } else if (strcmp(arg2, "clear") == 0) {
            const char *arg3_local = (argc >= 4 && argv[3]) ? argv[3] : "";
            /* "1" = clear serial only, "2" = clear all */
            int *flag = (int *)rtos_mem_malloc(sizeof(int));
            if (flag) *flag = (strcmp(arg3_local, "all") == 0) ? 2 : 1;
            void session_clear_task(void *p);
            if (!flag || rtos_task_create(NULL, "ses_clr", session_clear_task,
                                          flag, 4096, 1) != RTK_SUCCESS) {
                rtos_mem_free(flag);
                at_printf(ATCMD_ERROR_END_STR, 1);
            } else {
                at_printf(ATCMD_OK_END_STR);
            }
        } else if (strcmp(arg2, "reset") == 0) {
            /* AT+CLAW=session,reset[,channel,chat_id] — soft reset:
             * bump the version so a new session_id is issued, but keep
             * old history files on disk. With no channel/chat_id, default
             * to "serial" channel. */
            const char *channel = (argc >= 4 && argv[3] && argv[3][0]) ? argv[3] : "serial";
            const char *chat_id = (argc >= 5 && argv[4] && argv[4][0]) ? argv[4] : "atcmd";
            int rc = cap_session_mgr_bump_version(channel, chat_id);
            if (rc == RTK_SUCCESS) {
                at_printf("\r\n+CLAW:session,reset,%s,%s\r\n", channel, chat_id);
                at_printf(ATCMD_OK_END_STR);
            } else {
                at_printf(ATCMD_ERROR_END_STR, 1);
            }
        } else {
            at_printf("\r\n+CLAW:usage: AT+CLAW=session,<list|clear[,all]|reset[,channel,chat_id]>\r\n");
            at_printf(ATCMD_ERROR_END_STR, 1);
        }
        return;
    }

    /* ---- memory (spawn task — reads/writes VFS) ---- */
    if (strcmp(sub, "memory") == 0) {
        if (strcmp(arg2, "list") == 0 || strcmp(arg2, "clear") == 0) {
            /* "1"=list, "2"=clear */
            int *flag = (int *)rtos_mem_malloc(sizeof(int));
            if (flag) *flag = (strcmp(arg2, "clear") == 0) ? 2 : 1;
            void memory_op_task(void *p);
            if (!flag || rtos_task_create(NULL, "mem_op", memory_op_task,
                                          flag, 5120, 1) != RTK_SUCCESS) {
                rtos_mem_free(flag);
                at_printf(ATCMD_ERROR_END_STR, 1);
            } else {
                at_printf(ATCMD_OK_END_STR);
            }
        } else {
            at_printf("\r\n+CLAW:usage: AT+CLAW=memory,<list|clear>\r\n");
            at_printf(ATCMD_ERROR_END_STR, 1);
        }
        return;
    }

    /* ---- cap ----
     *
     * Output format notes:
     *   - We use "#<idx>" as the entry marker, not "[<idx>]". The SDK's
     *     `at_printf` is a thin wrapper over the platform vsnprintf; in
     *     this build it actually supports both, but external monitor
     *     wrappers may treat '[' as a pattern delimiter so '#' is more
     *     portable. Same convention is used by list_session_task.
     *   - We cast size_t → unsigned and use "%u". The actual root cause
     *     of the previous "+CLAW:cap,#" with no index in the output was
     *     "%zu" being unsupported by the build's newlib-nano vsnprintf
     *     — it short-circuits the format string on the first '%z' and
     *     produces an empty index. Always cast or use "%u" for size_t
     *     in any at_printf. */
    /* ---- tools — list LLM-visible tool names for a session (Inc 6) ----
     * AT+CLAW=tools[,<session_id>]  →  spawn a task that builds the tools JSON
     * for the given session (default "serial") and prints each function name.
     * This makes per-session cap_groups gating directly observable. */
    if (strcmp(sub, "tools") == 0) {
        void tools_list_task(void *p);
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
        return;
    }

    if (strcmp(sub, "cap") == 0) {
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
        return;
    }

#if LUA_DRIVER_TESTS_ENABLED
    /* ---- i2c ---- */
    if (strcmp(sub, "i2c") == 0) {
        if (strcmp(arg2, "sh1106") == 0) {
            /* AT+CLAW=i2c,sh1106[,sx[,sy]] — optional font scale.
             * sx/sy default to 0 (= unspecified); the Lua script then
             * applies its own default look (sx=1, sy=2). */
            int sx = (argc >= 4 && argv[3] && argv[3][0]) ? atoi(argv[3]) : 0;
            int sy = (argc >= 5 && argv[4] && argv[4][0]) ? atoi(argv[4]) : 0;
            swtimer_stop_all();
            rtos_time_delay_ms(100);
            at_printf("\r\n+CLAW:i2c,running sh1106 test (sx=%d sy=%d)...\r\n", sx, sy);
            lua_i2c_run_sh1106(sx, sy);
            at_printf(ATCMD_OK_END_STR);
        } else if (strcmp(arg2, "rw") == 0) {
            at_printf("\r\n+CLAW:i2c,master rw test (PA_26=SDA PA_25=SCL slave=0x50)...\r\n");
            at_printf("+CLAW:i2c,make sure slave board runs AT+CLAW=i2c,slave first\r\n");
            lua_i2c_run_rw();
            at_printf(ATCMD_OK_END_STR);
        } else if (strcmp(arg2, "slave") == 0) {
            at_printf("\r\n+CLAW:i2c,slave mode addr=0x50 (PA_25=SCL PA_26=SDA), waiting for master...\r\n");
            lua_i2c_run_slave();
            at_printf(ATCMD_OK_END_STR);
        } else {
            at_printf("\r\n+CLAW:usage: AT+CLAW=i2c,<sh1106|rw|slave>\r\n");
            at_printf("+CLAW:  sh1106[,sx[,sy]] — OLED demo; font scale (default 1,2)\r\n");
            at_printf("+CLAW:  rw    — master test (COM6), run slave on COM13 first\r\n");
            at_printf("+CLAW:  slave — slave mode (COM13), PA_25=SCL PA_26=SDA addr=0x50\r\n");
            at_printf(ATCMD_ERROR_END_STR, 1);
        }
        return;
    }

    /* ---- spi ---- */
    if (strcmp(sub, "spi") == 0) {
        if (arg2[0] == '\0') {
            at_printf("\r\n+CLAW:usage: AT+CLAW=spi,<poll|intr|dma>\r\n");
            at_printf(ATCMD_ERROR_END_STR, 1);
            return;
        }
        at_printf("\r\n+CLAW:spi,running %s test...\r\n", arg2);
        lua_spi_run(arg2);
        at_printf(ATCMD_OK_END_STR);
        return;
    }

    /* ---- ir ---- */
    if (strcmp(sub, "ir") == 0) {
        if (arg2[0] == '\0') {
            at_printf("\r\n+CLAW:usage: AT+CLAW=ir,<tx|tx,poll|tx,intr|rx>\r\n");
            at_printf(ATCMD_ERROR_END_STR, 1);
            return;
        }
        char ir_mode[32];
        if (arg3[0]) {
            snprintf(ir_mode, sizeof(ir_mode), "%s,%s", arg2, arg3);
        } else {
            strlcpy(ir_mode, arg2, sizeof(ir_mode));
        }
        at_printf("\r\n+CLAW:ir,running %s test...\r\n", ir_mode);
        lua_ir_run(ir_mode);
        at_printf(ATCMD_OK_END_STR);
        return;
    }

    /* ---- rtc ---- */
    if (strcmp(sub, "rtc") == 0) {
        const char *mode = arg2[0] ? arg2 : "test";
        at_printf("\r\n+CLAW:rtc,running %s test...\r\n", mode);
        lua_rtc_run(mode);
        at_printf(ATCMD_OK_END_STR);
        return;
    }

    /* ---- pwm ---- */
    if (strcmp(sub, "pwm") == 0) {
        at_printf("\r\n+CLAW:pwm,running test...\r\n");
        lua_pwm_run();
        at_printf(ATCMD_OK_END_STR);
        return;
    }

    /* ---- adc ---- */
    if (strcmp(sub, "adc") == 0) {
        const char *mode = (strcmp(arg2, "ext") == 0) ? "ext_supply" : "loopback";
        at_printf("\r\n+CLAW:adc,mode=%s,running...\r\n", mode);
        lua_adc_run(mode);
        at_printf(ATCMD_OK_END_STR);
        return;
    }

    /* ---- gpio ---- */
    if (strcmp(sub, "gpio") == 0) {
        at_printf("\r\n+CLAW:gpio,running interrupt test (PA30->PA31)...\r\n");
        lua_gpio_run();
        at_printf(ATCMD_OK_END_STR);
        return;
    }

    /* ---- thermal ---- */
    if (strcmp(sub, "thermal") == 0) {
        at_printf("\r\n+CLAW:thermal,running test...\r\n");
        lua_thermal_run();
        at_printf(ATCMD_OK_END_STR);
        return;
    }

    /* ---- touch ---- */
    if (strcmp(sub, "touch") == 0) {
        const char *mode = (strcmp(arg2, "ext") == 0) ? "ext" : "interactive";
        at_printf("\r\n+CLAW:touch,mode=%s,running...\r\n", mode);
        lua_touch_run(mode);
        at_printf(ATCMD_OK_END_STR);
        return;
    }

    /* ---- lcdc ---- */
    if (strcmp(sub, "lcdc") == 0) {
        const char *if_mode = arg2[0] ? arg2 : "";
        const char *panel   = arg3[0] ? arg3 : "";
        if (if_mode[0] == '\0' || panel[0] == '\0') {
            at_printf("\r\n+CLAW:usage: AT+CLAW=lcdc,<rgb,st7262|srgb,st7272a|mcu,ili9806> \r\n");
            at_printf(ATCMD_ERROR_END_STR, 1);
            return;
        }
        at_printf("\r\n+CLAW:lcdc,running %s %s test...\r\n", if_mode, panel);
        lua_lcdc_run(if_mode, panel);
        at_printf(ATCMD_OK_END_STR);
        return;
    }

    /* ---- speaker ---- */
    if (strcmp(sub, "speaker") == 0) {
        const char *mode = arg2[0] ? arg2 : "all";
        const char *vol  = arg3[0] ? arg3 : "";
        at_printf("\r\n+CLAW:speaker,running test (mode=");
        at_printf(mode);
        at_printf(")...\r\n");
        lua_speaker_run(mode, vol);
        at_printf(ATCMD_OK_END_STR);
        return;
    }

    /* ---- dmic ---- */
    if (strcmp(sub, "dmic") == 0) {
        const char *vol = arg2[0] ? arg2 : "0.2";
        at_printf("\r\n+CLAW:dmic,running SNR/THD test (vol=");
        at_printf(vol);
        at_printf(")...\r\n");
        lua_dmic_run(vol);
        at_printf(ATCMD_OK_END_STR);
        return;
    }

    /* ---- rec: record DMIC to WAV ---- */
    if (strcmp(sub, "rec") == 0) {
        const char *path = arg2[0] ? arg2 : "vfs:rec.wav";
        int duration_ms  = arg3[0] ? atoi(arg3) : 5000;
        if (duration_ms <= 0) duration_ms = 5000;
        at_printf("\r\n+CLAW:rec,recording %d ms -> %s\r\n", duration_ms, path);
        lua_audio_rec_run(path, duration_ms);
        at_printf(ATCMD_OK_END_STR);
        return;
    }

    /* ---- play: play WAV file ---- */
    if (strcmp(sub, "play") == 0) {
        const char *path = arg2[0] ? arg2 : "vfs:rec.wav";
        at_printf("\r\n+CLAW:play,playing %s\r\n", path);
        lua_audio_play_run(path);
        at_printf(ATCMD_OK_END_STR);
        return;
    }

    /* ---- usb ---- */
    if (strcmp(sub, "usb") == 0) {
#if LUA_USB_ENABLED
        if (strcmp(arg2, "uvc") == 0) {
            at_printf("\r\n+CLAW:usb,uvc,capturing frame...\r\n");
            lua_uvc_run();
            at_printf(ATCMD_OK_END_STR);

        } else if (strcmp(arg2, "list") == 0) {
            /* AT+CLAW=usb,list[,<path>] */
            at_printf("\r\n+CLAW:usb,list,%s\r\n", arg3[0] ? arg3 : "(root)");
            lua_msc_list_run(arg3[0] ? arg3 : "");
            at_printf(ATCMD_OK_END_STR);

        } else if (strcmp(arg2, "write") == 0) {
            /* AT+CLAW=usb,write,<path>,<data[,data...]> */
            const char *arg4 = (argc >= 5 && argv[4]) ? argv[4] : "";
            if (!arg3[0]) {
                at_printf("\r\n+CLAW:usage: AT+CLAW=usb,write,<path>,<data>\r\n");
                at_printf(ATCMD_ERROR_END_STR, 1);
            } else if (!arg4[0]) {
                at_printf("\r\n+CLAW:usage: AT+CLAW=usb,write,<path>,<data>\r\n");
                at_printf(ATCMD_ERROR_END_STR, 1);
            } else {
                /* Join argv[4..] with "," to allow commas inside data */
                char wbuf[512] = {0};
                strlcpy(wbuf, arg4, sizeof(wbuf));
                for (int i = 5; i < argc && argv[i]; i++) {
                    strlcat(wbuf, ",", sizeof(wbuf));
                    strlcat(wbuf, argv[i], sizeof(wbuf));
                }
                at_printf("\r\n+CLAW:usb,write,%s\r\n", arg3);
                lua_msc_write_run(arg3, wbuf);
                at_printf(ATCMD_OK_END_STR);
            }

        } else if (strcmp(arg2, "read") == 0) {
            /* AT+CLAW=usb,read,<path> */
            if (!arg3[0]) {
                at_printf("\r\n+CLAW:usage: AT+CLAW=usb,read,<path>\r\n");
                at_printf(ATCMD_ERROR_END_STR, 1);
            } else {
                at_printf("\r\n+CLAW:usb,read,%s\r\n", arg3);
                lua_msc_read_run(arg3);
                at_printf(ATCMD_OK_END_STR);
            }

        } else if (strcmp(arg2, "delete") == 0) {
            /* AT+CLAW=usb,delete,<path> */
            if (!arg3[0]) {
                at_printf("\r\n+CLAW:usage: AT+CLAW=usb,delete,<path>\r\n");
                at_printf(ATCMD_ERROR_END_STR, 1);
            } else {
                at_printf("\r\n+CLAW:usb,delete,%s\r\n", arg3);
                lua_msc_delete_run(arg3);
                at_printf(ATCMD_OK_END_STR);
            }

        } else {
            at_printf("\r\n+CLAW:usage: AT+CLAW=usb,<uvc|list|write|read|delete>\r\n");
            at_printf(ATCMD_ERROR_END_STR, 1);
        }
#else
        at_printf("\r\n+CLAW:usb not enabled in this build\r\n");
        at_printf(ATCMD_ERROR_END_STR, 1);
#endif
        return;
    }
#endif /* LUA_DRIVER_TESTS_ENABLED */

    /* ---- fs (always available: list, delete) ---- */
    if (strcmp(sub, "fs") == 0) {
        const char *op = arg2[0] ? arg2 : "list";

        if (strcmp(op, "list") == 0) {
            void list_vfs_task(void *p);
            if (rtos_task_create(NULL, "vfs_list", list_vfs_task,
                                 NULL, 4096, 1) != RTK_SUCCESS) {
                at_printf(ATCMD_ERROR_END_STR, 1);
            } else {
                at_printf(ATCMD_OK_END_STR);
            }
            return;
        }

        if (strcmp(op, "delete") == 0) {
            const char *path = arg3[0] ? arg3 : "";
            if (!path[0]) {
                at_printf("\r\n+CLAW:usage: AT+CLAW=fs,delete,<path>\r\n");
                at_printf(ATCMD_ERROR_END_STR, 1);
                return;
            }
            /* spawn task — remove() calls LittleFS which needs more stack */
            char *path_copy = (char *)rtos_mem_malloc(128);
            if (path_copy) strlcpy(path_copy, path, 128);
            void fs_delete_task(void *p);
            if (!path_copy || rtos_task_create(NULL, "fs_del", fs_delete_task,
                                               path_copy, 4096, 1) != RTK_SUCCESS) {
                rtos_mem_free(path_copy);
                at_printf(ATCMD_ERROR_END_STR, 1);
            } else {
                at_printf(ATCMD_OK_END_STR);
            }
            return;
        }
        /* write / read / test — spawn task (VFS needs more stack than AT task) */
#define CLAWFS_PATH "vfs:/clawfs_test.txt"
#define CLAWFS_DATA "CLAWFS_OK"
        if (strcmp(op, "write") == 0 || strcmp(op, "read") == 0 || strcmp(op, "test") == 0) {
            /* Pass op string via a small heap alloc */
            char *op_copy = (char *)rtos_mem_malloc(8);
            if (op_copy) strlcpy(op_copy, op, 8);
            void fs_rw_task(void *p);
            if (rtos_task_create(NULL, "fs_rw", fs_rw_task,
                                 op_copy, 4096, 1) != RTK_SUCCESS) {
                rtos_mem_free(op_copy);
                at_printf(ATCMD_ERROR_END_STR, 1);
            } else {
                at_printf(ATCMD_OK_END_STR);
            }
            return;
        }
        at_printf("\r\n+CLAW:fs usage: list,write,read,test,delete,<path>\r\n");
        at_printf(ATCMD_ERROR_END_STR, 4);
        return;
    }

#ifdef CLAW_BUILD_TESTS
    /* ---- test (unit tests) ---- */
    if (strcmp(sub, "test") == 0) {
        const char *suite = arg2[0] ? arg2 : "all";
        char *buf = (char *)malloc(4096);
        if (!buf) { at_printf(ATCMD_ERROR_END_STR, 1); return; }
        buf[0] = '\0';
        int fails = 0;
        if      (strcmp(suite, "cap")    == 0) fails = claw_test_cap(buf, 4096);
        else if (strcmp(suite, "mem")    == 0) fails = claw_test_mem(buf, 4096);
        else if (strcmp(suite, "router") == 0) fails = claw_test_router(buf, 4096);
        else if (strcmp(suite, "fs")     == 0) fails = claw_test_fs(buf, 4096);
        else                                    fails = claw_test_all(buf, 4096);
        at_printf("\r\n%s", buf);
        free(buf);
        if (fails == 0) at_printf(ATCMD_OK_END_STR);
        else            at_printf(ATCMD_ERROR_END_STR, fails);
        return;
    }
#endif /* CLAW_BUILD_TESTS */

    /* ---- sys ---- */
    if (strcmp(sub, "sys") == 0) {
        if (strcmp(arg2, "tasks") == 0) {
            UBaseType_t n = uxTaskGetNumberOfTasks();
            TaskStatus_t *arr = rtos_mem_malloc(n * sizeof(TaskStatus_t));
            if (!arr) { at_printf(ATCMD_ERROR_END_STR, 1); return; }
            UBaseType_t filled = uxTaskGetSystemState(arr, n, NULL);
            char line[80];
            snprintf(line, sizeof(line), "%-16s %-9s PRI  STK_FREE", "TASK", "STATE");
            at_printf("\r\n+CLAW:%s\r\n", line);
            for (UBaseType_t i = 0; i < filled; i++) {
                const char *st;
                switch (arr[i].eCurrentState) {
                case eRunning:   st = "running";   break;
                case eReady:     st = "ready";     break;
                case eBlocked:   st = "blocked";   break;
                case eSuspended: st = "suspended"; break;
                default:         st = "deleted";   break;
                }
                snprintf(line, sizeof(line), "%-16s %-9s %3u  %u bytes",
                         arr[i].pcTaskName, st,
                         (unsigned)arr[i].uxCurrentPriority,
                         (unsigned)arr[i].usStackHighWaterMark * 4);
                at_printf("+CLAW:%s\r\n", line);
            }
            rtos_mem_free(arr);
            at_printf(ATCMD_OK_END_STR);
        } else {
            at_printf("\r\n+CLAW:usage: sys,tasks\r\n");
            at_printf(ATCMD_ERROR_END_STR, 4);
        }
        return;
    }

    /* ---- unknown ---- */
    at_printf("\r\n+CLAW:unknown: %s  try: ask,lua,cfg,wifi,wechat,cap,"
              "session,memory,fs,i2c,spi,rtc,pwm,ir,adc,thermal,touch,lcdc,gpio,usb,sys,speaker,dmic\r\n", sub[0] ? sub : "(none)");
    at_printf(ATCMD_ERROR_END_STR, 99);
}

/* ---- Background tasks for VFS operations (all need more stack than AT task) ---- */

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

/* AT+CLAW=tools[,<session_id>] — print the LLM-visible tool names for a session
 * so per-session cap_groups gating (Inc 6) can be observed from serial. */
void tools_list_task(void *p)
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

void fs_rw_task(void *p)
{
    const char *op = p ? (const char *)p : "test";
    int err = 0;

    if (strcmp(op, "write") == 0 || strcmp(op, "test") == 0) {
        FILE *f = fopen(CLAWFS_PATH, "w");
        if (!f) { err = 1; goto done; }
        fwrite(CLAWFS_DATA, 1, strlen(CLAWFS_DATA), f);
        fclose(f);
        at_printf("+CLAW:fs,wrote %zu bytes to %s\r\n",
                  strlen(CLAWFS_DATA), CLAWFS_PATH);
    }
    if (strcmp(op, "read") == 0 || strcmp(op, "test") == 0) {
        FILE *f = fopen(CLAWFS_PATH, "r");
        if (!f) { err = 2; goto done; }
        char rbuf[64] = {0};
        size_t n = fread(rbuf, 1, sizeof(rbuf) - 1, f);
        fclose(f);
        rbuf[n] = '\0';
        at_printf("+CLAW:fs,read=%s,match=%s\r\n", rbuf,
                  strcmp(rbuf, CLAWFS_DATA) == 0 ? "OK" : "FAIL");
        if (strcmp(rbuf, CLAWFS_DATA) != 0) err = 3;
    }
done:
    if (p) rtos_mem_free(p);
    if (err == 0) at_printf(ATCMD_OK_END_STR);
    else          at_printf(ATCMD_ERROR_END_STR, err);
    rtos_task_delete(NULL);
}

void fs_delete_task(void *p)
{
    char *path = (char *)p;
    int rc = path ? remove(path) : -1;
    at_printf("\r\n+CLAW:fs,delete=%s,%s\r\n",
              path ? path : "?", rc == 0 ? "ok" : "fail");
    rtos_mem_free(path);
    if (rc == 0) at_printf(ATCMD_OK_END_STR);
    else         at_printf(ATCMD_ERROR_END_STR, 2);
    rtos_task_delete(NULL);
}

void session_clear_task(void *p)
{
    int flag = p ? *(int *)p : 1;
    rtos_mem_free(p);
    if (flag == 2) {
        int n = claw_memory_clear_all_sessions();
        at_printf("\r\n+CLAW:session,cleared,%d files\r\n", n < 0 ? 0 : n);
    } else {
        claw_memory_clear_session("serial");
        at_printf("\r\n+CLAW:session,cleared\r\n");
    }
    at_printf(ATCMD_OK_END_STR);
    rtos_task_delete(NULL);
}

void memory_op_task(void *p)
{
    int flag = p ? *(int *)p : 1;
    rtos_mem_free(p);
    if (flag == 2) {
        int rc = claw_memory_clear_long_term();
        at_printf("\r\n+CLAW:memory,cleared,%s\r\n", rc == 0 ? "ok" : "not found");
        at_printf(ATCMD_OK_END_STR);
    } else {
        char *json = claw_memory_list(32);
        if (json) {
            at_printf("\r\n+CLAW:memory,list=%s\r\n", json);
            free(json);
        } else {
            at_printf("\r\n+CLAW:memory,list=[]\r\n");
        }
        at_printf(ATCMD_OK_END_STR);
    }
    rtos_task_delete(NULL);
}

void list_session_task(void *p)
{
    (void)p;
    void *dir = opendir(claw_memory_get_session_root());
    if (!dir) {
        at_printf("\r\n+CLAW:session,list=empty\r\n");
        rtos_task_delete(NULL);
        return;
    }
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *n = ent->d_name;
        if (n[0] == '.') continue;
        /* Only count actual session ring-buffer files. Skip:
         *   chat_map     — sub-dir owned by cap_session_mgr (chat→version)
         *   *.bak        — atomic-write rollback shadows (transient)
         *   *.corrupt-*  — quarantined parse failures (kept for forensics)
         * The convention is "s_<sanitized>_<djb2>.json" (see
         *   claw_memory.c::session_file_path). Anchoring on the "s_" prefix
         * is more precise than a blanket directory walk. */
        if (strcmp(n, "chat_map") == 0) continue;
        size_t nl = strlen(n);
        if (nl >= 4 && strcmp(n + nl - 4, ".bak") == 0) continue;
        if (strstr(n, ".corrupt-")) continue;
        if (strncmp(n, "s_", 2) != 0) continue;     /* keep only ring files */
        at_printf("+CLAW:session,#%d=%s\r\n", count++, n);
    }
    closedir(dir);
    at_printf("+CLAW:session,total=%d\r\n", count);
    rtos_task_delete(NULL);
}

void list_vfs_task(void *p)
{
    (void)p;
    void *dir = opendir("vfs:/");
    if (!dir) {
        at_printf("\r\n+CLAW:fs,list=empty\r\n");
        rtos_task_delete(NULL);
        return;
    }
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        at_printf("+CLAW:fs,#%d=%s\r\n", count++, ent->d_name);
    }
    closedir(dir);
    at_printf("+CLAW:fs,total=%d\r\n", count);
    rtos_task_delete(NULL);
}

/* ---- AT command table ---- */

ATCMD_TABLE_DATA_SECTION
const log_item_t at_claw_items[] = {
    {"+CLAW", at_claw},
};

void print_claw_at(void)
{
    at_printf("AT+CLAW=<sub>[,arg...]\r\n");
    at_printf("  ask,<msg>  lua  cfg[,field,val]  wifi[,clear]\r\n");
    at_printf("  wechat,reset  cap  i2c,sh1106  spi,<mode>  rtc[,test]  pwm  ir,<tx|rx> gpio\r\n");
    at_printf("  speaker  dmic  lcdc,<rgb|srgb|mcu>,<panel>\r\n");

#ifdef CLAW_BUILD_TESTS
    at_printf("  test[,suite]  fs[,op]\r\n");
#endif
}

void at_claw_init(void)
{
    /* serial: progress/trace printed per-call; no dispatcher ACK needed */
claw_im_dispatch_register_with_flags("serial", serial_im_send,
    CLAW_IM_CHANNEL_FLAG_SILENT_PROGRESS |
    CLAW_IM_CHANNEL_FLAG_SILENT_TRACE    |
    CLAW_IM_CHANNEL_FLAG_NO_ACK,
    NULL);  /* serial has no LLM-callable send cap */
}

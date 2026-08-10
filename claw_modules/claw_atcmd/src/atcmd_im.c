/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AT+CLAW=im — IM channel configuration (Telegram / Feishu / QQ / WeChat).
 *
 *   AT+CLAW=im                                     Show all IM channel status
 *   AT+CLAW=im,telegram,<bot_token>|clear          Set/clear Telegram bot token
 *   AT+CLAW=im,feishu,<app_id>,<app_secret>|clear  Set/clear Feishu credentials
 *   AT+CLAW=im,qq,<app_id>,<app_secret>[,<0|2>]    Set QQ (msg_type 0=text 2=markdown)
 *   AT+CLAW=im,qq,clear                            Clear QQ credentials
 *   AT+CLAW=im,wechat                              Show WeChat config + login state
 *   AT+CLAW=im,wechat,<base_url>[,<app_id>]        Set WeChat iLink server
 *   AT+CLAW=im,wechat,login                        Fetch QR, print +CLAW:wechat,qr=<url>
 *   AT+CLAW=im,wechat,status                       Print current login state (JSON)
 *
 * Sensitive fields (tokens/secrets) are shown only as (set)/(empty).
 * Credential changes are applied live via each cap's on-config-saved hook.
 * WeChat login is a QR-scan flow: the device fetches a QR image URL, prints
 * it, and a background task polls for scan confirmation (token is obtained
 * and stored automatically — never pasted).
 */

#include "ameba_soc.h"
#include "atcmd_service.h"
#include "claw_config.h"
#ifdef CONFIG_CLAW_CAP_IM_WECHAT
#include "cap_im_wechat.h"
#endif
#include "ameba_claw_defs.h"
#include "atcmd_handlers.h"
#include <string.h>
#include <stdlib.h>

#define TAG "claw_cmd"

/* cap_im_wechat is the one optional capability referenced by name from this
 * core AT module. When it is compiled out (CONFIG_CLAW_CAP_IM_WECHAT unset) the
 * AT wechat sub-commands remain compiled but degrade gracefully via these
 * no-op stubs, so the module still links (plan §7 reference-point guard). */
#ifndef CONFIG_CLAW_CAP_IM_WECHAT
static inline int cap_im_wechat_get_qr(char *qr_url, size_t size)
{
    if (size) qr_url[0] = '\0';
    return -1;
}
static inline void cap_im_wechat_get_status_json(char *buf, size_t buf_size)
{
    if (buf_size) strlcpy(buf, "{\"state\":\"disabled\"}", buf_size);
}
#endif

/* ---- Config-save worker (cJSON+VFS needs more stack than the AT task) ---- */

typedef enum {
    IM_PF_TELEGRAM = 1,
    IM_PF_FEISHU,
    IM_PF_QQ,
    IM_PF_WECHAT,
} im_platform_t;

typedef struct {
    uint8_t platform;
    char    f1[256];   /* tg:token / feishu:app_id / qq:app_id / wechat:base_url */
    char    f2[128];   /* feishu:app_secret / qq:app_secret / wechat:app_id      */
    int     msg_type;  /* qq only; -1 = keep current                            */
} im_save_args_t;

static void im_save_task(void *param)
{
    im_save_args_t *a = (im_save_args_t *)param;
    int rc = -1;

    switch (a->platform) {
    case IM_PF_TELEGRAM:
        rc = claw_config_set_telegram(a->f1);
        break;
    case IM_PF_FEISHU:
        rc = claw_config_set_feishu(a->f1, a->f2);
        break;
    case IM_PF_QQ:
        rc = claw_config_set_qq(a->f1, a->f2, a->msg_type);
        break;
    case IM_PF_WECHAT:
        rc = claw_config_set_wechat(a->f1[0] ? a->f1 : NULL,
                                    a->f2[0] ? a->f2 : NULL);
        break;
    default:
        break;
    }

    if (rc == 0) {
        RTK_LOGI(TAG, "im config saved\n");
    } else {
        RTK_LOGE(TAG, "im config save failed: %d\n", rc);
    }
    free(a);
    rtos_task_delete(NULL);
}

static int im_spawn(im_save_args_t *a)
{
    if (rtos_task_create(NULL, "im_save", im_save_task, a,
                         CLAW_ATCMD_CFG_SAVE_STACK, 1) != RTK_SUCCESS) {
        free(a);
        return -1;
    }
    return 0;
}

#ifdef CONFIG_CLAW_CAP_IM_WECHAT
/* ---- WeChat login worker (needs ~16 KB for the QR-fetch TLS handshake) ---- */

static void wechat_login_task(void *arg)
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
#endif /* CONFIG_CLAW_CAP_IM_WECHAT */

/* ---- Status display ---- */

static void im_show_status(void)
{
    claw_config_t *cfg = claw_config_get();
#ifdef CONFIG_CLAW_CAP_IM_WECHAT
    char wc_status[600];
#endif

    at_printf("\r\n+CLAW:im,telegram=%s\r\n",
              cfg->telegram.bot_token[0] ? "(set)" : "(empty)");
    at_printf("+CLAW:im,feishu,app_id=%s,app_secret=%s\r\n",
              cfg->feishu.app_id[0]     ? cfg->feishu.app_id : "(empty)",
              cfg->feishu.app_secret[0] ? "(set)" : "(empty)");
    at_printf("+CLAW:im,qq,app_id=%s,app_secret=%s,msg_type=%d\r\n",
              cfg->qq.app_id[0]     ? cfg->qq.app_id : "(empty)",
              cfg->qq.app_secret[0] ? "(set)" : "(empty)",
              (int)cfg->qq.msg_type);

#ifdef CONFIG_CLAW_CAP_IM_WECHAT
    cap_im_wechat_get_status_json(wc_status, sizeof(wc_status));
    at_printf("+CLAW:im,wechat,base_url=%s,app_id=%s,status=%s\r\n",
              cfg->wechat.base_url[0] ? cfg->wechat.base_url : "(empty)",
              cfg->wechat.app_id[0]   ? cfg->wechat.app_id   : "(empty)",
              wc_status);
#endif

    at_printf(ATCMD_OK_END_STR);
}

/* ---- Per-platform handlers ---- */

/* Fill a save-args struct for a plain "paste credentials" platform and spawn
 * the save worker. Returns 0 on success (OK already needs printing by caller). */
static int im_save_simple(im_platform_t pf, const char *f1, const char *f2, int msg_type)
{
    im_save_args_t *a = (im_save_args_t *)calloc(1, sizeof(*a));
    if (!a) return -2;
    a->platform = (uint8_t)pf;
    a->msg_type = msg_type;
    if (f1) strlcpy(a->f1, f1, sizeof(a->f1));
    if (f2) strlcpy(a->f2, f2, sizeof(a->f2));
    return im_spawn(a);   /* frees a on failure */
}

#ifdef CONFIG_CLAW_CAP_IM_WECHAT
static void handle_im_wechat(const char *arg3, const char *arg4)
{
    /* No sub-arg: show WeChat config + current login state */
    if (arg3[0] == '\0') {
        claw_config_t *cfg = claw_config_get();
        char wc_status[600];
        cap_im_wechat_get_status_json(wc_status, sizeof(wc_status));
        at_printf("\r\n+CLAW:im,wechat,base_url=%s,app_id=%s\r\n",
                  cfg->wechat.base_url[0] ? cfg->wechat.base_url : "(empty)",
                  cfg->wechat.app_id[0]   ? cfg->wechat.app_id   : "(empty)");
        at_printf("+CLAW:im,wechat,status=%s\r\n", wc_status);
        at_printf(ATCMD_OK_END_STR);
        return;
    }

    if (strcmp(arg3, "login") == 0) {
        if (rtos_task_create(NULL, "wx_login", wechat_login_task,
                             NULL, CLAW_ATCMD_WECHAT_STACK, 1) != RTK_SUCCESS) {
            at_printf(ATCMD_ERROR_END_STR, 2);
            return;
        }
        at_printf("\r\n+CLAW:wechat,triggered\r\n");
        at_printf(ATCMD_OK_END_STR);
        return;
    }

    if (strcmp(arg3, "status") == 0) {
        char wc_status[600];
        cap_im_wechat_get_status_json(wc_status, sizeof(wc_status));
        at_printf("\r\n+CLAW:wechat,status=%s\r\n", wc_status);
        at_printf(ATCMD_OK_END_STR);
        return;
    }

    /* Otherwise arg3 is a base_url; arg4 optional app_id */
    int rc = im_save_simple(IM_PF_WECHAT, arg3, arg4[0] ? arg4 : NULL, -1);
    if (rc == -2) { at_printf(ATCMD_ERROR_END_STR, 2); return; }
    if (rc != 0)  { at_printf(ATCMD_ERROR_END_STR, 3); return; }
    at_printf("\r\n+CLAW:im,wechat saved\r\n");
    at_printf(ATCMD_OK_END_STR);
}
#endif /* CONFIG_CLAW_CAP_IM_WECHAT */

/* ---- Entry point ---- */

void handle_cmd_im(u16 argc, char **argv, const char *arg2, const char *arg3)
{
    const char *arg4 = (argc >= 5 && argv[4]) ? argv[4] : "";  /* 2nd value */
    const char *arg5 = (argc >= 6 && argv[5]) ? argv[5] : "";  /* qq msg_type */
    int rc;

    if (arg2[0] == '\0') { im_show_status(); return; }

    if (strcmp(arg2, "telegram") == 0) {
        if (arg3[0] == '\0') {
            at_printf("\r\n+CLAW:usage: AT+CLAW=im,telegram,<bot_token>|clear\r\n");
            at_printf(ATCMD_ERROR_END_STR, 1);
            return;
        }
        /* "clear" → empty token → disables the platform */
        const char *tok = (strcmp(arg3, "clear") == 0) ? "" : arg3;
        rc = im_save_simple(IM_PF_TELEGRAM, tok, "", -1);

    } else if (strcmp(arg2, "feishu") == 0) {
        if (arg3[0] == '\0') {
            at_printf("\r\n+CLAW:usage: AT+CLAW=im,feishu,<app_id>,<app_secret>|clear\r\n");
            at_printf(ATCMD_ERROR_END_STR, 1);
            return;
        }
        if (strcmp(arg3, "clear") == 0) {
            rc = im_save_simple(IM_PF_FEISHU, "", "", -1);
        } else {
            if (arg4[0] == '\0') {
                at_printf("\r\n+CLAW:usage: AT+CLAW=im,feishu,<app_id>,<app_secret>\r\n");
                at_printf(ATCMD_ERROR_END_STR, 1);
                return;
            }
            rc = im_save_simple(IM_PF_FEISHU, arg3, arg4, -1);
        }

    } else if (strcmp(arg2, "qq") == 0) {
        if (arg3[0] == '\0') {
            at_printf("\r\n+CLAW:usage: AT+CLAW=im,qq,<app_id>,<app_secret>[,<0|2>]|clear\r\n");
            at_printf(ATCMD_ERROR_END_STR, 1);
            return;
        }
        if (strcmp(arg3, "clear") == 0) {
            rc = im_save_simple(IM_PF_QQ, "", "", -1);
        } else {
            if (arg4[0] == '\0') {
                at_printf("\r\n+CLAW:usage: AT+CLAW=im,qq,<app_id>,<app_secret>[,<0|2>]\r\n");
                at_printf(ATCMD_ERROR_END_STR, 1);
                return;
            }
            int msg_type = -1;
            if (arg5[0]) {
                msg_type = atoi(arg5);
                if (msg_type != 0 && msg_type != 2) {
                    at_printf("\r\n+CLAW:qq msg_type must be 0 (text) or 2 (markdown)\r\n");
                    at_printf(ATCMD_ERROR_END_STR, 4);
                    return;
                }
            }
            rc = im_save_simple(IM_PF_QQ, arg3, arg4, msg_type);
        }

#ifdef CONFIG_CLAW_CAP_IM_WECHAT
    } else if (strcmp(arg2, "wechat") == 0) {
        handle_im_wechat(arg3, arg4);   /* prints its own OK/ERROR */
        return;
#endif

    } else {
        at_printf("\r\n+CLAW:unknown im platform: %s  try: telegram,feishu,qq,wechat\r\n", arg2);
        at_printf(ATCMD_ERROR_END_STR, 5);
        return;
    }

    if (rc == -2) { at_printf(ATCMD_ERROR_END_STR, 2); return; }
    if (rc != 0)  { at_printf(ATCMD_ERROR_END_STR, 3); return; }
    at_printf("\r\n+CLAW:im,%s saved\r\n", arg2);
    at_printf(ATCMD_OK_END_STR);
}

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
 *   AT+CLAW=session,new[,ch,id[,name]] Create new session, keep history (default ch=serial id=atcmd)
 *   AT+CLAW=memory,list                List all long-term memories
 *   AT+CLAW=memory,clear               Clear all long-term memories
 *   AT+CLAW=fs,list                    List all files in VFS root
 *   AT+CLAW=fs,write                   Write test file
 *   AT+CLAW=fs,read                    Read test file
 *   AT+CLAW=fs,test                    Full write+read+verify cycle
 *   AT+CLAW=fs,delete,<path>           Delete file at path
 *   AT+CLAW=i2c,sh1106                 Run SH1106 OLED I2C test
 *   AT+CLAW=spi,<poll|intr|dma>        Run SPI hardware test
 *   AT+CLAW=led[,<num_leds>]           WS2812 strip demo (MOSI from board.json, default 15)
 *   AT+CLAW=led,loop[,<num_leds>]      WS2812 strip continuous animation (background)
 *   AT+CLAW=led,off                    Stop the WS2812 animation and turn the strip off
 *   AT+CLAW=ir,<tx|rx>                 Run IR hardware test
 *   AT+CLAW=rtc[,test]                Run RTC set_time/alarm test
 *   AT+CLAW=pwm                        Run PWM test (PA_6, TIM4 ch0)
 *   AT+CLAW=adc                        Run ADC loopback test (PA_13 <-> PA_25 wired)
 *   AT+CLAW=adc,ext                    Run ADC external supply test (supply on PA_13)
 *   AT+CLAW=thermal                    Run on-chip thermal sensor test
 *   AT+CLAW=captouch                   Run on-chip CapTouch self-cap keys interactive test
 *   AT+CLAW=captouch,ext               Run on-chip CapTouch self-cap keys external trigger test
 *   AT+CLAW=gpio                       Run GPIO interrupt test (PA30->PA31)
 *   AT+CLAW=lcdc,rgb,st7262            Run LCDC RGB st7262 (800x480) colour-fill test
 *   AT+CLAW=lcdc,srgb,st7272a          Run LCDC SRGB st7272a (320x240) colour-fill test
 *   AT+CLAW=lcdc,mcu,ili9806           Run LCDC MCU ILI9806 (480x800) colour-fill test
 *   AT+CLAW=speaker[,sine|nokia]        Run speaker test; sine=1kHz only, nokia=wav only, default=allv via MAX98357A)
 *   AT+CLAW=dmic[,<vol>]               Run DMIC SNR/THD test; vol=speaker volume 0.0-1.0 (default 0.4)
 *   AT+CLAW=rec[,<path>[,<ms>]]        Record DMIC to WAV; default path=vfs:rec.wav, duration=5000ms
 *   AT+CLAW=play[,<path>]              Play WAV file; default path=vfs:rec.wav
 *   AT+CLAW=usb,uvc                    Capture one JPEG frame from USB UVC camera, save to vfs:capture.jpg
 *   AT+CLAW=basic,timer                Run basic timer test (TIM0)
 *   AT+CLAW=usb,list[,<path>]          List U-disk directory (default: root)
 *   AT+CLAW=usb,write,<path>,<data>    Write data to file on U-disk (create/overwrite)
 *   AT+CLAW=usb,read,<path>            Read and print file content from U-disk
 *   AT+CLAW=usb,delete,<path>          Delete file from U-disk
 *   AT+CLAW=sys,tasks                  List all FreeRTOS tasks with state, priority and stack watermark
 *   AT+CLAW=gpio_ctrl,<pin>,<gesture>  Test-bench: drive a GPIO to emulate a DUT button press
 *                                      gesture = click|double|long[,ms]|bounce|press|release|seq,d0,d1,...
 *                                      (only built when CLAW_AGENT_AUTO_TEST is enabled)
 *   AT+CLAW=test[,<cap|mem|router|fs>] Run unit tests (CLAW_BUILD_TESTS)
 */

#include "ameba_soc.h"
#include "atcmd_service.h"
#include "claw_im_dispatch.h"
#include "ameba_claw_defs.h"
#include "lua_modules_config.h"   /* LUA_DRIVER_TESTS_ENABLED */
#include "atcmd_handlers.h"
#include <string.h>

/* ---- Serial IM channel ---- */

static void serial_im_send(const char *chat_id, const char *text)
{
    (void)chat_id;
    if (text) {
        at_printf("\r\n+CLAW:%s\r\n", text);
    }
}

void at_claw_serial_echo(const char *text)
{
    if (text && text[0]) {
        at_printf("\r\n+CLAW:%s\r\n", text);
    }
}

/* ---- AT+CLAW main handler ---- */

static void at_claw(u16 argc, char **argv)
{
    const char *sub  = (argc >= 2 && argv[1]) ? argv[1] : "";
    const char *arg2 = (argc >= 3 && argv[2]) ? argv[2] : "";
    const char *arg3 = (argc >= 4 && argv[3]) ? argv[3] : "";

    if (strcmp(sub, "ask")     == 0) { handle_cmd_ask(argc, argv, arg2, arg3);     return; }
    if (strcmp(sub, "ask_buf") == 0) { handle_cmd_ask_buf(argc, argv, arg2);       return; }
    if (strcmp(sub, "lua")     == 0) { handle_cmd_lua();                            return; }
    if (strcmp(sub, "cfg")     == 0) { handle_cmd_cfg(argc, argv, arg2, arg3);     return; }
    if (strcmp(sub, "skill")   == 0) { handle_cmd_skill(argc, argv, arg2, arg3);   return; }
    if (strcmp(sub, "wifi")    == 0) { handle_cmd_wifi(arg2);                       return; }
    if (strcmp(sub, "wechat")  == 0) { handle_cmd_wechat(arg2);                    return; }
    if (strcmp(sub, "session") == 0) { handle_cmd_session(argc, argv, arg2, arg3); return; }
    if (strcmp(sub, "memory")  == 0) { handle_cmd_memory(arg2);                    return; }
    if (strcmp(sub, "tools")   == 0) { handle_cmd_tools(arg2);                     return; }
    if (strcmp(sub, "cap")     == 0) { handle_cmd_cap(argc, argv, arg2, arg3);     return; }
    if (strcmp(sub, "fs")      == 0) { handle_cmd_fs(argc, argv, arg2, arg3);      return; }
    if (strcmp(sub, "sys")     == 0) { handle_cmd_sys(arg2);                       return; }
#if CLAW_AGENT_AUTO_TEST
    if (strcmp(sub, "gpio_ctrl") == 0) { handle_cmd_gpio_ctrl(argc, argv, arg2, arg3); return; }
#endif

#ifdef CLAW_BUILD_TESTS
    if (strcmp(sub, "test")    == 0) { handle_cmd_test(arg2);                      return; }
#endif

    if (handle_cmd_hw_test(argc, argv, sub, arg2, arg3)) return;

    at_printf("\r\n+CLAW:unknown: %s  try: ask,lua,cfg,wifi,wechat,cap,"
              "session,memory,fs,basic,i2c,spi,led,rtc,pwm,ir,adc,thermal,env,captouch,lcdc,gpio,usb,sys,speaker,dmic\r\n",
              sub[0] ? sub : "(none)");
    at_printf(ATCMD_ERROR_END_STR, 99);
}

/* ---- AT command table ---- */

ATCMD_TABLE_DATA_SECTION
const log_item_t at_claw_items[] = {
    {"+CLAW", at_claw},
};

void print_claw_at(void)
{
    at_printf("AT+CLAW=<sub>[,arg...]\r\n");
    at_printf("  ask,<msg>  ask_buf[,<chunk>|clear]  lua  cfg[,field,val]  wifi[,clear]\r\n");
    at_printf("  wechat,reset  cap  i2c,sh1106  spi,<mode>  led[,n]|led,loop[,n]|led,off  rtc[,test]  pwm  ir,<tx|rx> gpio\r\n");
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

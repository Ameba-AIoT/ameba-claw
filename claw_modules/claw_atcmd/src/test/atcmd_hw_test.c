/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ameba_soc.h"
#include "atcmd_service.h"
#include "lua_modules_config.h"

#define LUA_USB_ENABLED (LUA_MOD_ENABLE_USB_UVC || LUA_MOD_ENABLE_USB_MSC)

#if LUA_DRIVER_TESTS_ENABLED

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

extern void lua_i2c_run_sh1106(int sx, int sy);
extern void lua_i2c_run_rw(void);
extern void lua_i2c_run_slave(void);
extern void lua_spi_run(const char *mode);
#if LUA_MOD_ENABLE_IR
extern void lua_ir_run(const char *mode);
#endif
extern void lua_rtc_run(const char *mode);
extern void lua_pwm_run(void);
extern void lua_servo_run(int start_angle, int end_angle, int step,
                          int delay_ms, int edge_hold_ms);
extern void lua_gpio_run(void);
#if LUA_MOD_ENABLE_UART
extern void lua_uart_run(const char *mode);
#endif
#if LUA_MOD_ENABLE_AUDIO
extern void lua_speaker_run(const char *mode, const char *vol);
extern void lua_dmic_run(const char *vol);
extern void lua_audio_rec_run(const char *path, int duration_ms);
extern void lua_audio_play_run(const char *path);
#endif
#if LUA_MOD_ENABLE_LCDC
extern void lua_lcdc_run(const char *if_mode, const char *panel);
#endif
#if LUA_MOD_ENABLE_DISPLAY
extern void display_lcdc_run(void);
extern void display_lvgl_bench_run(int frames, const char *dev_id);
extern void display_lvgl_static_run(int hold_ms, const char *dev_id);
#endif
#if LUA_MOD_ENABLE_ADC
extern void lua_adc_run(const char *mode);
#endif
#if LUA_MOD_ENABLE_THERMAL
extern void lua_thermal_run(int count, int interval_ms);
#endif
#if LUA_MOD_ENABLE_ENVIRONMENTAL_SENSOR
extern void lua_module_environmental_sensor_run(const char *pin);
#endif
#if LUA_MOD_ENABLE_LIGHT_SENSOR
extern void lua_module_light_sensor_run(const char *do_pin, int count);
#endif
#if LUA_MOD_ENABLE_IMU
extern void lua_module_imu_run(const char *chip, const char *sda, const char *scl,
                               int i2c, int addr, int count, int interval_ms);
#endif
#if LUA_MOD_ENABLE_STORAGE
extern void lua_storage_run_info(void);
extern void lua_storage_run_write(const char *path, const char *data);
extern void lua_storage_run_read(const char *path);
extern void lua_storage_run_list(const char *path);
extern void lua_storage_run_remove(const char *path);
#endif
#if LUA_MOD_ENABLE_MAGNETOMETER
extern void lua_module_magnetometer_run(const char *sda, const char *scl,
                                        int i2c, int addr, const char *int_gpio,
                                        int count, int interval_ms);
#endif
#if LUA_MOD_ENABLE_CAPTOUCH
extern void lua_captouch_run(const char *mode);
#endif
#if LUA_MOD_ENABLE_BASICTIMER
extern void lua_basictimer_run(void);
#endif
extern void lua_led_strip_run(int count);
extern void lua_led_strip_loop(int count);
extern void lua_led_strip_stop(void);
extern void swtimer_stop_all(void);
#if LUA_USB_ENABLED
extern void lua_uvc_run(void);
extern void lua_msc_run(void);
extern void lua_msc_list_run(const char *path);
extern void lua_msc_write_run(const char *path, const char *data);
extern void lua_msc_read_run(const char *path);
extern void lua_msc_delete_run(const char *path);
#endif

int handle_cmd_hw_test(u16 argc, char **argv, const char *sub,
                       const char *arg2, const char *arg3)
{
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
            at_printf("+CLAW:  rw    — master test, run slave first\r\n");
            at_printf("+CLAW:  slave — slave mode, addr=0x50\r\n");
            at_printf(ATCMD_ERROR_END_STR, 1);
        }
        return 1;
    }

    /* ---- spi ---- */
    if (strcmp(sub, "spi") == 0) {
        if (arg2[0] == '\0') {
            at_printf("\r\n+CLAW:usage: AT+CLAW=spi,<poll|intr|dma|st7789>\r\n");
            at_printf(ATCMD_ERROR_END_STR, 1);
            return 1;
        }
        at_printf("\r\n+CLAW:spi,running %s test...\r\n", arg2);
        lua_spi_run(arg2);
        at_printf(ATCMD_OK_END_STR);
        return 1;
    }

    /* ---- ir ---- */
#if LUA_MOD_ENABLE_IR
    if (strcmp(sub, "ir") == 0) {
        if (arg2[0] == '\0') {
            at_printf("\r\n+CLAW:usage: AT+CLAW=ir,<tx|tx,poll|tx,intr|rx>\r\n");
            at_printf(ATCMD_ERROR_END_STR, 1);
            return 1;
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
        return 1;
    }
#endif

    /* ---- rtc ---- */
    if (strcmp(sub, "rtc") == 0) {
        const char *mode = arg2[0] ? arg2 : "test";
        at_printf("\r\n+CLAW:rtc,running %s test...\r\n", mode);
        lua_rtc_run(mode);
        at_printf(ATCMD_OK_END_STR);
        return 1;
    }

    /* ---- pwm ---- */
    if (strcmp(sub, "pwm") == 0) {
        at_printf("\r\n+CLAW:pwm,running test...\r\n");
        lua_pwm_run();
        at_printf(ATCMD_OK_END_STR);
        return 1;
    }

    /* ---- servo ---- *
     * Sweep:    AT+CLAW=servo[,<start>[,<end>[,<step>[,<delay_ms>]]]]
     * Goto:     AT+CLAW=servo,at,<angle>[,<hold_ms>]
     *
     * sweep params (all optional, defaults shown):
     *   start    deg  45   sweep start angle
     *   end      deg  135  sweep end angle
     *   step     deg  10   angle increment per step
     *   delay_ms ms   80   pause between steps
     * goto params:
     *   angle    deg  —    target angle (required)
     *   hold_ms  ms   1000 how long to hold at the angle
     *
     * Examples:
     *   AT+CLAW=servo             default sweep 45->135
     *   AT+CLAW=servo,0,180       full-range sweep
     *   AT+CLAW=servo,0,180,5,50  full-range, fine step, faster
     *   AT+CLAW=servo,at,90       go to 90 deg, hold 1 s
     *   AT+CLAW=servo,at,0,2000   go to 0 deg,  hold 2 s  */
    if (strcmp(sub, "servo") == 0) {
        if (strcmp(arg2, "at") == 0) {
            /* Goto-angle mode */
            if (!arg3[0]) {
                at_printf("\r\n+CLAW:usage: AT+CLAW=servo,at,<angle>[,<hold_ms>]\r\n");
                at_printf(ATCMD_ERROR_END_STR, 1);
                return 1;
            }
            int angle   = atoi(arg3);
            int hold_ms = (argc >= 5 && argv[4] && argv[4][0])
                          ? atoi(argv[4]) : 1000;
            at_printf("\r\n+CLAW:servo,goto %d deg, hold %d ms (PA_26 TIM4/ch3 50Hz)...\r\n",
                      angle, hold_ms);
            /* start == end triggers a single-position hold in the Lua script. */
            lua_servo_run(angle, angle, 1, 0, hold_ms);
        } else {
            /* Sweep mode */
            int start = (argc >= 3 && arg2[0]) ? atoi(arg2) : -1;
            int end_a = (argc >= 4 && arg3[0]) ? atoi(arg3) : -1;
            int step  = (argc >= 5 && argv[4] && argv[4][0]) ? atoi(argv[4]) : -1;
            int dly   = (argc >= 6 && argv[5] && argv[5][0]) ? atoi(argv[5]) : -1;
            at_printf("\r\n+CLAW:servo,SG90 sweep (PA_26 TIM4/ch3 50Hz)"
                      " start=%d end=%d step=%d delay=%dms...\r\n",
                      start >= 0 ? start : 45,
                      end_a >= 0 ? end_a : 135,
                      step  >  0 ? step  : 10,
                      dly   >= 0 ? dly   : 80);
            lua_servo_run(start, end_a, step, dly, -1);
        }
        at_printf(ATCMD_OK_END_STR);
        return 1;
    }

    /* ---- adc ---- */
#if LUA_MOD_ENABLE_ADC
    if (strcmp(sub, "adc") == 0) {
        const char *mode = (strcmp(arg2, "ext") == 0) ? "ext_supply" : "loopback";
        at_printf("\r\n+CLAW:adc,mode=%s,running...\r\n", mode);
        lua_adc_run(mode);
        at_printf(ATCMD_OK_END_STR);
        return 1;
    }
#endif

    /* ---- led (WS2812 strip) ----
     * AT+CLAW=led[,<n>]        one-shot demo (n pixels, default 15)
     * AT+CLAW=led,loop[,<n>]   continuous animation (runs in background)
     * AT+CLAW=led,off          stop the animation and turn the strip off
     */
    if (strcmp(sub, "led") == 0) {
        if (strcmp(arg2, "off") == 0 || strcmp(arg2, "stop") == 0) {
            at_printf("\r\n+CLAW:led,stopping...\r\n");
            lua_led_strip_stop();
            at_printf(ATCMD_OK_END_STR);
        } else if (strcmp(arg2, "loop") == 0) {
            int count = (arg3[0]) ? atoi(arg3) : 15;
            if (count < 1) {
                count = 15;
            }
            at_printf("\r\n+CLAW:led,loop start (%d pixels)...\r\n", count);
            lua_led_strip_loop(count);
            at_printf(ATCMD_OK_END_STR);
        } else {
            int count = (arg2[0]) ? atoi(arg2) : 15;
            if (count < 1) {
                count = 15;
            }
            at_printf("\r\n+CLAW:led,running WS2812 demo (%d pixels)...\r\n", count);
            lua_led_strip_run(count);
            at_printf(ATCMD_OK_END_STR);
        }
        return 1;
    }

    /* ---- gpio ---- */
    if (strcmp(sub, "gpio") == 0) {
        at_printf("\r\n+CLAW:gpio,running interrupt test (PA30->PA31)...\r\n");
        lua_gpio_run();
        at_printf(ATCMD_OK_END_STR);
        return 1;
    }

    /* ---- uart ---- */
#if LUA_MOD_ENABLE_UART
    if (strcmp(sub, "uart") == 0) {
        const char *umode = (arg2 && arg2[0]) ? arg2 : "loopback";
        at_printf("\r\n+CLAW:uart,running %s test...\r\n", umode);
        lua_uart_run(arg2);
        at_printf(ATCMD_OK_END_STR);
        return 1;
    }
#endif

    /* ---- thermal ---- */
#if LUA_MOD_ENABLE_THERMAL
    if (strcmp(sub, "thermal") == 0) {
        int count       = (argc >= 3 && argv[2] && argv[2][0]) ? atoi(argv[2]) : -1;
        int interval_ms = (argc >= 4 && argv[3] && argv[3][0]) ? atoi(argv[3]) : -1;
        at_printf("\r\n+CLAW:thermal,running test...\r\n");
        lua_thermal_run(count, interval_ms);
        at_printf(ATCMD_OK_END_STR);
        return 1;
    }
#endif

#if LUA_MOD_ENABLE_ENVIRONMENTAL_SENSOR
    /* ---- env (environmental sensors) ----
     *   AT+CLAW=env,dht11          -- DHT11 on default pin PB_8
     *   AT+CLAW=env,dht11,PB_8    -- DHT11 on explicit pin
     */
    if (strcmp(sub, "env") == 0) {
        if (strcmp(arg2, "dht11") == 0) {
            const char *pin = (arg3 && arg3[0]) ? arg3 : "PB_8";
            at_printf("\r\n+CLAW:env,dht11,pin=%s,running...\r\n", pin);
            lua_module_environmental_sensor_run(pin);
            at_printf(ATCMD_OK_END_STR);
        } else {
            at_printf("\r\n+CLAW:usage: AT+CLAW=env,<dht11>[,<pin>]\r\n");
            at_printf(ATCMD_ERROR_END_STR, 1);
        }
        return 1;
    }
#endif

#if LUA_MOD_ENABLE_LIGHT_SENSOR
    /* ---- light_sensor (LM393 + LDR) ----
     *   AT+CLAW=light_sensor             -- pin from board.json "light_sensor" device
     *   AT+CLAW=light_sensor,PA_26       -- explicit DO pin override
     */
    if (strcmp(sub, "light_sensor") == 0) {
        const char *do_pin = (arg2 && arg2[0]) ? arg2 : NULL;  /* NULL = read from board.json in Lua */
        int count = (arg3 && arg3[0]) ? atoi(arg3) : 10;
        if (count <= 0) { count = 10; }
        at_printf("\r\n+CLAW:light_sensor,do_pin=%s,count=%d,running...\r\n",
                  do_pin ? do_pin : "(board.json)", count);
        lua_module_light_sensor_run(do_pin, count);
        at_printf(ATCMD_OK_END_STR);
        return 1;
    }
#endif

#if LUA_MOD_ENABLE_IMU
    /* ---- imu (MPU-6050 / GY-521, 6-axis) ----
     *   AT+CLAW=imu,mpu6050                              -- default pins from board.json
     *   AT+CLAW=imu,mpu6050,PA_26,PA_25                  -- explicit sda,scl (i2c0, 0x68)
     *   AT+CLAW=imu,mpu6050,PA_26,PA_25,1,0x69           -- explicit sda,scl,i2c,addr
     *   AT+CLAW=imu,mpu6050,PA_26,PA_25,0,0x68,20,200    -- + count=20, interval=200ms
     * Nothing is hard-coded: chip/sda/scl/i2c/addr/count/interval all come from the
     * command line (or board.json); only field defaults apply when an arg is omitted. */
    if (strcmp(sub, "imu") == 0) {
        const char *chip = (arg2 && arg2[0]) ? arg2 : "mpu6050";
        const char *sda  = (arg3 && arg3[0]) ? arg3 : "PA_26";
        const char *scl  = (argc >= 5 && argv[4] && argv[4][0]) ? argv[4] : "PA_25";
        int i2c   = (argc >= 6 && argv[5] && argv[5][0]) ? (int)strtol(argv[5], NULL, 0) : 0;
        int addr  = (argc >= 7 && argv[6] && argv[6][0]) ? (int)strtol(argv[6], NULL, 0) : 0x68;
        int count = (argc >= 8 && argv[7] && argv[7][0]) ? (int)strtol(argv[7], NULL, 0) : 10;
        int intvl = (argc >= 9 && argv[8] && argv[8][0]) ? (int)strtol(argv[8], NULL, 0) : 100;
        at_printf("\r\n+CLAW:imu,%s,sda=%s,scl=%s,i2c=%d,addr=0x%02x,count=%d,interval=%dms,running...\r\n",
                  chip, sda, scl, i2c, addr, count, intvl);
        lua_module_imu_run(chip, sda, scl, i2c, addr, count, intvl);
        at_printf(ATCMD_OK_END_STR);
        return 1;
    }
#endif

#if LUA_MOD_ENABLE_MAGNETOMETER
    /* ---- magnetometer (BMM150 3-axis, I2C) ----
     *   AT+CLAW=magnetometer                                    -- board.json defaults
     *   AT+CLAW=magnetometer,PA_26,PA_25                        -- explicit sda,scl
     *   AT+CLAW=magnetometer,PA_26,PA_25,0,0x10                 -- + i2c, addr
     *   AT+CLAW=magnetometer,PA_26,PA_25,0,0x10,PB_8            -- + INT pin
     *   AT+CLAW=magnetometer,PA_26,PA_25,0,0x10,PB_8,20,1000    -- + count, interval_ms */
    if (strcmp(sub, "magnetometer") == 0) {
        const char *sda      = (arg2 && arg2[0]) ? arg2 : "PA_26";
        const char *scl      = (arg3 && arg3[0]) ? arg3 : "PA_25";
        int i2c      = (argc >= 5 && argv[4] && argv[4][0]) ? (int)strtol(argv[4], NULL, 0) : 0;
        int addr     = (argc >= 6 && argv[5] && argv[5][0]) ? (int)strtol(argv[5], NULL, 0) : 0x10;
        const char *int_gpio = (argc >= 7 && argv[6] && argv[6][0]) ? argv[6] : NULL;
        int count    = (argc >= 8 && argv[7] && argv[7][0]) ? (int)strtol(argv[7], NULL, 0) : 20;
        int intvl    = (argc >= 9 && argv[8] && argv[8][0]) ? (int)strtol(argv[8], NULL, 0) : 1000;
        at_printf("\r\n+CLAW:magnetometer,sda=%s,scl=%s,i2c=%d,addr=0x%02x,int=%s,count=%d,interval=%dms,running...\r\n",
                  sda, scl, i2c, addr, int_gpio ? int_gpio : "none", count, intvl);
        lua_module_magnetometer_run(sda, scl, i2c, addr, int_gpio, count, intvl);
        at_printf(ATCMD_OK_END_STR);
        return 1;
    }
#endif

    /* ---- basic (hardware peripheral basics) ---- */
#if LUA_MOD_ENABLE_BASICTIMER
    if (strcmp(sub, "basic") == 0) {
        if (strcmp(arg2, "timer") == 0) {
            at_printf("\r\n+CLAW:basic,running basictimer test (TIM0)...\r\n");
            lua_basictimer_run();
            at_printf(ATCMD_OK_END_STR);
        } else {
            at_printf("\r\n+CLAW:usage: AT+CLAW=basic,timer\r\n");
            at_printf(ATCMD_ERROR_END_STR, 1);
        }
        return 1;
    }
#endif

    /* ---- captouch (on-chip CapTouch self-cap keys) ---- */
#if LUA_MOD_ENABLE_CAPTOUCH
    if (strcmp(sub, "captouch") == 0) {
        const char *mode = (strcmp(arg2, "ext") == 0) ? "ext" : "interactive";
        at_printf("\r\n+CLAW:captouch,mode=%s,running...\r\n", mode);
        lua_captouch_run(mode);
        at_printf(ATCMD_OK_END_STR);
        return 1;
    }
#endif

    /* ---- lcdc ---- */
    if (strcmp(sub, "lcdc") == 0) {
#if LUA_MOD_ENABLE_LCDC
        const char *if_mode = arg2[0] ? arg2 : "";
        const char *panel   = arg3[0] ? arg3 : "";
        if (if_mode[0] == '\0' || panel[0] == '\0') {
            at_printf("\r\n+CLAW:usage: AT+CLAW=lcdc,<rgb,st7262|srgb,st7272a|mcu,ili9806> \r\n");
            at_printf(ATCMD_ERROR_END_STR, 1);
            return 1;
        }
        at_printf("\r\n+CLAW:lcdc,running %s %s test...\r\n", if_mode, panel);
        lua_lcdc_run(if_mode, panel);
        at_printf(ATCMD_OK_END_STR);
#else
        at_printf("\r\n+CLAW:lcdc,LCDC module disabled\r\n");
        at_printf(ATCMD_ERROR_END_STR, 1);
#endif
        return 1;
    }

    /* ---- display_lcdc (pure-C ST7701P RGB 480x480 three-colour flush) ---- */
    if (strcmp(sub, "display_lcdc") == 0) {
#if LUA_MOD_ENABLE_DISPLAY
        at_printf("\r\n+CLAW:display_lcdc,running ST7701P RGB 480x480 test...\r\n");
        display_lcdc_run();
        at_printf(ATCMD_OK_END_STR);
#else
        at_printf("\r\n+CLAW:display_lcdc,DISPLAY module disabled\r\n");
        at_printf(ATCMD_ERROR_END_STR, 1);
#endif
        return 1;
    }

    /* ---- display_bench (Lua/LVGL render-path FPS bench) --------------------
     * AT+CLAW=display_bench[,frames][,device]
     *   frames : animated frames to render (default 180)
     *   device : board.json device id (default display_lcdc_rgb_st7701p)
     * Drives require('display')→init→clear/fill_circle→present_full and prints
     * "[bench] N frames / M ms = F fps".  Screen blanks when the bench state is
     * torn down (sentinel __gc).  NOTE: fails with "already in use" if a REPL
     * session still owns the display — exit the REPL first. */
    if (strcmp(sub, "display_bench") == 0) {
#if LUA_MOD_ENABLE_DISPLAY
        int frames = (arg2 && arg2[0]) ? atoi(arg2) : 0;
        const char *dev = (arg3 && arg3[0]) ? arg3 : NULL;
        at_printf("\r\n+CLAW:display_bench,running Lua/LVGL FPS bench...\r\n");
        display_lvgl_bench_run(frames, dev);
        at_printf(ATCMD_OK_END_STR);
#else
        at_printf("\r\n+CLAW:display_bench,DISPLAY module disabled\r\n");
        at_printf(ATCMD_ERROR_END_STR, 1);
#endif
        return 1;
    }

    /* ---- display_static (Lua/LVGL single-buffer contention probe) ---------
     * AT+CLAW=display_static[,hold_ms][,device]
     *   hold_ms : ms to hold each solid frame with NO CPU writes (default 3000)
     *   device  : board.json device id (default display_lcdc_rgb_st7701p)
     * Shows solid red/green/blue via the SAME require('display')→init→clear→
     * present_full path as display_bench, but idles between frames so the DMA
     * scans the framebuffer undisturbed.  Clean static frame + torn animation
     * ⇒ single-buffer CPU/DMA contention.  Same REPL-ownership caveat as bench. */
    if (strcmp(sub, "display_static") == 0) {
#if LUA_MOD_ENABLE_DISPLAY
        int hold = (arg2 && arg2[0]) ? atoi(arg2) : 0;
        const char *dev = (arg3 && arg3[0]) ? arg3 : NULL;
        at_printf("\r\n+CLAW:display_static,showing R/G/B static frames...\r\n");
        display_lvgl_static_run(hold, dev);
        at_printf(ATCMD_OK_END_STR);
#else
        at_printf("\r\n+CLAW:display_static,DISPLAY module disabled\r\n");
        at_printf(ATCMD_ERROR_END_STR, 1);
#endif
        return 1;
    }

    /* ---- speaker ---- */
    if (strcmp(sub, "speaker") == 0) {
#if LUA_MOD_ENABLE_AUDIO
        const char *mode = arg2[0] ? arg2 : "all";
        const char *vol  = arg3[0] ? arg3 : "";
        at_printf("\r\n+CLAW:speaker,running test (mode=");
        at_printf(mode);
        at_printf(")...\r\n");
        lua_speaker_run(mode, vol);
        at_printf(ATCMD_OK_END_STR);
#else
        at_printf("\r\n+CLAW:speaker,AUDIO module disabled\r\n");
        at_printf(ATCMD_ERROR_END_STR, 1);
#endif
        return 1;
    }

    /* ---- dmic ---- */
    if (strcmp(sub, "dmic") == 0) {
#if LUA_MOD_ENABLE_AUDIO
        const char *vol = arg2[0] ? arg2 : "0.2";
        at_printf("\r\n+CLAW:dmic,running SNR/THD test (vol=");
        at_printf(vol);
        at_printf(")...\r\n");
        lua_dmic_run(vol);
        at_printf(ATCMD_OK_END_STR);
#else
        at_printf("\r\n+CLAW:dmic,AUDIO module disabled\r\n");
        at_printf(ATCMD_ERROR_END_STR, 1);
#endif
        return 1;
    }

    /* ---- rec: record DMIC to WAV ---- */
    if (strcmp(sub, "rec") == 0) {
#if LUA_MOD_ENABLE_AUDIO
        const char *path = arg2[0] ? arg2 : "vfs:rec.wav";
        int duration_ms  = arg3[0] ? atoi(arg3) : 5000;
        if (duration_ms <= 0) duration_ms = 5000;
        at_printf("\r\n+CLAW:rec,recording %d ms -> %s\r\n", duration_ms, path);
        lua_audio_rec_run(path, duration_ms);
        at_printf(ATCMD_OK_END_STR);
#else
        at_printf("\r\n+CLAW:rec,AUDIO module disabled\r\n");
        at_printf(ATCMD_ERROR_END_STR, 1);
#endif
        return 1;
    }

    /* ---- play: play WAV file ---- */
    if (strcmp(sub, "play") == 0) {
#if LUA_MOD_ENABLE_AUDIO
        const char *path = arg2[0] ? arg2 : "vfs:rec.wav";
        at_printf("\r\n+CLAW:play,playing %s\r\n", path);
        lua_audio_play_run(path);
        at_printf(ATCMD_OK_END_STR);
#else
        at_printf("\r\n+CLAW:play,AUDIO module disabled\r\n");
        at_printf(ATCMD_ERROR_END_STR, 1);
#endif
        return 1;
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
        return 1;
    }

    /* ---- storage (SD card / LittleFS VFS) ----
     * AT+CLAW=storage,info              — root dir + free space
     * AT+CLAW=storage,write[,path[,data]] — write test file
     * AT+CLAW=storage,read[,path]       — read and print file
     * AT+CLAW=storage,list[,path]       — list directory
     * AT+CLAW=storage,remove[,path]     — delete file */
    if (strcmp(sub, "storage") == 0) {
#if LUA_MOD_ENABLE_STORAGE
        if (strcmp(arg2, "info") == 0 || arg2[0] == '\0') {
            at_printf("\r\n+CLAW:storage,querying info...\r\n");
            lua_storage_run_info();
            at_printf(ATCMD_OK_END_STR);

        } else if (strcmp(arg2, "write") == 0) {
            /* AT+CLAW=storage,write[,path[,data]] */
            const char *arg4 = (argc >= 5 && argv[4] && argv[4][0]) ? argv[4] : "";
            at_printf("\r\n+CLAW:storage,write path=%s\r\n", arg3[0] ? arg3 : "(default)");
            lua_storage_run_write(arg3[0] ? arg3 : NULL, arg4[0] ? arg4 : NULL);
            at_printf(ATCMD_OK_END_STR);

        } else if (strcmp(arg2, "read") == 0) {
            at_printf("\r\n+CLAW:storage,read path=%s\r\n", arg3[0] ? arg3 : "(default)");
            lua_storage_run_read(arg3[0] ? arg3 : NULL);
            at_printf(ATCMD_OK_END_STR);

        } else if (strcmp(arg2, "list") == 0) {
            at_printf("\r\n+CLAW:storage,list path=%s\r\n", arg3[0] ? arg3 : "(root)");
            lua_storage_run_list(arg3[0] ? arg3 : NULL);
            at_printf(ATCMD_OK_END_STR);

        } else if (strcmp(arg2, "remove") == 0) {
            at_printf("\r\n+CLAW:storage,remove path=%s\r\n", arg3[0] ? arg3 : "(default)");
            lua_storage_run_remove(arg3[0] ? arg3 : NULL);
            at_printf(ATCMD_OK_END_STR);

        } else {
            at_printf("\r\n+CLAW:usage: AT+CLAW=storage,<info|write|read|list|remove>\r\n");
            at_printf("+CLAW:  info            — SD/vfs root + free space\r\n");
            at_printf("+CLAW:  write[,path[,data]] — write file (default claw_test.txt)\r\n");
            at_printf("+CLAW:  read[,path]     — read and print file\r\n");
            at_printf("+CLAW:  list[,path]     — list directory entries\r\n");
            at_printf("+CLAW:  remove[,path]   — delete file\r\n");
            at_printf(ATCMD_ERROR_END_STR, 1);
        }
#else
        at_printf("\r\n+CLAW:storage,STORAGE module disabled\r\n");
        at_printf(ATCMD_ERROR_END_STR, 1);
#endif
        return 1;
    }

    return 0;  /* sub not recognised */
}

#else /* !LUA_DRIVER_TESTS_ENABLED */

int handle_cmd_hw_test(u16 argc, char **argv, const char *sub,
                       const char *arg2, const char *arg3)
{
    (void)argc; (void)argv; (void)sub; (void)arg2; (void)arg3;
    return 0;
}

#endif /* LUA_DRIVER_TESTS_ENABLED */

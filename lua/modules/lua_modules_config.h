/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * lua_modules_config.h — per-module enable switches for the Lua runtime.
 *
 * Single source of truth for which Lua C modules are compiled and registered.
 * Each LUA_MOD_ENABLE_<NAME> macro can be overridden from the build system
 * (CMake passes e.g. -DLUA_MOD_ENABLE_SPI=0); when a build does not define it,
 * the default below applies. The same macro gates BOTH the CMake source list
 * (so disabled modules cost no flash) AND the registry table entry in
 * lua_module_registry.c (so disabled modules cost no RAM / registration time).
 *
 * Layering convention (see lua_module_registry.h):
 *   - "SW"  modules: pure-software helpers (cap, event, file, sys, cjson,
 *                    timer, udp, wifi).
 *   - "HW"  modules: hardware-peripheral drivers (gpio, i2c, spi, uart, pwm,
 *                    rtc, ir, lcdc, audio).
 */
#pragma once

/* ---- Software (SW) modules ------------------------------------------------ */
#ifndef LUA_MOD_ENABLE_CAP
#define LUA_MOD_ENABLE_CAP    1   /* cap.* — bridge to claw capability calls   */
#endif
#ifndef LUA_MOD_ENABLE_EVENT
#define LUA_MOD_ENABLE_EVENT  1   /* event.* — publish events to claw router   */
#endif
#ifndef LUA_MOD_ENABLE_FILE
#define LUA_MOD_ENABLE_FILE   1   /* file.* — VFS read/write/list              */
#endif
#ifndef LUA_MOD_ENABLE_SYS
#define LUA_MOD_ENABLE_SYS    1   /* sys.* — sleep/time/reboot helpers         */
#endif
#ifndef LUA_MOD_ENABLE_CJSON
#define LUA_MOD_ENABLE_CJSON  1   /* cjson.* — JSON encode/decode              */
#endif
#ifndef LUA_MOD_ENABLE_TIMER
#define LUA_MOD_ENABLE_TIMER  1   /* timer.* — software timers                 */
#endif
#ifndef LUA_MOD_ENABLE_UDP
#define LUA_MOD_ENABLE_UDP    1   /* udp.* — UDP sockets (preload / lazy)      */
#endif
#ifndef LUA_MOD_ENABLE_WIFI
#define LUA_MOD_ENABLE_WIFI   1   /* wifi.* — Wi-Fi status / control           */
#endif

/* ---- Hardware (HW) driver modules ----------------------------------------- */
#ifndef LUA_MOD_ENABLE_GPIO
#define LUA_MOD_ENABLE_GPIO   1
#endif
#ifndef LUA_MOD_ENABLE_I2C
#define LUA_MOD_ENABLE_I2C    1
#endif
#ifndef LUA_MOD_ENABLE_SPI
#define LUA_MOD_ENABLE_SPI    1
#endif
#ifndef LUA_MOD_ENABLE_UART
#define LUA_MOD_ENABLE_UART   1
#endif
#ifndef LUA_MOD_ENABLE_PWM
#define LUA_MOD_ENABLE_PWM    1
#endif
#ifndef LUA_MOD_ENABLE_RTC
#define LUA_MOD_ENABLE_RTC    1
#endif
#ifndef LUA_MOD_ENABLE_IR
#define LUA_MOD_ENABLE_IR     1
#endif
#ifndef LUA_MOD_ENABLE_LCDC
#define LUA_MOD_ENABLE_LCDC   1
#endif
#ifndef LUA_MOD_ENABLE_AUDIO
#define LUA_MOD_ENABLE_AUDIO  1
#endif
#ifndef LUA_MOD_ENABLE_ADC
#define LUA_MOD_ENABLE_ADC    1
#endif
#ifndef LUA_MOD_ENABLE_THERMAL
#define LUA_MOD_ENABLE_THERMAL 1
#endif
#ifndef LUA_MOD_ENABLE_TOUCH
#define LUA_MOD_ENABLE_TOUCH  1
#endif

/* ---- USB host modules (uvc / msc) ----------------------------------------- *
 * Per-module switches like every other module, but their default tracks the
 * project's USB Host Kconfig: a Lua USB class binding only makes sense when the
 * matching host class is built into the image. LUA_MOD_ENABLE_USB_UVC follows
 * CONFIG_USBH_UVC and _USB_MSC follows CONFIG_USBH_MSC. CMake passes the same
 * value as a -D for the lua library; this header is the fallback for other TUs
 * (e.g. cap_atcmd.c) that see the Kconfig macros via platform_autoconf.h.      */
/* ---- Lua driver test provision scripts ------------------------------------ *
 * When CONFIG_LUA_DRIVER_TESTS=y the build compiles the per-driver
 * *_test_provision.c files and embeds the test Lua scripts.  CMake passes
 * -DLUA_DRIVER_TESTS_ENABLED=1/0; this header provides a fallback for TUs
 * (e.g. cap_atcmd.c) that see Kconfig macros via platform_autoconf.h.        */
#ifndef LUA_DRIVER_TESTS_ENABLED
#  if defined(CONFIG_LUA_DRIVER_TESTS)
#    define LUA_DRIVER_TESTS_ENABLED 1
#  else
#    define LUA_DRIVER_TESTS_ENABLED 0
#  endif
#endif

#ifndef LUA_MOD_ENABLE_USB_UVC
#  if defined(CONFIG_USBH_UVC)
#    define LUA_MOD_ENABLE_USB_UVC 1
#  else
#    define LUA_MOD_ENABLE_USB_UVC 0
#  endif
#endif
#ifndef LUA_MOD_ENABLE_USB_MSC
#  if defined(CONFIG_USBH_MSC)
#    define LUA_MOD_ENABLE_USB_MSC 1
#  else
#    define LUA_MOD_ENABLE_USB_MSC 0
#  endif
#endif

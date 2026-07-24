/*
 * Copyright (c) 2026 Realtek Semiconductor Corp.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * lua_module_registry.c — the one table of Lua C modules.
 *
 * See lua_module_registry.h for the rationale. Add a module here exactly once;
 * linit.c (REPL), cap_skill_mgr.c (sandbox) and lua_main.c (provisioning) all
 * consume this table, so there is no second list to keep in sync.
 */
#include "lua_module_registry.h"
#include "lua_modules_config.h"

#include "lualib.h"
#include "lauxlib.h"
#include "os_wrapper.h"
#include <string.h>

/* Must match CLAW_LUA_DISABLED_MODULES_SIZE in claw_config.h.
 * The _Static_assert in lua_module_registry_init() catches divergence. */
#define LUA_DISABLED_BUF_SIZE 256

/* ---- luaopen_* forward declarations (gated by lua_modules_config.h) ---- */
#if LUA_MOD_ENABLE_GPIO
LUAMOD_API int luaopen_gpio(lua_State *L);
LUAMOD_API int luaopen_button(lua_State *L);   /* button subsystem (shares gpio module switch) */
#endif
#if LUA_MOD_ENABLE_I2C
LUAMOD_API int luaopen_i2c(lua_State *L);
#endif
#if LUA_MOD_ENABLE_SPI
LUAMOD_API int luaopen_spi(lua_State *L);
#endif
#if LUA_MOD_ENABLE_DISPLAY
LUAMOD_API int luaopen_display(lua_State *L);
#endif
#if LUA_MOD_ENABLE_LVGL
LUAMOD_API int luaopen_lvgl(lua_State *L);
#endif
#if LUA_MOD_ENABLE_UART
LUAMOD_API int luaopen_uart(lua_State *L);
#endif
#if LUA_MOD_ENABLE_PWM
LUAMOD_API int luaopen_pwm(lua_State *L);
#endif
#if LUA_MOD_ENABLE_RTC
LUAMOD_API int luaopen_rtc(lua_State *L);
#endif
#if LUA_MOD_ENABLE_IR
LUAMOD_API int luaopen_ir(lua_State *L);
#endif
#if LUA_MOD_ENABLE_LCDC
LUAMOD_API int luaopen_lcdc(lua_State *L);
#endif
#if LUA_MOD_ENABLE_ADC
LUAMOD_API int luaopen_adc(lua_State *L);
#endif
#if LUA_MOD_ENABLE_THERMAL
LUAMOD_API int luaopen_thermal(lua_State *L);
#endif
#if LUA_MOD_ENABLE_CAPTOUCH
LUAMOD_API int luaopen_captouch(lua_State *L);   /* on-chip CapTouch self-cap keys (NOT the GT911 panel; that is `touch`) */
#endif
#if LUA_MOD_ENABLE_TOUCH
LUAMOD_API int luaopen_touch(lua_State *L);       /* GT911 I2C touch panel (st7701p 480x480) */
#endif
#if LUA_MOD_ENABLE_BASICTIMER
LUAMOD_API int luaopen_basictimer(lua_State *L);
#endif
#if LUA_MOD_ENABLE_AUDIO
LUAMOD_API int luaopen_audio(lua_State *L);
#endif
#if LUA_MOD_ENABLE_LED_STRIP
LUAMOD_API int luaopen_led_strip(lua_State *L);
#endif
#if LUA_MOD_ENABLE_ENVIRONMENTAL_SENSOR
LUAMOD_API int luaopen_environmental_sensor(lua_State *L);
#endif
#if LUA_MOD_ENABLE_LIGHT_SENSOR
LUAMOD_API int luaopen_light_sensor(lua_State *L);
#endif
#if LUA_MOD_ENABLE_IMU
LUAMOD_API int luaopen_imu(lua_State *L);   /* MPU-6050 6-axis IMU (accel+gyro+temp) over I2C */
#endif
#if LUA_MOD_ENABLE_STORAGE
LUAMOD_API int luaopen_storage(lua_State *L);
#endif
#if LUA_MOD_ENABLE_MAGNETOMETER
LUAMOD_API int luaopen_magnetometer(lua_State *L);  /* BMM150 3-axis magnetometer over I2C */
#endif

/* ---- provision_fn declarations — only exist when test scripts are compiled ---- */
#if LUA_DRIVER_TESTS_ENABLED
extern void lua_driver_uart_provision(void);
extern void lua_module_environmental_sensor_provision(void);
extern void lua_module_light_sensor_provision(void);
extern void lua_module_imu_provision(void);
extern void lua_module_magnetometer_provision(void);
extern void lua_driver_basictimer_provision(void);
extern void lua_driver_gpio_provision(void);
extern void lua_driver_i2c_provision(void);
extern void lua_driver_spi_provision(void);
extern void lua_driver_pwm_provision(void);
extern void lua_driver_rtc_provision(void);
extern void lua_driver_ir_provision(void);
extern void lua_driver_lcdc_provision(void);
extern void lua_driver_adc_provision(void);
extern void lua_driver_thermal_provision(void);
extern void lua_driver_captouch_provision(void);
extern void lua_driver_audio_speaker_provision(void);
extern void lua_driver_audio_dmic_provision(void);
static void audio_provision_all(void)
{
    lua_driver_audio_speaker_provision();
    lua_driver_audio_dmic_provision();
}
#endif

#if LUA_MOD_ENABLE_CAP
LUAMOD_API int luaopen_cap(lua_State *L);
#endif
#if LUA_MOD_ENABLE_EVENT
LUAMOD_API int luaopen_event(lua_State *L);
#endif
#if LUA_MOD_ENABLE_THREAD
LUAMOD_API int luaopen_thread(lua_State *L);
#endif
#if LUA_MOD_ENABLE_FILE
LUAMOD_API int luaopen_file(lua_State *L);
#endif
#if LUA_MOD_ENABLE_SYS
LUAMOD_API int luaopen_sys(lua_State *L);
#endif
#if LUA_MOD_ENABLE_CJSON
LUAMOD_API int luaopen_cjson(lua_State *L);
#endif
#if LUA_MOD_ENABLE_TIMER
LUAMOD_API int luaopen_timer(lua_State *L);
#endif
#if LUA_MOD_ENABLE_UDP
LUAMOD_API int luaopen_udp(lua_State *L);
#endif
#if LUA_MOD_ENABLE_WIFI
LUAMOD_API int luaopen_wifi(lua_State *L);
#endif
#if LUA_MOD_ENABLE_USB_UVC
LUAMOD_API int luaopen_usb_uvc(lua_State *L);
#endif
#if LUA_MOD_ENABLE_USB_MSC
LUAMOD_API int luaopen_usb_msc(lua_State *L);
#endif

#define REPL    LUA_MOD_ENV_REPL
#define SKILL   LUA_MOD_ENV_SKILL
#define TIMER   LUA_MOD_ENV_TIMER

/* The single registry. Each entry's env_flag picks which environments may
 *   require() it (REPL = AT+CLAW=lua_repl interactive shell, SKILL = LLM-authored
 *   scripts run via lua_run / lua_run_async).
 *   - REPL | SKILL  — module is safe and intended for both humans and LLM:
 *     gpio i2c rtc sys timer file cap pwm usb_uvc usb_msc audio udp spi
 *     basictimer ir wifi adc thermal captouch uart.
 *   - REPL only     — driver-level peripherals exposed for hand testing but
 *     not yet curated for autonomous LLM use (no LLM-facing doc yet):
 *     lcdc.
 *   - REPL | SKILL  — cjson: JSON is useful in both environments. The old
 *     comment "REPL has its own json via lua_main.c" referred to a hand-rolled
 *     function that was removed; cjson now serves both contexts.
 *
 *   Audio + udp need SKILL because the streaming/sock APIs were designed for
 *   skill scripts (long-running record/play, timer-callback-safe UDP fds).
 *   Whatever is exposed here MUST also appear in
 *   cap_lua/skills/builtin_lua_modules/SKILL.md, otherwise LLM is misled. */
/* PROV(fn): provision_fn is only valid when test scripts are compiled in. */
#if LUA_DRIVER_TESTS_ENABLED
#  define PROV(fn) fn
#else
#  define PROV(fn) NULL
#endif

/* Runtime filter state: set once at boot, queried on every install() call. */
static lua_chip_filter_fn s_chip_filter = NULL;
/* Own copy of the disabled CSV — never a pointer alias into another buffer.
 * Protected by s_disabled_mutex so HTTP-task writes and Lua-task reads
 * cannot race. */
static char           s_disabled_buf[LUA_DISABLED_BUF_SIZE];
static const char    *s_disabled_csv = s_disabled_buf;
static rtos_mutex_t   s_disabled_mutex = NULL;

static void disabled_lock(void)
{
    rtos_mutex_take(s_disabled_mutex, 0xFFFFFFFFUL);
}

static void disabled_unlock(void)
{
    rtos_mutex_give(s_disabled_mutex);
}

void lua_module_registry_init(void)
{
    /* Must be called once at boot, before any task that touches the registry.
     * ameba_claw_main.c calls this from phase_config() (single-threaded). */
    _Static_assert(LUA_DISABLED_BUF_SIZE == 256,
                   "LUA_DISABLED_BUF_SIZE drifted from CLAW_LUA_DISABLED_MODULES_SIZE");
    if (!s_disabled_mutex) {
        rtos_mutex_create(&s_disabled_mutex);
    }
    s_disabled_buf[0] = '\0';
}

void lua_module_registry_set_chip_filter(lua_chip_filter_fn fn)
{
    s_chip_filter = fn;
}

void lua_module_registry_set_disabled(const char *disabled_csv)
{
    disabled_lock();
    if (disabled_csv) {
        strlcpy(s_disabled_buf, disabled_csv, LUA_DISABLED_BUF_SIZE);
    } else {
        s_disabled_buf[0] = '\0';
    }
    disabled_unlock();
}

int lua_module_registry_chip_ok(const char *chip_peripheral)
{
    if (!chip_peripheral || !s_chip_filter) return 1;
    return s_chip_filter(chip_peripheral);
}

/* Scan comma-separated list for an exact token match.
 * Does NOT touch the registry's internal disabled buffer; callers may pass
 * any CSV string.  Thread-safety is the caller's responsibility for the
 * provided csv pointer. */
int lua_module_registry_csv_contains(const char *csv, const char *name)
{
    size_t nlen;
    const char *p;
    if (!csv || !csv[0] || !name || !name[0]) return 0;
    nlen = strlen(name);
    p = csv;
    while (*p) {
        const char *start;
        size_t tlen;
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        start = p;
        while (*p && *p != ',') p++;
        tlen = (size_t)(p - start);
        while (tlen > 0 && start[tlen - 1] == ' ') tlen--;
        if (tlen == nlen && strncmp(start, name, nlen) == 0) return 1;
    }
    return 0;
}

/* Check the registry's internal disabled buffer (thread-safe). */
static int disabled_csv_contains(const char *name)
{
    int result;
    disabled_lock();
    result = lua_module_registry_csv_contains(s_disabled_csv, name);
    disabled_unlock();
    return result;
}

/* Column shorthands: locked / chip_peripheral */
#define LK  1       /* locked: cannot be disabled by user */
#define UL  0       /* unlocked */
#define NC  NULL    /* no chip_peripheral constraint */

/* TIMER column: lightweight modules only — no blocking I/O, no heavy HW init.
 * audio/udp/wifi/usb omitted: initialisation cost too high for timer callback stack. */
/* Table columns: name, open_fn, provision_fn, category, load, env_flags, locked, chip_peripheral */
static const lua_module_desc_t s_modules[] = {
    /* ---- Hardware drivers ----                                                          lk  chip   */
#if LUA_MOD_ENABLE_BASICTIMER
    { "basictimer", luaopen_basictimer, PROV(lua_driver_basictimer_provision), LUA_MOD_CAT_DRV, LUA_MOD_LOAD_EAGER, REPL | SKILL,      UL, "basictimer" },
#endif
#if LUA_MOD_ENABLE_GPIO
    { "gpio",    luaopen_gpio,    PROV(lua_driver_gpio_provision),    LUA_MOD_CAT_DRV, LUA_MOD_LOAD_EAGER, REPL | SKILL | TIMER, UL, "gpio" },
    { "button",  luaopen_button,  NULL,                               LUA_MOD_CAT_DRV, LUA_MOD_LOAD_EAGER, REPL | SKILL | TIMER, UL, "gpio" },
#endif
#if LUA_MOD_ENABLE_I2C
    { "i2c",     luaopen_i2c,     PROV(lua_driver_i2c_provision),     LUA_MOD_CAT_DRV, LUA_MOD_LOAD_EAGER, REPL | SKILL | TIMER, UL, "i2c"  },
#endif
#if LUA_MOD_ENABLE_RTC
    { "rtc",     luaopen_rtc,     PROV(lua_driver_rtc_provision),     LUA_MOD_CAT_DRV, LUA_MOD_LOAD_EAGER, REPL | SKILL | TIMER, UL, "rtc"  },
#endif
#if LUA_MOD_ENABLE_SPI
    { "spi",     luaopen_spi,     PROV(lua_driver_spi_provision),     LUA_MOD_CAT_DRV, LUA_MOD_LOAD_EAGER, REPL | SKILL,          UL, "spi"  },
#endif
#if LUA_MOD_ENABLE_DISPLAY
    { "display", luaopen_display, NULL,                               LUA_MOD_CAT_DEV, LUA_MOD_LOAD_EAGER, REPL | SKILL,          UL, "lcdc" },
#endif
#if LUA_MOD_ENABLE_LVGL
    { "lvgl",    luaopen_lvgl,    NULL,                               LUA_MOD_CAT_DEV, LUA_MOD_LOAD_EAGER, REPL | SKILL,          UL, "lcdc" },
#endif
#if LUA_MOD_ENABLE_UART
    { "uart",    luaopen_uart,    PROV(lua_driver_uart_provision),    LUA_MOD_CAT_DRV, LUA_MOD_LOAD_EAGER, REPL | SKILL,          UL, "uart" },
#endif
#if LUA_MOD_ENABLE_PWM
    { "pwm",     luaopen_pwm,     PROV(lua_driver_pwm_provision),     LUA_MOD_CAT_DRV, LUA_MOD_LOAD_EAGER, REPL | SKILL,          UL, "pwm"  },
#endif
#if LUA_MOD_ENABLE_IR
    { "ir",      luaopen_ir,      PROV(lua_driver_ir_provision),      LUA_MOD_CAT_DRV, LUA_MOD_LOAD_EAGER, REPL | SKILL,          UL, "ir"   },
#endif
#if LUA_MOD_ENABLE_LCDC
    { "lcdc",    luaopen_lcdc,    PROV(lua_driver_lcdc_provision),    LUA_MOD_CAT_DRV, LUA_MOD_LOAD_EAGER, REPL,                  UL, "lcdc" },
#endif
#if LUA_MOD_ENABLE_ADC
    { "adc",     luaopen_adc,     PROV(lua_driver_adc_provision),     LUA_MOD_CAT_DRV, LUA_MOD_LOAD_EAGER, REPL | SKILL,          UL, "adc"  },
#endif
#if LUA_MOD_ENABLE_THERMAL
    { "thermal", luaopen_thermal, PROV(lua_driver_thermal_provision), LUA_MOD_CAT_DRV, LUA_MOD_LOAD_EAGER, REPL | SKILL,          UL, "thermal"  },
#endif
#if LUA_MOD_ENABLE_CAPTOUCH
    { "captouch", luaopen_captouch, PROV(lua_driver_captouch_provision), LUA_MOD_CAT_DRV, LUA_MOD_LOAD_EAGER, REPL | SKILL,       UL, "captouch" },
#endif
#if LUA_MOD_ENABLE_TOUCH
    { "touch",   luaopen_touch,   NULL,                               LUA_MOD_CAT_DEV, LUA_MOD_LOAD_EAGER, REPL | SKILL,          UL, "touch" },
#endif
#if LUA_MOD_ENABLE_AUDIO
    { "audio",   luaopen_audio,   PROV(audio_provision_all),          LUA_MOD_CAT_DRV, LUA_MOD_LOAD_EAGER, REPL | SKILL,          UL, "audio" },
#endif
#if LUA_MOD_ENABLE_LED_STRIP
    { "led_strip", luaopen_led_strip, NULL,                           LUA_MOD_CAT_DEV, LUA_MOD_LOAD_EAGER, REPL | SKILL,          UL, NC     },
#endif
#if LUA_MOD_ENABLE_ENVIRONMENTAL_SENSOR
    { "environmental_sensor", luaopen_environmental_sensor, PROV(lua_module_environmental_sensor_provision), LUA_MOD_CAT_DEV, LUA_MOD_LOAD_EAGER, REPL | SKILL, UL, "i2c" },
#endif
#if LUA_MOD_ENABLE_LIGHT_SENSOR
    { "light_sensor", luaopen_light_sensor, PROV(lua_module_light_sensor_provision), LUA_MOD_CAT_DEV, LUA_MOD_LOAD_EAGER, REPL | SKILL, UL, "i2c" },
#endif
#if LUA_MOD_ENABLE_IMU
    { "imu",     luaopen_imu,     PROV(lua_module_imu_provision),     LUA_MOD_CAT_DEV, LUA_MOD_LOAD_EAGER, REPL | SKILL,          UL, "i2c"  },
#endif
#if LUA_MOD_ENABLE_MAGNETOMETER
    { "magnetometer", luaopen_magnetometer, PROV(lua_module_magnetometer_provision), LUA_MOD_CAT_DEV, LUA_MOD_LOAD_EAGER, REPL | SKILL, UL, "i2c" },
#endif
#if LUA_MOD_ENABLE_USB_UVC
    { "usb_uvc", luaopen_usb_uvc, NULL,                               LUA_MOD_CAT_DRV, LUA_MOD_LOAD_EAGER, REPL | SKILL,          UL, "usb"  },
#endif
#if LUA_MOD_ENABLE_USB_MSC
    { "usb_msc", luaopen_usb_msc, NULL,                               LUA_MOD_CAT_DRV, LUA_MOD_LOAD_EAGER, REPL | SKILL,          UL, "usb"  },
#endif

    /* ---- Software modules (SW) — no chip_peripheral constraint ---- */
#if LUA_MOD_ENABLE_SYS
    { "sys",     luaopen_sys,     NULL, LUA_MOD_CAT_SW, LUA_MOD_LOAD_EAGER,   REPL | SKILL | TIMER, LK, NC },
#endif
#if LUA_MOD_ENABLE_TIMER
    { "timer",   luaopen_timer,   NULL, LUA_MOD_CAT_SW, LUA_MOD_LOAD_EAGER,   REPL | SKILL,          UL, NC },
#endif
#if LUA_MOD_ENABLE_FILE
    { "file",    luaopen_file,    NULL, LUA_MOD_CAT_SW, LUA_MOD_LOAD_EAGER,   REPL | SKILL | TIMER,  UL, NC },
#endif
#if LUA_MOD_ENABLE_CAP
    { "cap",     luaopen_cap,     NULL, LUA_MOD_CAT_SW, LUA_MOD_LOAD_EAGER,   REPL | SKILL | TIMER,  LK, NC },
#endif
#if LUA_MOD_ENABLE_CJSON
    { "cjson",   luaopen_cjson,   NULL, LUA_MOD_CAT_SW, LUA_MOD_LOAD_EAGER,   REPL | SKILL | TIMER,  UL, NC },
#endif
#if LUA_MOD_ENABLE_EVENT
    { "event",   luaopen_event,   NULL, LUA_MOD_CAT_SW, LUA_MOD_LOAD_EAGER,   REPL | SKILL,          LK, NC },
#endif
#if LUA_MOD_ENABLE_THREAD
    /* SKILL only for now (D3 in lua_module_thread_architecture.md): job orchestration
     * in the REPL has unresolved shared-budget edge cases, revisit later.
     * provision_fn left NULL here (that slot only fires for REPL-tagged
     * modules, see lua_module_registry_provision_all below) — this module's
     * test script is provisioned by an explicit call from lua_task()
     * instead, so it lands without also exposing "thread" to the REPL. */
    { "thread",  luaopen_thread,  NULL,                      LUA_MOD_CAT_SW, LUA_MOD_LOAD_EAGER, SKILL,                 UL, NC },
#endif
#if LUA_MOD_ENABLE_WIFI
    { "wifi",    luaopen_wifi,    NULL, LUA_MOD_CAT_SW, LUA_MOD_LOAD_EAGER,   REPL | SKILL,          UL, NC },
#endif
#if LUA_MOD_ENABLE_UDP
    { "udp",     luaopen_udp,     NULL, LUA_MOD_CAT_SW, LUA_MOD_LOAD_PRELOAD, REPL | SKILL,          LK, NC },
#endif
#if LUA_MOD_ENABLE_STORAGE
    { "storage", luaopen_storage, NULL, LUA_MOD_CAT_SW, LUA_MOD_LOAD_EAGER,   REPL | SKILL,          UL, NC },
#endif
};

const lua_module_desc_t *lua_module_registry(size_t *count)
{
    if (count) {
        *count = sizeof(s_modules) / sizeof(s_modules[0]);
    }
    return s_modules;
}

void lua_module_registry_install(lua_State *L, unsigned env_flag)
{
    size_t i, n = 0;
    const lua_module_desc_t *t = lua_module_registry(&n);

    for (i = 0; i < n; i++) {
        if (!(t[i].env_flags & env_flag)) {
            continue;
        }
        /* Chip filter: skip HW modules not supported by the current board chip */
        if (t[i].chip_peripheral && s_chip_filter) {
            if (!s_chip_filter(t[i].chip_peripheral)) continue;
        }
        /* User disabled list: skip unless the module is locked */
        if (!t[i].locked && disabled_csv_contains(t[i].name)) {
            continue;
        }
        /* Lazy (preload) loading only applies in the REPL, which keeps a full
         * package table. The sandbox installs its modules eagerly. Test the
         * REPL bit (not strict equality) so an OR-combined env_flag still
         * routes preload modules through package.preload, consistent with the
         * bitwise membership check above. */
        if (t[i].load == LUA_MOD_LOAD_PRELOAD && (env_flag & LUA_MOD_ENV_REPL)) {
            luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
            lua_pushcfunction(L, t[i].open_fn);
            lua_setfield(L, -2, t[i].name);
            lua_pop(L, 1);
        } else {
            luaL_requiref(L, t[i].name, t[i].open_fn, 1);
            lua_pop(L, 1);
        }
    }
}

void lua_module_registry_provision_all(void)
{
    /* Initialise hardware driver locks before any concurrent Lua execution
     * can start.  These must be created in the single-threaded boot phase. */
#if LUA_MOD_ENABLE_BASICTIMER
    extern void lua_driver_basictimer_init(void);
    lua_driver_basictimer_init();
#endif
#if LUA_MOD_ENABLE_GPIO
    extern void lua_driver_gpio_init(void);
    extern void lua_driver_gpio_button_init(void);
    lua_driver_gpio_init();
    lua_driver_gpio_button_init();
#endif
#if LUA_MOD_ENABLE_I2C
    extern void lua_driver_i2c_init(void);
    lua_driver_i2c_init();
#endif
#if LUA_MOD_ENABLE_UART
    extern void lua_driver_uart_init(void);
    lua_driver_uart_init();
#endif
#if LUA_MOD_ENABLE_IMU
    extern void lua_module_imu_init(void);
    lua_module_imu_init();
#endif
#if LUA_MOD_ENABLE_MAGNETOMETER
    extern void lua_module_magnetometer_init(void);
    lua_module_magnetometer_init();
#endif
#if LUA_MOD_ENABLE_IR
    extern void lua_driver_ir_init(void);
    lua_driver_ir_init();
#endif
#if LUA_MOD_ENABLE_RTC
    extern void lua_driver_rtc_init(void);
    lua_driver_rtc_init();
#endif
#if LUA_MOD_ENABLE_PWM
    extern void lua_driver_pwm_init(void);
    lua_driver_pwm_init();
#endif
#if LUA_MOD_ENABLE_SPI
    extern void lua_driver_spi_init(void);
    lua_driver_spi_init();
#endif
#if LUA_MOD_ENABLE_DISPLAY
    extern void lua_module_display_init(void);
    lua_module_display_init();
#endif
#if LUA_MOD_ENABLE_LVGL
    extern void lua_module_lvgl_init(void);
    lua_module_lvgl_init();
#endif

    size_t i, n = 0;
    const lua_module_desc_t *t = lua_module_registry(&n);

    for (i = 0; i < n; i++) {
        if ((t[i].env_flags & LUA_MOD_ENV_REPL) && t[i].provision_fn) {
            t[i].provision_fn();
        }
    }
}

set(private_sources)
set(private_includes)
set(private_definitions)
set(private_compile_options)

# ---- Test provision: embedded Lua peripheral driver test scripts ----
# Embed each test .lua as a C-string header at configure time. Gated by
# CONFIG_CLAW_ENABLE_TESTS: when off, the headers are not generated and the
# *_test_provision.c sources that #include them are dropped below (so no dangling
# include). The C guards read CONFIG_CLAW_ENABLE_TESTS directly (via
# platform_autoconf.h), so no compile-def is passed here.
if(CONFIG_CLAW_ENABLE_TESTS)
    set(_lua_to_cstr "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_to_cstr.py")

    macro(_embed_lua _src_lua _out_h _sym)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_src_lua}")
        execute_process(
            COMMAND python3 "${_lua_to_cstr}" "${_src_lua}" "${_out_h}" "${_sym}"
            RESULT_VARIABLE _embed_result
        )
        if(NOT _embed_result EQUAL 0)
            message(FATAL_ERROR "lua_to_cstr.py failed for ${_src_lua}")
        endif()
    endmacro()

    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_driver_uart/test/test_uart.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/uart_test_lua.h" "s_uart_test_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_driver_i2c/test/i2c_sh1106.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/i2c_sh1106_lua.h" "s_i2c_sh1106_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_driver_i2c/lib/oled_sh1106.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/oled_sh1106_lua.h" "s_oled_sh1106_lib")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_driver_pwm/test/test_pwm.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/pwm_test_lua.h" "s_pwm_test_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_driver_pwm/test/test_servo.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/servo_test_lua.h" "s_servo_test_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_driver_pwm/docs/pwm.md"
        "${CMAKE_CURRENT_BINARY_DIR}/pwm_doc_md.h" "s_pwm_doc_md")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_driver_adc/test/test_adc.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/adc_test_lua.h" "s_adc_test_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_driver_thermal/test/test_thermal.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/thermal_test_lua.h" "s_thermal_test_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_driver_captouch/test/test_captouch.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/captouch_test_lua.h" "s_captouch_test_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_module_basictimer/test/test_basictimer.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/basictimer_test_lua.h" "s_basictimer_test_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_module_led_strip/test/test_led_strip.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/test_led_strip_lua.h" "s_led_strip_test_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_module_thread/test/test_thread_sync.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/test_thread_sync_lua.h" "s_thread_test_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_module_thread/test/thread_full_flow_parent.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/thread_full_flow_parent_lua.h" "s_thread_full_flow_parent_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_module_thread/test/thread_full_flow_child_producer.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/thread_full_flow_child_producer_lua.h" "s_thread_full_flow_child_producer_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_module_thread/test/thread_full_flow_child_consumer.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/thread_full_flow_child_consumer_lua.h" "s_thread_full_flow_child_consumer_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_module_thread/test/test_thread_stop.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/test_thread_stop_lua.h" "s_thread_stop_test_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_module_thread/test/thread_stop_child_blocker.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/thread_stop_child_blocker_lua.h" "s_thread_stop_child_blocker_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_module_thread/test/test_thread_job_named_replace.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/test_thread_job_named_replace_lua.h" "s_thread_job_named_replace_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_module_thread/test/test_thread_job_exclusive.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/test_thread_job_exclusive_lua.h" "s_thread_job_exclusive_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_module_environmental_sensor/test/test_environmental_sensor.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/environmental_sensor_test_lua.h" "s_environmental_sensor_test_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_module_light_sensor/test/test_light_sensor.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/light_sensor_test_lua.h" "s_light_sensor_test_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_module_imu/test/test_imu.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/imu_test_lua.h" "s_imu_test_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_module_magnetometer/test/test_magnetometer.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/magnetometer_test_lua.h" "s_magnetometer_test_script")

    set(_lcdc_test_dir "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_driver_lcdc/test")
    _embed_lua(
        "${_lcdc_test_dir}/test_lcdc_rgb_st7262.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/lcdc_rgb_st7262_lua.h" "s_lcdc_rgb_st7262_script")
    _embed_lua(
        "${_lcdc_test_dir}/test_lcdc_mcu_ili9806.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/lcdc_mcu_ili9806_lua.h" "s_lcdc_mcu_ili9806_script")
    _embed_lua(
        "${_lcdc_test_dir}/test_lcdc_srgb_st7272a.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/lcdc_srgb_st7272a_lua.h" "s_lcdc_srgb_st7272a_script")
    _embed_lua(
        "${_lcdc_test_dir}/test_lcdc_rgb_st7701p_touch_gt711.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/lcdc_rgb_st7701p_touch_gt711_lua.h"
        "s_lcdc_rgb_st7701p_touch_gt711_script")

    ameba_list_append(private_includes ${CMAKE_CURRENT_BINARY_DIR})
endif()

# Add binary dir for the generated test-script headers (no-op when tests off).
ameba_list_append(private_includes ${CMAKE_CURRENT_BINARY_DIR})

ameba_list_append(private_includes
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/interpreter
    modules
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_modules/claw_event_dispatcher/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_modules/claw_cap/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_capabilities/cap_lua/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_capabilities/cap_time/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_modules/claw_agent/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    modules/lua_driver_gpio/src
    modules/lua_driver_uart/src
    modules/lua_driver_audio/src
    modules/lua_driver_i2c/src
    modules/lua_driver_spi/src
    modules/lua_driver_ir/src
    modules/lua_driver_rtc/src
    modules/lua_driver_pwm/src
    modules/lua_driver_adc/src
    modules/lua_driver_thermal/src
    modules/lua_driver_captouch/src
    modules/lua_driver_touch/src
    modules/lua_module_environmental_sensor/src
    modules/lua_module_light_sensor/src
    modules/lua_module_imu/src
    modules/lua_module_magnetometer/src
    modules/lua_module_magnetometer/src/backend
    modules/lua_module_magnetometer/src/backend/bmm150
    modules/lua_driver_uvc/src
    modules/lua_driver_msc/src
    modules/lua_driver_lcdc/src
    modules/lua_module_basictimer/src
    ${c_COMPONENT_DIR}/network/cJSON
    ${c_COMPONENT_DIR}/usb/host/core
    ${c_COMPONENT_DIR}/usb/host/uvc
    ${c_COMPONENT_DIR}/usb/host/msc
    ${c_COMPONENT_DIR}/usb/common
    ${c_COMPONENT_DIR}/file_system/fatfs/r0.14b/include
)

# ---- Core: vendored Lua 5.3 interpreter (interpreter/) + Ameba glue + registry.
# The interpreter/ subtree is unmodified upstream Lua, kept separate so it is
# easy to diff against / upgrade. linit.c, lloadlib_stub.c, lua_main.c and
# lua_repl.c are Ameba-specific glue and stay at the lua/ root. All are always
# compiled. ----
ameba_list_append(private_sources
    interpreter/lapi.c
    interpreter/lcode.c
    interpreter/lctype.c
    interpreter/ldebug.c
    interpreter/ldo.c
    interpreter/ldump.c
    interpreter/lfunc.c
    interpreter/lgc.c
    interpreter/llex.c
    interpreter/lmem.c
    interpreter/lobject.c
    interpreter/lopcodes.c
    interpreter/lparser.c
    interpreter/lstate.c
    interpreter/lstring.c
    interpreter/ltable.c
    interpreter/ltm.c
    interpreter/lundump.c
    interpreter/lvm.c
    interpreter/lzio.c
    interpreter/lauxlib.c
    interpreter/lbaselib.c
    interpreter/lstrlib.c
    interpreter/ltablib.c
    interpreter/lmathlib.c
    interpreter/lcorolib.c
    interpreter/lutf8lib.c
    interpreter/ldblib.c
    interpreter/liolib.c
    lloadlib_stub.c
    linit.c
    lua_main.c
    modules/lua_module_registry.c
)

# ---- Per-module enable switches (default ON) ----------------------------------
# Single control point for trimming the Lua module set. Override per build with
# e.g. -DLUA_MOD_ENABLE_SPI=OFF. Each switch gates BOTH the sources appended
# below AND the registry table entry in lua_module_registry.c, via the matching
# -DLUA_MOD_ENABLE_<NAME>=0/1 compile definition — so a disabled module costs no
# flash (not compiled) and no RAM (not registered).
#
# Layout: <NAME>|<source1>|<source2>|... — '|' separates fields because CMake
# treats ';' as a list separator (it would split each spec apart). Drivers
# bundle their test-provision source; software modules are single-file. The
# first field is the switch name.
# ---- Compile-time driver enables driven by ameba_claw Kconfig ----
# When a CLAW_LUA_DRV_* symbol is disabled (=n), force the corresponding
# module OFF before option() sees it.  When the symbol is absent (non-Kconfig
# build), the option() default (ON) stands unchanged.
#
# NOTE: The SDK's import_kconfig() UNSETS =n symbols instead of setting them
# to 0/false.  Therefore `DEFINED ... AND NOT ...` never fires for disabled
# symbols.  Instead we check NOT ${sym} (empty/unset == falsy) and guard with
# c_MCU_KCONFIG_FILE (set by common.cmake before ameba_claw is added) to
# distinguish "Kconfig build with symbol=n" from "non-Kconfig build".
macro(_kconfig_disable _kconfig_sym _cmake_var)
    if(NOT ${_kconfig_sym} AND DEFINED c_MCU_KCONFIG_FILE)
        set(${_cmake_var} OFF CACHE BOOL "" FORCE)
    endif()
endmacro()
_kconfig_disable(CONFIG_CLAW_LUA_DRV_GPIO         LUA_MOD_ENABLE_GPIO)
_kconfig_disable(CONFIG_CLAW_LUA_DRV_I2C          LUA_MOD_ENABLE_I2C)
# Cascade: modules that depend on I2C also require it at link time (lua_i2c_bus_*)
if(NOT CONFIG_CLAW_LUA_DRV_I2C AND DEFINED c_MCU_KCONFIG_FILE)
    set(LUA_MOD_ENABLE_IMU                  OFF CACHE BOOL "" FORCE)
    set(LUA_MOD_ENABLE_ENVIRONMENTAL_SENSOR OFF CACHE BOOL "" FORCE)
    set(LUA_MOD_ENABLE_LIGHT_SENSOR         OFF CACHE BOOL "" FORCE)
    set(LUA_MOD_ENABLE_MAGNETOMETER         OFF CACHE BOOL "" FORCE)
endif()
_kconfig_disable(CONFIG_CLAW_LUA_DRV_SPI          LUA_MOD_ENABLE_SPI)
_kconfig_disable(CONFIG_CLAW_LUA_DRV_UART         LUA_MOD_ENABLE_UART)
_kconfig_disable(CONFIG_CLAW_LUA_DRV_PWM          LUA_MOD_ENABLE_PWM)
_kconfig_disable(CONFIG_CLAW_LUA_DRV_ADC          LUA_MOD_ENABLE_ADC)
_kconfig_disable(CONFIG_CLAW_LUA_DRV_RTC          LUA_MOD_ENABLE_RTC)
_kconfig_disable(CONFIG_CLAW_LUA_DRV_IR           LUA_MOD_ENABLE_IR)
_kconfig_disable(CONFIG_CLAW_LUA_DRV_THERMAL      LUA_MOD_ENABLE_THERMAL)
_kconfig_disable(CONFIG_CLAW_LUA_DRV_CAPTOUCH     LUA_MOD_ENABLE_CAPTOUCH)
_kconfig_disable(CONFIG_CLAW_LUA_DRV_BASICTIMER   LUA_MOD_ENABLE_BASICTIMER)
_kconfig_disable(CONFIG_CLAW_LUA_DRV_LED_STRIP    LUA_MOD_ENABLE_LED_STRIP)
_kconfig_disable(CONFIG_CLAW_LUA_DRV_IMU          LUA_MOD_ENABLE_IMU)
_kconfig_disable(CONFIG_CLAW_LUA_DRV_ENV_SENSOR   LUA_MOD_ENABLE_ENVIRONMENTAL_SENSOR)
_kconfig_disable(CONFIG_CLAW_LUA_DRV_LIGHT_SENSOR LUA_MOD_ENABLE_LIGHT_SENSOR)
_kconfig_disable(CONFIG_CLAW_LUA_DRV_MAGNETOMETER LUA_MOD_ENABLE_MAGNETOMETER)
_kconfig_disable(CONFIG_CLAW_LUA_DRV_LVGL         LUA_MOD_ENABLE_LVGL)
_kconfig_disable(CONFIG_CLAW_LUA_BLE              LUA_MOD_ENABLE_BLE)

# Project-level defaults: modules not yet wired for this board/platform stay OFF
# unless the user explicitly enables them via Kconfig.
# AUDIO: Kconfig default=n; also force OFF here so option() default ON is overridden.
if(NOT CONFIG_CLAW_LUA_DRV_AUDIO)
    set(LUA_MOD_ENABLE_AUDIO OFF CACHE BOOL "" FORCE)
endif()
# DISPLAY and LVGL: require LVGL library AND explicit Kconfig enable
if(NOT CONFIG_LVGL_ENABLE OR NOT CONFIG_CLAW_LUA_DRV_LVGL)
    set(LUA_MOD_ENABLE_DISPLAY OFF CACHE BOOL "" FORCE)
    set(LUA_MOD_ENABLE_LVGL    OFF CACHE BOOL "" FORCE)
endif()
# TOUCH: no Kconfig symbol yet; follow LVGL — touch_engine_* is consumed by
# lua_module_lvgl's lv_indev read_cb and must be linked whenever LVGL is on.
if(CONFIG_LVGL_ENABLE AND CONFIG_CLAW_LUA_DRV_LVGL)
    set(LUA_MOD_ENABLE_TOUCH ON  CACHE BOOL "" FORCE)
else()
    set(LUA_MOD_ENABLE_TOUCH OFF CACHE BOOL "" FORCE)
endif()
# LCDC: no Kconfig symbol yet; always off
set(LUA_MOD_ENABLE_LCDC  OFF CACHE BOOL "" FORCE)

set(_lua_module_specs
    "GPIO|modules/lua_driver_gpio/src/lua_driver_gpio.c|modules/lua_driver_gpio/src/lua_driver_gpio_button.c|modules/lua_driver_gpio/test/lua_gpio_test_provision.c"
    "I2C|modules/lua_driver_i2c/src/lua_driver_i2c.c|modules/lua_driver_i2c/test/lua_i2c_test_provision.c"
    "SPI|modules/lua_driver_spi/src/lua_driver_spi.c|modules/lua_driver_spi/test/lua_spi_test_provision.c"
    "UART|modules/lua_driver_uart/src/lua_driver_uart.c|modules/lua_driver_uart/test/lua_uart_test_provision.c"
    "PWM|modules/lua_driver_pwm/src/lua_driver_pwm.c|modules/lua_driver_pwm/test/lua_pwm_test_provision.c"
    "RTC|modules/lua_driver_rtc/src/lua_driver_rtc.c|modules/lua_driver_rtc/test/lua_rtc_test_provision.c"
    "IR|modules/lua_driver_ir/src/lua_driver_ir.c|modules/lua_driver_ir/test/lua_ir_test_provision.c"
    "LCDC|modules/lua_driver_lcdc/src/lua_driver_lcdc.c|modules/lua_driver_lcdc/test/lua_lcdc_test_provision.c"
    "AUDIO|modules/lua_driver_audio/src/lua_driver_audio.c|modules/lua_driver_audio/test/lua_speaker_test_provision.c|modules/lua_driver_audio/test/lua_dmic_test_provision.c"
    "ADC|modules/lua_driver_adc/src/lua_driver_adc.c|modules/lua_driver_adc/test/lua_adc_test_provision.c"
    "THERMAL|modules/lua_driver_thermal/src/lua_driver_thermal.c|modules/lua_driver_thermal/test/lua_thermal_test_provision.c"
    "CAPTOUCH|modules/lua_driver_captouch/src/lua_driver_captouch.c|modules/lua_driver_captouch/test/lua_captouch_test_provision.c"
    "TOUCH|modules/lua_driver_touch/src/lua_driver_touch.c"
    "BASICTIMER|modules/lua_module_basictimer/src/lua_driver_basictimer.c|modules/lua_module_basictimer/test/lua_basictimer_test_provision.c"
    "LED_STRIP|modules/lua_module_led_strip/src/lua_module_led_strip.c|modules/lua_module_led_strip/test/lua_led_strip_test_provision.c"
    "ENVIRONMENTAL_SENSOR|modules/lua_module_environmental_sensor/src/lua_module_environmental_sensor.c|modules/lua_module_environmental_sensor/test/lua_environmental_sensor_test_provision.c"
    "LIGHT_SENSOR|modules/lua_module_light_sensor/src/lua_module_light_sensor.c|modules/lua_module_light_sensor/test/lua_light_sensor_test_provision.c"
    "IMU|modules/lua_module_imu/src/lua_module_imu.c|modules/lua_module_imu/src/backend/imu_backend.c|modules/lua_module_imu/src/backend/mpu6050/mpu6050.c|modules/lua_module_imu/src/backend/bmi270/bmi270.c|modules/lua_module_imu/src/backend/bmi270/bmi270_config.c|modules/lua_module_imu/test/lua_imu_test_provision.c"
    "MAGNETOMETER|modules/lua_module_magnetometer/src/lua_module_magnetometer.c|modules/lua_module_magnetometer/src/backend/mag_backend.c|modules/lua_module_magnetometer/src/backend/bmm150/bmm150_backend.c|modules/lua_module_magnetometer/src/backend/bmm150/bmm150.c|modules/lua_module_magnetometer/test/lua_magnetometer_test_provision.c"
    "STORAGE|modules/lua_module_storage/src/lua_module_storage.c|modules/lua_module_storage/test/lua_storage_test_provision.c"
    "DISPLAY|modules/lua_module_display/src/display_lua.c|modules/lua_module_display/src/backend/display_backend_spi.c|modules/lua_module_display/src/backend/display_backend_lcdc.c|modules/lua_module_display/src/panel/spi/panel_spi_st7789.c|modules/lua_module_display/src/panel/jpeg/jpeg_hw.c|modules/lua_module_display/test/display_lcdc_test.c|modules/lua_module_display/test/display_lvgl_bench.c"
    "LVGL|modules/lua_module_lvgl/src/lvgl_lua.c|modules/lua_module_lvgl/src/lvgl_widgets.c"
    "CAP|modules/lua_module_cap/src/lua_module_cap.c"
    "EVENT|modules/lua_module_event/src/lua_module_event.c"
    "THREAD|modules/lua_module_thread/src/lua_module_thread.c|modules/lua_module_thread/src/thread_sync.c|modules/lua_module_thread/src/thread_job.c|modules/lua_module_thread/test/lua_thread_test_provision.c"
    "FILE|modules/lua_module_file/src/lua_module_file.c"
    "SYS|modules/lua_module_sys/src/lua_module_sys.c"
    "CJSON|modules/lua_module_cjson/src/lua_module_cjson.c"
    "TIMER|modules/lua_module_timer/src/lua_module_timer.c"
    "UDP|modules/lua_module_udp/src/lua_module_udp.c"
    "WIFI|modules/lua_module_wifi/src/lua_module_wifi.c"
    "BLE|modules/lua_module_ble/src/lua_module_ble.c|modules/lua_module_ble/src/lua_ble_bridge.c"
)

# lua_module_display needs LVGL (lvgl.h / lv_conf.h).  LVGL is now enabled and
# built for this project (CONFIG_LVGL_ENABLE=y in prj.conf → lib_lvgl.a), so the
# module can be compiled; its LVGL include dirs are wired below (gated on the
# switch).  Default ON like every other module.

foreach(_spec ${_lua_module_specs})
    string(REPLACE "|" ";" _spec_items "${_spec}")         # back to a CMake list
    list(GET _spec_items 0 _mod)
    list(REMOVE_AT _spec_items 0)                          # remaining = sources
    option(LUA_MOD_ENABLE_${_mod} "Enable Lua module ${_mod}" ON)
    # IMU backends drive the bus through lua_driver_i2c's shared C bus API, so
    # the I2C module must be compiled in whenever IMU is (else lua_i2c_bus_*
    # is an undefined reference at link time).
    # I2C-dependent modules cannot link without I2C; force them off if I2C is absent.
    # (Normally handled by the cascade block above; this is a runtime-config safety net.)
    if(("${_mod}" STREQUAL "IMU" OR "${_mod}" STREQUAL "MAGNETOMETER" OR
        "${_mod}" STREQUAL "ENVIRONMENTAL_SENSOR" OR "${_mod}" STREQUAL "LIGHT_SENSOR")
        AND NOT LUA_MOD_ENABLE_I2C)
        set(LUA_MOD_ENABLE_${_mod} OFF CACHE BOOL "" FORCE)
    endif()
    # STORAGE uses FatFS symbols (fatfs2_mount_flag, f_getfree); link fails without FatFS.
    if("${_mod}" STREQUAL "STORAGE" AND NOT CONFIG_VFS_FATFS_INCLUDED)
        set(LUA_MOD_ENABLE_${_mod} OFF CACHE BOOL "" FORCE)
    endif()
    # BLE calls rtk_bt_* which only exist when the BT stack is compiled (CONFIG_BT).
    # CONFIG_BT is a def_bool (WHC mode driven), so select BT_MENU alone cannot
    # guarantee it; force BLE off here if BT is absent to avoid undefined refs.
    if("${_mod}" STREQUAL "BLE" AND NOT CONFIG_BT)
        set(LUA_MOD_ENABLE_${_mod} OFF CACHE BOOL "" FORCE)
    endif()
    if(LUA_MOD_ENABLE_${_mod})
        foreach(_src ${_spec_items})
            # /test/ sources are the *_test_provision.c that embed & write the
            # test .lua to vfs:/; only compile them when driver tests are on.
            string(FIND "${_src}" "/test/" _is_test_src)
            if(_is_test_src EQUAL -1 OR CONFIG_CLAW_ENABLE_TESTS)
                ameba_list_append(private_sources "${_src}")
            endif()
        endforeach()
        ameba_list_append(private_definitions LUA_MOD_ENABLE_${_mod}=1)
    else()
        ameba_list_append(private_definitions LUA_MOD_ENABLE_${_mod}=0)
    endif()
endforeach()

# lua_module_display (display_lua.c) includes "lvgl.h"; LVGL also pulls in the
# per-SoC "lv_conf.h".  Wire the same two include roots the SDK's own LVGL
# targets use (see build.ninja): the core headers dir and the SoC config dir.
# lua_module_lvgl needs the exact same LVGL headers PLUS display_lua.c's
# backend vtable (display_backend.h) and ownership arbiter
# (display_ownership.h) — it is a hard dependency, not merely built alongside
# it (see design_spec/display/phase5_lvgl_full.md §2/§4), so this block is
# shared by both switches.
if(LUA_MOD_ENABLE_DISPLAY OR LUA_MOD_ENABLE_LVGL)
    ameba_list_append(private_includes
        # LVGL core headers + per-SoC lv_conf.h (same roots the SDK LVGL targets use)
        ${c_CMPT_UI_DIR}/LVGL/lvgl-9.3/lvgl
        ${c_CMPT_UI_DIR}/LVGL/lvgl-9.3/config/${c_SOC_TYPE}
        # display module's own internal headers (backend vtable + panel tables +
        # display_ownership.h, consumed by lua_module_lvgl too)
        modules/lua_module_display/src
        modules/lua_module_display/src/backend
        modules/lua_module_display/src/panel/spi
        modules/lua_module_display/src/panel/lcdc
        modules/lua_module_display/src/panel/jpeg
        # Hardware JPEG decoder + post-processor headers (JpegDec*/PP* API),
        # consumed by panel/jpeg/jpeg_hw.c for display.draw_image.
        ${c_CMPT_FWLIB_DIR}/jpeg_decoder/inc
    )
endif()
if(LUA_MOD_ENABLE_LVGL)
    ameba_list_append(private_includes
        modules/lua_module_lvgl/include
        modules/lua_module_lvgl/src
    )
endif()

# BLE module: rtk_bt_* public headers. When BT is compiled its api/CMakeLists.txt
# does ameba_global_include(api/include), so these are usually already visible;
# adding them explicitly is a safety net (mirrors the def_bool caveat above).
if(LUA_MOD_ENABLE_BLE)
    ameba_list_append(private_includes
        ${c_CMPT_BLUETOOTH_DIR}/api/include
    )
endif()

# USB UVC / MSC modules: dual-gated by the ameba_claw Kconfig symbol AND the
# SDK-level USB host Kconfig.  The CLAW_LUA_DRV_USB_* guard prevents including
# USB host stack headers when they are not wired into the lua library's include
# paths, which would cause link failures even when CONFIG_USBH_* is set.
# usbh_uvc_intf.h was deleted in SDK commit 9f134a5bcb (2026-07-17, UVC API rework).
# Disable UVC Lua module until lua_driver_uvc is ported to the new usbh_uvc_start/stop API.
set(CONFIG_USBH_UVC OFF)
if(CONFIG_CLAW_LUA_DRV_USB_UVC AND CONFIG_USBH_UVC)
    ameba_list_append(private_sources modules/lua_driver_uvc/src/luusb_uvc.c)
    if(CONFIG_CLAW_ENABLE_TESTS)
        ameba_list_append(private_sources modules/lua_driver_uvc/test/lua_uvc_test_provision.c)
    endif()
    ameba_list_append(private_definitions LUA_MOD_ENABLE_USB_UVC=1)
else()
    ameba_list_append(private_definitions LUA_MOD_ENABLE_USB_UVC=0)
endif()
if(CONFIG_CLAW_LUA_DRV_USB_MSC AND CONFIG_USBH_MSC)
    ameba_list_append(private_sources
        modules/lua_driver_msc/src/luusb_msc.c
        modules/lua_driver_msc/src/lua_msc_ops.c
    )
    if(CONFIG_CLAW_ENABLE_TESTS)
        ameba_list_append(private_sources modules/lua_driver_msc/test/lua_msc_test_provision.c)
    endif()
    ameba_list_append(private_definitions LUA_MOD_ENABLE_USB_MSC=1)
else()
    ameba_list_append(private_definitions LUA_MOD_ENABLE_USB_MSC=0)
endif()


# NOTE: LUA_USE_C89 is intentionally NOT defined here. It is an ABI-critical
# numeric option (it makes lua_Integer 32-bit) that EVERY consumer of lua.h must
# agree on, so it is pinned unconditionally in lua/interpreter/luaconf.h instead.
# A private build -D did not propagate to out-of-tree consumers (cap_lua, etc.),
# which silently corrupted integers across the C-API boundary. Do not re-add it.

ameba_list_append(private_compile_options
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-sign-compare
    -Wno-implicit-fallthrough
)

ameba_add_internal_library(lua
    p_SOURCES
        ${private_sources}
    p_INCLUDES
        ${private_includes}
    p_DEFINITIONS
        ${private_definitions}
    p_COMPILE_OPTIONS
        ${private_compile_options}
)

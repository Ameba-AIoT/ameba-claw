set(private_sources)
set(private_includes)
set(private_definitions)
set(private_compile_options)

# ---- Test provision: embedded Lua scripts (only when CONFIG_LUA_DRIVER_TESTS) ----
# Each execute_process embeds a test .lua as a C string header at configure time.
# Re-runs automatically when the source .lua changes (CMAKE_CONFIGURE_DEPENDS).
# LUA_DRIVER_TESTS_ENABLED=1 always: test scripts are pre-generated in the
# test/ source directory; handle_cmd_hw_test needs this flag to be active.
ameba_list_append(private_definitions LUA_DRIVER_TESTS_ENABLED=1)

if(CONFIG_LUA_DRIVER_TESTS)
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
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_driver_uart/test/uart_at.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/uart_at_lua.h" "s_uart_at_script")
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
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_module_environmental_sensor/test/test_environmental_sensor.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/environmental_sensor_test_lua.h" "s_environmental_sensor_test_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_module_light_sensor/test/test_light_sensor.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/light_sensor_test_lua.h" "s_light_sensor_test_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_module_imu/test/test_imu.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/imu_test_lua.h" "s_imu_test_script")

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

# Always add binary dir so pre-generated test script headers are found
ameba_list_append(private_includes ${CMAKE_CURRENT_BINARY_DIR})

ameba_list_append(private_includes
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/interpreter
    modules
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_modules/claw_event_dispatcher/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_modules/claw_cap/include
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
    "LED_STRIP|modules/lua_module_led_strip/src/lua_module_led_strip.c"
    "ENVIRONMENTAL_SENSOR|modules/lua_module_environmental_sensor/src/lua_module_environmental_sensor.c|modules/lua_module_environmental_sensor/test/lua_environmental_sensor_test_provision.c"
    "LIGHT_SENSOR|modules/lua_module_light_sensor/src/lua_module_light_sensor.c|modules/lua_module_light_sensor/test/lua_light_sensor_test_provision.c"
    "IMU|modules/lua_module_imu/src/lua_module_imu.c|modules/lua_module_imu/src/backend/imu_backend.c|modules/lua_module_imu/src/backend/mpu6050/mpu6050.c|modules/lua_module_imu/test/lua_imu_test_provision.c"
    "DISPLAY|modules/lua_module_display/src/display_lua.c|modules/lua_module_display/src/backend/display_backend_spi.c|modules/lua_module_display/src/backend/display_backend_lcdc.c|modules/lua_module_display/src/panel/spi/panel_spi_st7789.c|modules/lua_module_display/src/panel/jpeg/jpeg_hw.c|modules/lua_module_display/test/display_lcdc_test.c|modules/lua_module_display/test/display_lvgl_bench.c"
    "LVGL|modules/lua_module_lvgl/src/lvgl_lua.c|modules/lua_module_lvgl/src/lvgl_widgets.c"
    "CAP|modules/lua_module_cap/src/lua_module_cap.c"
    "EVENT|modules/lua_module_event/src/lua_module_event.c"
    "FILE|modules/lua_module_file/src/lua_module_file.c"
    "SYS|modules/lua_module_sys/src/lua_module_sys.c"
    "CJSON|modules/lua_module_cjson/src/lua_module_cjson.c"
    "TIMER|modules/lua_module_timer/src/lua_module_timer.c"
    "UDP|modules/lua_module_udp/src/lua_module_udp.c"
    "WIFI|modules/lua_module_wifi/src/lua_module_wifi.c"
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
    # DISPLAY and LVGL modules require the LVGL library; auto-disable both when
    # CONFIG_LVGL_ENABLE is off so the linker does not fail on missing lv_* symbols.
    if("${_mod}" STREQUAL "DISPLAY" AND NOT CONFIG_LVGL_ENABLE)
        set(LUA_MOD_ENABLE_DISPLAY OFF CACHE BOOL "" FORCE)
    endif()
    if("${_mod}" STREQUAL "LVGL" AND NOT CONFIG_LVGL_ENABLE)
        set(LUA_MOD_ENABLE_LVGL OFF CACHE BOOL "" FORCE)
    endif()
    # IMU backends drive the bus through lua_driver_i2c's shared C bus API, so
    # the I2C module must be compiled in whenever IMU is (else lua_i2c_bus_*
    # is an undefined reference at link time).
    if("${_mod}" STREQUAL "IMU" AND NOT LUA_MOD_ENABLE_I2C)
        message(FATAL_ERROR "LUA_MOD_ENABLE_IMU requires LUA_MOD_ENABLE_I2C")
    endif()
    if(LUA_MOD_ENABLE_${_mod})
        foreach(_src ${_spec_items})
            string(FIND "${_src}" "/test/" _is_test_src)
            if(_is_test_src EQUAL -1)
                ameba_list_append(private_sources "${_src}")
            else()
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

# USB UVC / MSC modules follow the same per-module switch as everything above,
# but are tied to the project's USB Host Kconfig: a Lua USB class binding only
# makes sense when the matching host class is actually built into the image.
# So LUA_MOD_ENABLE_USB_UVC tracks CONFIG_USBH_UVC and _USB_MSC tracks
# CONFIG_USBH_MSC (each requires the USB host stack + FatFS to be linked).
if(CONFIG_USBH_UVC)
    ameba_list_append(private_sources modules/lua_driver_uvc/src/luusb_uvc.c)
    ameba_list_append(private_sources modules/lua_driver_uvc/test/lua_uvc_test_provision.c)
    ameba_list_append(private_definitions LUA_MOD_ENABLE_USB_UVC=1)
else()
    ameba_list_append(private_definitions LUA_MOD_ENABLE_USB_UVC=0)
endif()
if(CONFIG_USBH_MSC)
    ameba_list_append(private_sources
        modules/lua_driver_msc/src/luusb_msc.c
        modules/lua_driver_msc/src/lua_msc_ops.c
    )
    ameba_list_append(private_sources modules/lua_driver_msc/test/lua_msc_test_provision.c)
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

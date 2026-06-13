set(private_sources)
set(private_includes)
set(private_definitions)
set(private_compile_options)

# ---- Test provision: embedded Lua scripts (only when CONFIG_LUA_DRIVER_TESTS) ----
# Each execute_process embeds a test .lua as a C string header at configure time.
# Re-runs automatically when the source .lua changes (CMAKE_CONFIGURE_DEPENDS).
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
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_driver_adc/test/test_adc.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/adc_test_lua.h" "s_adc_test_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_driver_thermal/test/test_thermal.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/thermal_test_lua.h" "s_thermal_test_script")
    _embed_lua(
        "${CMAKE_CURRENT_SOURCE_DIR}/modules/lua_driver_touch/test/test_touch.lua"
        "${CMAKE_CURRENT_BINARY_DIR}/touch_test_lua.h" "s_touch_test_script")

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

    ameba_list_append(private_includes ${CMAKE_CURRENT_BINARY_DIR})
    ameba_list_append(private_definitions LUA_DRIVER_TESTS_ENABLED=1)
else()
    ameba_list_append(private_definitions LUA_DRIVER_TESTS_ENABLED=0)
endif()

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
    modules/lua_driver_touch/src
    modules/lua_driver_uvc/src
    modules/lua_driver_msc/src
    modules/lua_driver_lcdc/src
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
    lua_repl.c
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
    "GPIO|modules/lua_driver_gpio/src/lua_driver_gpio.c|modules/lua_driver_gpio/test/lua_gpio_test_provision.c"
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
    "TOUCH|modules/lua_driver_touch/src/lua_driver_touch.c|modules/lua_driver_touch/test/lua_touch_test_provision.c"
    "CAP|modules/lua_module_cap/src/lua_module_cap.c"
    "EVENT|modules/lua_module_event/src/lua_module_event.c"
    "FILE|modules/lua_module_file/src/lua_module_file.c"
    "SYS|modules/lua_module_sys/src/lua_module_sys.c"
    "CJSON|modules/lua_module_cjson/src/lua_module_cjson.c"
    "TIMER|modules/lua_module_timer/src/lua_module_timer.c"
    "UDP|modules/lua_module_udp/src/lua_module_udp.c"
    "WIFI|modules/lua_module_wifi/src/lua_module_wifi.c"
)

foreach(_spec ${_lua_module_specs})
    string(REPLACE "|" ";" _spec_items "${_spec}")         # back to a CMake list
    list(GET _spec_items 0 _mod)
    list(REMOVE_AT _spec_items 0)                          # remaining = sources
    option(LUA_MOD_ENABLE_${_mod} "Enable Lua module ${_mod}" ON)
    if(LUA_MOD_ENABLE_${_mod})
        foreach(_src ${_spec_items})
            string(FIND "${_src}" "/test/" _is_test_src)
            if(_is_test_src EQUAL -1)
                ameba_list_append(private_sources "${_src}")
            elseif(CONFIG_LUA_DRIVER_TESTS)
                ameba_list_append(private_sources "${_src}")
            endif()
        endforeach()
        ameba_list_append(private_definitions LUA_MOD_ENABLE_${_mod}=1)
    else()
        ameba_list_append(private_definitions LUA_MOD_ENABLE_${_mod}=0)
    endif()
endforeach()

# USB UVC / MSC modules follow the same per-module switch as everything above,
# but are tied to the project's USB Host Kconfig: a Lua USB class binding only
# makes sense when the matching host class is actually built into the image.
# So LUA_MOD_ENABLE_USB_UVC tracks CONFIG_USBH_UVC and _USB_MSC tracks
# CONFIG_USBH_MSC (each requires the USB host stack + FatFS to be linked).
if(CONFIG_USBH_UVC)
    ameba_list_append(private_sources modules/lua_driver_uvc/src/luusb_uvc.c)
    if(CONFIG_LUA_DRIVER_TESTS)
        ameba_list_append(private_sources modules/lua_driver_uvc/test/lua_uvc_test_provision.c)
    endif()
    ameba_list_append(private_definitions LUA_MOD_ENABLE_USB_UVC=1)
else()
    ameba_list_append(private_definitions LUA_MOD_ENABLE_USB_UVC=0)
endif()
if(CONFIG_USBH_MSC)
    ameba_list_append(private_sources
        modules/lua_driver_msc/src/luusb_msc.c
        modules/lua_driver_msc/src/lua_msc_ops.c
    )
    if(CONFIG_LUA_DRIVER_TESTS)
        ameba_list_append(private_sources modules/lua_driver_msc/test/lua_msc_test_provision.c)
    endif()
    ameba_list_append(private_definitions LUA_MOD_ENABLE_USB_MSC=1)
else()
    ameba_list_append(private_definitions LUA_MOD_ENABLE_USB_MSC=0)
endif()


ameba_list_append(private_definitions
    LUA_USE_C89
)

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

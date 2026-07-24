set(private_sources)
set(private_includes)
set(private_definitions)

# ---- claw_atcmd: serial AT+CLAW command interface ----
# cap_atcmd.c owns only the AT dispatch entry point and serial channel init.
# Each subcommand group lives in its own file under src/; hardware driver
# tests and unit tests are in src/test/.
ameba_list_append(private_sources
    src/cap_atcmd.c
    src/atcmd_ask.c
    src/atcmd_lua.c
    src/atcmd_cfg.c
    src/atcmd_im.c
    src/atcmd_session.c
    src/atcmd_memory.c
    src/atcmd_cap.c
    src/atcmd_fs.c
    src/atcmd_sys.c
    src/test/atcmd_hw_test.c
)

if(CONFIG_CLAW_BUILD_TESTS)
    # cap_atcmd.c and atcmd_unit_test.c guard the AT+CLAW=test dispatch with
    # #ifdef CONFIG_CLAW_BUILD_TESTS, which platform_autoconf.h defines globally
    # when the symbol is enabled — no per-lib compile-def needed.
    ameba_list_append(private_sources src/test/atcmd_unit_test.c)
    # atcmd_unit_test.c includes claw_test.h, which lives with the test target
    # sources under ameba_claw/test/ (not on this lib's default include path).
    ameba_list_append(private_includes ${CMAKE_CURRENT_SOURCE_DIR}/../../test)
endif()

# CONFIG_CLAW_LUA_DRIVER_TESTS is defined in ameba_claw/Kconfig (default y), but
# the SDK menuconfig pipeline never loads ameba_claw/Kconfig (RTK_APP_DIR is not
# set for external projects), so this variable is always undefined in CMake and
# if(CONFIG_CLAW_LUA_DRIVER_TESTS) would silently evaluate to false.
# Pass -DCONFIG_CLAW_LUA_DRIVER_TESTS=ON to the build to enable driver tests.
if(CONFIG_CLAW_LUA_DRIVER_TESTS)
    ameba_list_append(private_definitions LUA_DRIVER_TESTS_ENABLED=1)
else()
    ameba_list_append(private_definitions LUA_DRIVER_TESTS_ENABLED=0)
endif()

# Propagate lua module enable flags so atcmd_hw_test.c sees the same
# LUA_MOD_ENABLE_* values as the lua library.  Must pass both =1 and =0
# explicitly — lua_modules_config.h defaults everything to 0 via #ifndef,
# so omitting the =1 case leaves the guard disabled even when the module is on.
foreach(_mod DISPLAY ENVIRONMENTAL_SENSOR IMU AUDIO LCDC MAGNETOMETER LIGHT_SENSOR STORAGE USB_UVC USB_MSC UART ADC IR THERMAL CAPTOUCH BASICTIMER)
    if(LUA_MOD_ENABLE_${_mod})
        ameba_list_append(private_definitions LUA_MOD_ENABLE_${_mod}=1)
    else()
        ameba_list_append(private_definitions LUA_MOD_ENABLE_${_mod}=0)
    endif()
endforeach()

# Agent auto-test harness (peripheral stimulus, e.g. AT+CLAW=gpio_ctrl).
# Disabled by default → zero image-size cost in release builds. Enable via
# CMake -DCLAW_AGENT_AUTO_TEST or env CLAW_AGENT_AUTO_TEST=1; this compiles the
# source under test/agent_auto_test/ AND defines CLAW_AGENT_AUTO_TEST=1 for the
# whole claw_atcmd lib (so cap_atcmd.c's dispatch #if also lights up).
if(CONFIG_CLAW_AGENT_AUTO_TEST OR DEFINED CLAW_AGENT_AUTO_TEST OR "$ENV{CLAW_AGENT_AUTO_TEST}" STREQUAL "1")
    ameba_list_append(private_sources
        ${CMAKE_CURRENT_SOURCE_DIR}/../../test/agent_auto_test/atcmd_gpio_ctrl.c)
    ameba_list_append(private_definitions CLAW_AGENT_AUTO_TEST=1)
    message(STATUS "[claw_atcmd] agent_auto_test enabled (gpio_ctrl compiled in)")
endif()

ameba_list_append(private_includes
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lua/modules
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lua
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lua/interpreter
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_common/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_wifi_mgr/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_agent/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_agent/llm_agent
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_cap/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_memory/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_im_dispatch/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_event_dispatcher/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_session_mgr/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_capabilities/cap_im/cap_im_wechat/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_capabilities/cap_im/cap_im_wechat/wechat_bot
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_capabilities/cap_webui/include
)
ameba_list_append(private_sources src/lua_repl.c)
ameba_list_append(private_includes
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_capabilities/cap_lua/include)

ameba_add_internal_library(claw_atcmd
    p_SOURCES ${private_sources}
    p_INCLUDES ${private_includes}
    p_DEFINITIONS ${private_definitions}
    p_COMPILE_OPTIONS -Wno-unused-parameter
)

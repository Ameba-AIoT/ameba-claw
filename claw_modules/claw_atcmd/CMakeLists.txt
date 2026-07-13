set(private_sources)
set(private_includes)
set(private_definitions)

# ---- claw_atcmd: serial AT+CLAW command interface ----
# cap_atcmd.c owns only the AT dispatch entry point and serial channel init.
# Each subcommand group lives in its own file under src/; hardware driver
# tests and unit tests are in src/test/.
ameba_list_append(private_sources
    src/cap_atcmd.c
    src/lua_repl.c
    src/atcmd_ask.c
    src/atcmd_cfg.c
    src/atcmd_session.c
    src/atcmd_memory.c
    src/atcmd_cap.c
    src/atcmd_fs.c
    src/atcmd_sys.c
    src/test/atcmd_hw_test.c
)

if(DEFINED CLAW_BUILD_TESTS)
    ameba_list_append(private_sources src/test/atcmd_unit_test.c)
endif()

if(CONFIG_LUA_DRIVER_TESTS)
    ameba_list_append(private_definitions LUA_DRIVER_TESTS_ENABLED=1)
else()
    ameba_list_append(private_definitions LUA_DRIVER_TESTS_ENABLED=0)
endif()

# Propagate lua module enable flags so atcmd_hw_test.c sees the same
# LUA_MOD_ENABLE_* values as the lua library (lua_modules_config.h defaults
# to ON; cmake may have them OFF).
if(NOT LUA_MOD_ENABLE_DISPLAY)
    ameba_list_append(private_definitions LUA_MOD_ENABLE_DISPLAY=0)
endif()
if(NOT LUA_MOD_ENABLE_ENVIRONMENTAL_SENSOR)
    ameba_list_append(private_definitions LUA_MOD_ENABLE_ENVIRONMENTAL_SENSOR=0)
endif()
if(NOT LUA_MOD_ENABLE_IMU)
    ameba_list_append(private_definitions LUA_MOD_ENABLE_IMU=0)
endif()

# Agent auto-test harness (peripheral stimulus, e.g. AT+CLAW=gpio_ctrl).
# Disabled by default → zero image-size cost in release builds. Enable via
# CMake -DCLAW_AGENT_AUTO_TEST or env CLAW_AGENT_AUTO_TEST=1; this compiles the
# source under test/agent_auto_test/ AND defines CLAW_AGENT_AUTO_TEST=1 for the
# whole claw_atcmd lib (so cap_atcmd.c's dispatch #if also lights up).
if(DEFINED CLAW_AGENT_AUTO_TEST OR "$ENV{CLAW_AGENT_AUTO_TEST}" STREQUAL "1")
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

ameba_add_internal_library(claw_atcmd
    p_SOURCES ${private_sources}
    p_INCLUDES ${private_includes}
    p_DEFINITIONS ${private_definitions}
    p_COMPILE_OPTIONS -Wno-unused-parameter
)

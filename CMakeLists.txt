# ameba_claw top-level CMakeLists.txt
set(BOARD "EV721FL0_R03" CACHE STRING "Default board for cap_board_mgr")
# Layout: claw_modules (core layer) / claw_capabilities (capability layer) / lua

# Override the SDK's default -Os with -O3 for ameba_claw's own sources. The SDK
# injects -Os at an ancestor directory scope; CMake orders directory options
# ancestor-first, so this -O3 lands last on the command line and GCC honours it.
# Must run before the ameba_add_subdirectory() calls so subdirectories inherit it.
add_compile_options(
    "$<$<COMPILE_LANGUAGE:C>:-O3>"
    "$<$<COMPILE_LANGUAGE:CXX>:-O3>"
)

# Assemble the rolfs image staging dir from all module content sources, grouped
# by reader (not language):  <any>/docs/*.md -> staging/docs/ (LLM-facing docs),
# <any>/lib/*.lua -> staging/lib/,  <any>/skills/*/ -> staging/skills/.
# Staging runs as a build-time custom_command (not configure-time file(COPY)) so
# ninja re-stages whenever any .md / .lua / SKILL.md content changes.
if(CONFIG_LITTLEFS_WITHIN_APP_IMG)
    set(rolfs_staging ${CMAKE_BINARY_DIR}/rolfs_staging)
    set(rolfs_stamp   ${CMAKE_BINARY_DIR}/rolfs_staging.stamp)

    # Collect rolfs source files for ninja dependency tracking. Registering each
    # scanned dir on CMAKE_CONFIGURE_DEPENDS makes CMake re-run configure (and
    # re-glob) when files are added or removed.
    set(rolfs_sources)

    set(lua_modules_dir ${CMAKE_CURRENT_SOURCE_DIR}/lua/modules)
    set(caps_dir        ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities)

    # Lua modules: auto-scan all subdirectories — no hand-maintained list.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${lua_modules_dir})
    file(GLOB _lua_mod_dirs LIST_DIRECTORIES true ${lua_modules_dir}/*)
    foreach(mod_dir ${_lua_mod_dirs})
        if(IS_DIRECTORY ${mod_dir})
            foreach(sub docs lib skills)
                if(EXISTS ${mod_dir}/${sub})
                    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${mod_dir}/${sub})
                    file(GLOB_RECURSE _files LIST_DIRECTORIES false ${mod_dir}/${sub}/*)
                    list(APPEND rolfs_sources ${_files})
                endif()
            endforeach()
        endif()
    endforeach()

    # Caps: Kconfig-gated — only enabled caps' docs enter the firmware, so the
    # LLM never sees docs for tools compiled out.
    set(_rolfs_caps cap_lua cap_files cap_board_mgr cap_skill_mgr)
    if(CONFIG_CLAW_CAP_AUDIO_STREAM)
        list(APPEND _rolfs_caps cap_audio_stream)
    endif()
    if(CONFIG_CLAW_CAP_NET_DISCOVER)
        list(APPEND _rolfs_caps cap_net_discover)
    endif()
    if(CONFIG_CLAW_CAP_ROUTER_MGR)
        list(APPEND _rolfs_caps cap_router_mgr)
    endif()
    foreach(cap ${_rolfs_caps})
        set(cap_dir ${caps_dir}/${cap})
        foreach(sub docs lib skills)
            if(EXISTS ${cap_dir}/${sub})
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${cap_dir}/${sub})
                file(GLOB_RECURSE _files LIST_DIRECTORIES false ${cap_dir}/${sub}/*)
                list(APPEND rolfs_sources ${_files})
            endif()
        endforeach()
    endforeach()

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/SYSTEM.md)
        list(APPEND rolfs_sources ${CMAKE_CURRENT_SOURCE_DIR}/SYSTEM.md)
    endif()

    # Materialise the build-time staging script from cmake/rolfs_stage.cmake.in.
    # configure_file(@ONLY) fills @VAR@ with these configure-time values and leaves
    # ${VAR} verbatim for the -P run — no backslash escaping in the template. It
    # also puts the template on CMAKE_CONFIGURE_DEPENDS.
    set(rolfs_stage_script ${CMAKE_BINARY_DIR}/rolfs_stage.cmake)
    set(src_dir ${CMAKE_CURRENT_SOURCE_DIR})
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/rolfs_stage.cmake.in
        ${rolfs_stage_script}
        @ONLY
    )

    add_custom_command(
        OUTPUT  ${rolfs_stamp}
        COMMAND ${CMAKE_COMMAND} -P ${rolfs_stage_script}
        # Invalidate the firmware_package marker so the next build re-runs postbuild
        # and re-appends the rolfs blob — otherwise ninja treats it as up-to-date and
        # new rolfs content never lands in app.bin.
        COMMAND ${CMAKE_COMMAND} -E remove -f
            ${CMAKE_BINARY_DIR}/CMakeFiles/firmware_package
        DEPENDS ${rolfs_sources} ${rolfs_stage_script}
        COMMENT "Staging rolfs content"
        VERBATIM
    )
    add_custom_target(rolfs_staging_target DEPENDS ${rolfs_stamp})

    # Hand the content dir + staging target to the framework; ameba_firmware_package
    # (common.cmake) wires firmware_package's dependency on it at SOC-project scope.
    ameba_add_rolfs_content(${rolfs_staging} rolfs_staging_target)
endif()

# ── Core layer ───────────────────────────────────────────────────────────────
ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_common)
ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_cap)
ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_cap_registry)
ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_memory)
ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_config)
ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_wifi_mgr)
ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_im_dispatch)
ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_agent)
ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_event_dispatcher)

# ── Lua runtime ──────────────────────────────────────────────────────────────
ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/lua)

# ── Capability layer: foundational services ──────────────────────────────────
ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_http_server)
ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_session_mgr)

# ── Capability layer ─────────────────────────────────────────────────────────
# Each optional cap is gated by its CONFIG_CLAW_CAP_* symbol (all default y). A
# disabled cap's subdirectory is not added, so it drops out of the build and the
# runtime registry. cap_lua is CORE (always compiled).
# See design_spec/arch/kconfig_cap_trimming_plan.md.

# general-purpose tools
if(CONFIG_CLAW_CAP_TIME)
    ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_time)
endif()
if(CONFIG_CLAW_CAP_FILES)
    ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_files)
endif()
if(CONFIG_CLAW_CAP_SYSTEM)
    ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_system)
endif()
if(CONFIG_CLAW_CAP_BOARD_MGR)
    ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_board_mgr)
endif()
if(CONFIG_CLAW_CAP_SCHEDULER)
    ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_scheduler)
endif()
if(CONFIG_CLAW_CAP_WEB_SEARCH)
    ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_web_search)
endif()
if(CONFIG_CLAW_CAP_HTTP_REQUEST)
    ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_http_request)
endif()
if(CONFIG_CLAW_CAP_VISION)
    ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_vision)
endif()
if(CONFIG_CLAW_CAP_NET_DISCOVER)
    ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_net_discover)
endif()
if(CONFIG_CLAW_CAP_AUDIO_STREAM)
    ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_audio_stream)
endif()

# skill management / routing / lua execution (cap_lua is CORE — unconditional)
ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_lua)
if(CONFIG_CLAW_CAP_SKILL_MGR)
    ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_skill_mgr)
endif()
if(CONFIG_CLAW_CAP_ROUTER_MGR)
    ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_router_mgr)
endif()

# observability
if(CONFIG_CLAW_CAP_HONESTY)
    ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_honesty)
endif()

# MCP
if(CONFIG_CLAW_CAP_MCP_CLIENT)
    ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_mcp_client)
endif()
if(CONFIG_CLAW_CAP_MCP_SERVER)
    ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_mcp_server)
endif()

# IM channels + WebUI (cap_webui is CORE — unconditional)
ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_webui)
if(CONFIG_CLAW_CAP_IM_ATTACHMENT)
    ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_im/cap_im_attachment)
endif()
if(CONFIG_CLAW_CAP_IM_TELEGRAM)
    ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_im/cap_im_telegram)
endif()
if(CONFIG_CLAW_CAP_IM_FEISHU)
    ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_im/cap_im_feishu)
endif()
if(CONFIG_CLAW_CAP_IM_QQ)
    ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_im/cap_im_qq)
endif()
if(CONFIG_CLAW_CAP_IM_WECHAT)
    ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_im/cap_im_wechat)
endif()
if(CONFIG_CLAW_CAP_IM_LOCAL)
    ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_im/cap_im_local)
endif()

# ── Serial AT interface (depends on both core + capability headers) ───────────
ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_atcmd)

# ── Main target ──────────────────────────────────────────────────────────────
ameba_internal_library(ameba_claw)

target_sources(${CURRENT_LIB_NAME} PRIVATE
    ameba_claw_main.c
    app_example.c
)

# Propagate LUA_DRIVER_TESTS_ENABLED to this target's sources (cap_atcmd.c): the
# lua library sets it privately, so it doesn't reach this separate target.
if(CONFIG_CLAW_LUA_DRIVER_TESTS)
    target_compile_definitions(${CURRENT_LIB_NAME} PRIVATE LUA_DRIVER_TESTS_ENABLED=1)
endif()

# Test infrastructure — only when CONFIG_CLAW_BUILD_TESTS is set. The CONFIG_ macro
# reaches C via platform_autoconf.h, so guards use #ifdef directly (no compile-def).
if(CONFIG_CLAW_BUILD_TESTS)
    target_sources(${CURRENT_LIB_NAME} PRIVATE test/claw_test.c)
    target_include_directories(${CURRENT_LIB_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
    message(STATUS "[ameba_claw] Test build enabled (test/claw_test.c included)")
endif()

# Propagate CLAW_AGENT_AUTO_TEST to this target (app_example.c uses it for startup
# GPIO pull-up init); it's set in claw_atcmd too, but that separate target's defs
# don't reach here.
if(CONFIG_CLAW_AGENT_AUTO_TEST OR DEFINED CLAW_AGENT_AUTO_TEST OR "$ENV{CLAW_AGENT_AUTO_TEST}" STREQUAL "1")
    target_compile_definitions(${CURRENT_LIB_NAME} PRIVATE CLAW_AGENT_AUTO_TEST=1)
endif()

target_include_directories(${CURRENT_LIB_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/lua/modules   # lua_modules_config.h (USB AT cmd gating)
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_common/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_wifi_mgr/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_webui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_agent/llm_agent
    # Core layer headers
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_cap/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_cap_registry/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_agent/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_agent/src
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_event_dispatcher/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_memory/include
    # Capability layer headers
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_session_mgr/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_im/cap_im_wechat/wechat_bot
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_time/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_files/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_system/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_board_mgr/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_scheduler/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_web_search/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_http_request/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_vision/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_net_discover/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_audio_stream/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_skill_mgr/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_router_mgr/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_honesty/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_mcp_client/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_mcp_server/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_im/cap_im_attachment/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_im/cap_im_telegram/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_http_server/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_im/cap_im_local/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_im/cap_im_feishu/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_im/cap_im_qq/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_im/cap_im_wechat/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_modules/claw_atcmd/include
    ${CMAKE_CURRENT_SOURCE_DIR}/claw_capabilities/cap_lua/include
)

# CONFIG_DFS is now defined directly in autoconf_8721f.h; do NOT re-inject it via
# cmake (redefinition error under -Werror). It was previously added here via DEFER.

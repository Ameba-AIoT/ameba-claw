set(private_sources)
set(private_includes)

set(WEBUI_RES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/res)
set(WEBUI_SRC_DIR ${WEBUI_RES_DIR})
set(WEBUI_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${WEBUI_GEN_DIR})

# Collect all WebUI source files so CMake/Ninja know when to rebuild.
file(GLOB WEBUI_SOURCES
    "${WEBUI_SRC_DIR}/*.html"
    "${WEBUI_SRC_DIR}/*.css"
    "${WEBUI_SRC_DIR}/*.js"
)

set(WEBUI_GEN_C "${WEBUI_GEN_DIR}/dashboard_html.c")

# build_html.py merges + minifies src/* -> a C byte-array source file.
# Because the *content* of the generated .c changes when any source file
# changes, ccache hashes the new .c and produces a fresh object — no stale
# cache hits possible (unlike the old .incbin/.S approach).
add_custom_command(
    OUTPUT  "${WEBUI_GEN_C}"
    COMMAND ${CMAKE_COMMAND} -E env
            python3 "${WEBUI_RES_DIR}/build_html.py"
                    "${WEBUI_SRC_DIR}"
                    "${WEBUI_GEN_C}"
    DEPENDS ${WEBUI_SOURCES} "${WEBUI_RES_DIR}/build_html.py"
    COMMENT "Bundling WebUI (merge + minify)"
    VERBATIM
)

ameba_list_append(private_sources "${WEBUI_GEN_C}")

# Embed binary resources (favicon.ico) via .incbin — unchanged approach.
set(_ico_asm "${WEBUI_GEN_DIR}/favicon_ico.S")
set(_ico_file "${WEBUI_RES_DIR}/favicon.ico")
file(WRITE "${_ico_asm}"
    "    .section .rodata\n"
    "    .balign 4\n"
    "    .global _res_favicon_ico_start\n"
    "_res_favicon_ico_start:\n"
    "    .incbin \"${_ico_file}\"\n"
    "    .global _res_favicon_ico_end\n"
    "_res_favicon_ico_end:\n"
)
set_source_files_properties("${_ico_asm}" PROPERTIES
    OBJECT_DEPENDS "${_ico_file}")
ameba_list_append(private_sources "${_ico_asm}")

ameba_list_append(private_sources src/cap_webui.c)
ameba_list_append(private_includes
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_cap/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_wifi_mgr/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_http_server/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../cap_im/cap_im_wechat/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../cap_im/cap_im_local/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_session_mgr/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_memory/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_event_dispatcher/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_agent/include
)

ameba_add_internal_library(cap_webui
    p_SOURCES ${private_sources}
    p_INCLUDES ${private_includes}
    p_COMPILE_OPTIONS -Wno-unused-parameter
)

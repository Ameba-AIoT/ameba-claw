set(private_sources)
set(private_includes)

# Embed HTML files into firmware via .incbin — no Python step required.
# Generated .S files use absolute paths so the assembler can find them.
set(WEBUI_RES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/res)
set(WEBUI_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${WEBUI_GEN_DIR})

foreach(_name IN ITEMS "dashboard")
    set(_asm_file "${WEBUI_GEN_DIR}/${_name}_html.S")
    set(_html_file "${WEBUI_RES_DIR}/${_name}.html")
    file(WRITE "${_asm_file}"
        "    .section .rodata\n"
        "    .balign 4\n"
        "    .global _res_${_name}_html_start\n"
        "_res_${_name}_html_start:\n"
        "    .incbin \"${_html_file}\"\n"
        "    .global _res_${_name}_html_end\n"
        "_res_${_name}_html_end:\n"
    )
    # Tell Ninja/Make that the object file depends on the HTML source,
    # so a build is triggered whenever the HTML is edited.
    set_source_files_properties("${_asm_file}" PROPERTIES
        OBJECT_DEPENDS "${_html_file}")
    ameba_list_append(private_sources "${_asm_file}")
endforeach()

ameba_list_append(private_sources src/cap_webui.c)
ameba_list_append(private_includes
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_cap/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_wifi_mgr/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_http_server/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../cap_im_wechat/include
)

ameba_add_internal_library(cap_webui
    p_SOURCES ${private_sources}
    p_INCLUDES ${private_includes}
    p_COMPILE_OPTIONS -Wno-unused-parameter
)

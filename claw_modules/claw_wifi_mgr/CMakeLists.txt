set(private_sources)
set(private_includes)

ameba_list_append(private_sources src/claw_wifi_mgr.c)
ameba_list_append(private_includes
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_capabilities/cap_scheduler/include
)

ameba_add_internal_library(claw_wifi_mgr
    p_SOURCES ${private_sources}
    p_INCLUDES ${private_includes}
    p_COMPILE_OPTIONS -Wno-unused-parameter
)

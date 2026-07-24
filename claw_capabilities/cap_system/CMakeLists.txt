set(private_sources)
set(private_includes)

ameba_list_append(private_sources src/cap_system.c)
ameba_list_append(private_includes
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_cap/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_cap_registry/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_config/include
)

ameba_add_internal_library(cap_system
    p_SOURCES ${private_sources}
    p_INCLUDES ${private_includes}
    p_COMPILE_OPTIONS -Wno-unused-parameter
)

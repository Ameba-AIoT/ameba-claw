set(private_sources)
set(private_includes)

ameba_list_append(private_sources src/cap_files.c)
ameba_list_append(private_includes
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_common/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_cap/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_cap_registry/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_config/include
)
ameba_list_append(private_includes ${CMAKE_CURRENT_SOURCE_DIR}/../cap_lua/include)

ameba_add_internal_library(cap_files
    p_SOURCES ${private_sources}
    p_INCLUDES ${private_includes}
    p_COMPILE_OPTIONS -Wno-unused-parameter
)

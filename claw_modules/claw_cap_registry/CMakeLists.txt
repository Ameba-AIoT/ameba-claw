set(private_sources)
set(private_includes)
set(private_compile_options)

ameba_list_append(private_sources
    src/claw_cap_registry.c
)

ameba_list_append(private_includes
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_config/include
)

ameba_list_append(private_compile_options
    -Wno-unused-parameter
)

ameba_add_internal_library(claw_cap_registry
    p_SOURCES
        ${private_sources}
    p_INCLUDES
        ${private_includes}
    p_COMPILE_OPTIONS
        ${private_compile_options}
)

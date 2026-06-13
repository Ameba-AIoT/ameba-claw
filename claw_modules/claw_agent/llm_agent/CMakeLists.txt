set(private_sources)
set(private_includes)
set(private_definitions)
set(private_compile_options)

ameba_list_append(private_sources
    llm_agent_http.c
)

ameba_list_append(private_includes
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)

ameba_list_append(private_compile_options
    -Wno-unused-parameter
)

ameba_add_internal_library(llm_agent
    p_SOURCES
        ${private_sources}
    p_INCLUDES
        ${private_includes}
    p_DEFINITIONS
        ${private_definitions}
    p_COMPILE_OPTIONS
        ${private_compile_options}
)

# llm_agent is an internal HTTP client used by claw_agent
ameba_add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/llm_agent)

set(private_sources)
set(private_includes)
set(private_definitions)
set(private_compile_options)

ameba_list_append(private_sources
    src/claw_agent.c
    src/claw_agent_llm.c
)

ameba_list_append(private_includes
    include
    src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/llm_agent
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_common/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_cap/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_config/include
)

ameba_list_append(private_compile_options
    -Wno-unused-parameter
)

ameba_add_internal_library(claw_agent
    p_SOURCES ${private_sources}
    p_INCLUDES ${private_includes}
    p_DEFINITIONS ${private_definitions}
    p_COMPILE_OPTIONS ${private_compile_options}
)

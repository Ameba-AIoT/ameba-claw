set(private_sources)
set(private_includes)

ameba_list_append(private_sources
    src/claw_memory.c
    src/claw_memory_extract.c
    src/claw_memory_compact.c
)

ameba_list_append(private_includes
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_agent/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_agent/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_cap/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_agent/llm_agent
)

ameba_add_internal_library(claw_memory
    p_SOURCES ${private_sources}
    p_INCLUDES ${private_includes}
    p_COMPILE_OPTIONS -Wno-unused-parameter
)

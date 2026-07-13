set(private_sources)
set(private_includes)

ameba_list_append(private_sources
    src/cap_session_mgr.c
)

ameba_list_append(private_includes
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_event_dispatcher/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_cap/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_memory/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_agent/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_agent/llm_agent
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

ameba_add_internal_library(cap_session_mgr
    p_SOURCES ${private_sources}
    p_INCLUDES ${private_includes}
    p_COMPILE_OPTIONS -Wno-unused-parameter
)

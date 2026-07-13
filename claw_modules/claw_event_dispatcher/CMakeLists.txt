set(private_sources)
set(private_includes)

ameba_list_append(private_sources
    src/claw_event.c
    src/claw_event_dispatcher.c
    src/claw_event_publisher.c
    src/session_cmd.c
)

ameba_list_append(private_includes
    include
    src
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_cap/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_agent/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_session_mgr/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_agent/llm_agent
)

ameba_add_internal_library(claw_event_dispatcher
    p_SOURCES ${private_sources}
    p_INCLUDES ${private_includes}
    p_COMPILE_OPTIONS -Wno-unused-parameter
)

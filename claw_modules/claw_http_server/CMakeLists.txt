set(private_sources)
set(private_includes)

ameba_list_append(private_sources src/claw_http_server.c)
ameba_list_append(private_includes
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

ameba_add_internal_library(claw_http_server
    p_SOURCES ${private_sources}
    p_INCLUDES ${private_includes}
    p_COMPILE_OPTIONS -Wno-unused-parameter
)

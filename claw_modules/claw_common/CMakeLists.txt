set(private_sources)
set(private_includes)

ameba_list_append(private_sources src/claw_utf8.c)
ameba_list_append(private_includes
    include
)

ameba_add_internal_library(claw_common
    p_SOURCES ${private_sources}
    p_INCLUDES ${private_includes}
    p_COMPILE_OPTIONS -Wno-unused-parameter
)

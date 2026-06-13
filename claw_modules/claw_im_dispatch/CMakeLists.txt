set(private_sources)
set(private_includes)

ameba_list_append(private_sources src/claw_im_dispatch.c)
ameba_list_append(private_includes
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../claw_cap/include
)

ameba_add_internal_library(claw_im_dispatch
    p_SOURCES ${private_sources}
    p_INCLUDES ${private_includes}
    p_COMPILE_OPTIONS -Wno-unused-parameter
)

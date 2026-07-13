set(private_sources)
set(private_includes)
set(private_definitions)
set(private_compile_options)

ameba_list_append(private_sources
    wechat_bot_http.c
    wechat_bot_api.c
    wechat_bot_token.c
)

ameba_list_append(private_compile_options
    -Wno-unused-parameter
)

ameba_list_append(private_includes
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../claw_modules/claw_agent/llm_agent
)

ameba_add_internal_library(wechat_bot
    p_SOURCES
        ${private_sources}
    p_INCLUDES
        ${private_includes}
    p_DEFINITIONS
        ${private_definitions}
    p_COMPILE_OPTIONS
        ${private_compile_options}
)

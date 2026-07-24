set(private_sources)
set(private_includes)
set(private_definitions)

ameba_list_append(private_sources src/cap_audio_stream.c)

if(NOT LUA_MOD_ENABLE_AUDIO)
    ameba_list_append(private_definitions LUA_MOD_ENABLE_AUDIO=0)
endif()

ameba_list_append(private_includes
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_cap/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_cap_registry/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lua/modules/lua_driver_audio/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lua/modules
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lua
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lua/interpreter
)

ameba_add_internal_library(cap_audio_stream
    p_SOURCES ${private_sources}
    p_INCLUDES ${private_includes}
    p_DEFINITIONS ${private_definitions}
    p_COMPILE_OPTIONS -Wno-unused-parameter
)

set(private_sources)
set(private_includes)

# ---- Library sources and includes ----
# cap_lua provides the Lua execution primitive (lua_run by path). It owns the
# strict sandbox, per-run lua_State, timeout + cancel hook, and JSON->Lua table
# argument decoding. Skill management lives in cap_skill_mgr.
ameba_list_append(private_sources
    src/cap_lua.c
)
ameba_list_append(private_includes
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_cap/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lua
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lua/interpreter
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lua/modules
)

ameba_add_internal_library(cap_lua
    p_SOURCES ${private_sources}
    p_INCLUDES ${private_includes}
    p_COMPILE_OPTIONS -Wno-unused-parameter
)

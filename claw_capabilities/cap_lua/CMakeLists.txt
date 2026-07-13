set(private_sources)
set(private_includes)

# ---- Library sources and includes ----
# cap_lua provides the Lua execution primitive (lua_run by path). It owns the
# strict sandbox, per-run lua_State, timeout + cancel hook, and JSON->Lua table
# argument decoding. Skill management lives in cap_skill_mgr.
#
# Split into three translation units (M1-4, behaviour-preserving):
#   cap_lua_runtime.c — execution primitive + synchronous lua_run
#   cap_lua_async.c   — background job table + lua_job_* tools
#   cap_lua_cmd.c     — cap descriptor table + registration (cap_lua_init)
# Shared private surface in include/cap_lua_internal.h.
ameba_list_append(private_sources
    src/cap_lua_runtime.c
    src/cap_lua_async.c
    src/cap_lua_cmd.c
)
ameba_list_append(private_includes
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_cap/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_common/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lua
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lua/interpreter
    ${CMAKE_CURRENT_SOURCE_DIR}/../../lua/modules
)

ameba_add_internal_library(cap_lua
    p_SOURCES ${private_sources}
    p_INCLUDES ${private_includes}
    p_COMPILE_OPTIONS -Wno-unused-parameter
)

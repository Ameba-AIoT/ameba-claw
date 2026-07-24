set(private_sources)
set(private_includes)

# ---- Library sources and includes ----
# Built-in skills are no longer embedded as C strings. They are packed as real
# files into the read-only littlefs region (rolfs:/skills, improvement #12
# Inc 2) from tools/image_scripts/rolfs_content/skills/ and read at runtime via
# the standard VFS file API.
ameba_list_append(private_sources
    src/cap_skill_mgr.c
)
ameba_list_append(private_includes
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_cap/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_cap_registry/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_agent/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../cap_lua/include
)

ameba_add_internal_library(cap_skill_mgr
    p_SOURCES ${private_sources}
    p_INCLUDES ${private_includes}
    p_COMPILE_OPTIONS -Wno-unused-parameter
)

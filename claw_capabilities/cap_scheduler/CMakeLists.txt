set(private_sources)
set(private_includes)

ameba_list_append(private_sources src/cap_scheduler.c src/cron_expr.c)
ameba_list_append(private_includes
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_cap/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_cap_registry/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_wifi_mgr/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_event_dispatcher/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_modules/claw_agent/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../claw_capabilities/cap_time/include
)

ameba_add_internal_library(cap_scheduler
    p_SOURCES ${private_sources}
    p_INCLUDES ${private_includes}
    p_COMPILE_OPTIONS -Wno-unused-parameter
)

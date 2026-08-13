# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "2.0.51")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# ESP-Claw is integrated as an in-process agent runtime.  In addition to the
# core, expose the official sandboxed file tools and Lua runtime used by Chat
# Coding.  Hardware-facing upstream components stay out of this build because
# Metalio owns the display, touch, audio and camera drivers.
set(ESP_CLAW_COMPONENT_ROOT "${CMAKE_SOURCE_DIR}/third_party/esp-claw/components")
list(APPEND EXTRA_COMPONENT_DIRS
    "${ESP_CLAW_COMPONENT_ROOT}/common/http_reuse"
    "${ESP_CLAW_COMPONENT_ROOT}/claw_modules/claw_core"
    "${ESP_CLAW_COMPONENT_ROOT}/claw_modules/claw_utils"
    "${ESP_CLAW_COMPONENT_ROOT}/claw_modules/claw_event_router"
    "${ESP_CLAW_COMPONENT_ROOT}/claw_modules/claw_cap"
    "${ESP_CLAW_COMPONENT_ROOT}/claw_modules/claw_manager"
    "${ESP_CLAW_COMPONENT_ROOT}/claw_modules/claw_skill"
    "${ESP_CLAW_COMPONENT_ROOT}/claw_modules/claw_memory"
    "${ESP_CLAW_COMPONENT_ROOT}/claw_capabilities/cap_files"
    "${ESP_CLAW_COMPONENT_ROOT}/claw_capabilities/cap_lua"
    "${ESP_CLAW_COMPONENT_ROOT}/claw_capabilities/cap_skill_mgr"
    "${ESP_CLAW_COMPONENT_ROOT}/lua_modules/lua_module_delay"
    "${ESP_CLAW_COMPONENT_ROOT}/lua_modules/lua_module_json"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi)

# Override esp-sr packed model with custom wakeword when present
if(EXISTS "${CMAKE_SOURCE_DIR}/wakeword/srmodels.bin")
    if(TARGET srmodels_bin)
        add_custom_target(custom_wakeword_srmodels ALL
            COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_SOURCE_DIR}/wakeword/srmodels.bin"
                "${CMAKE_BINARY_DIR}/srmodels/srmodels.bin"
            COMMENT "Override srmodels.bin with custom wakeword"
        )
        add_dependencies(custom_wakeword_srmodels srmodels_bin)
        if(TARGET flash)
            add_dependencies(flash custom_wakeword_srmodels)
        endif()
        message(STATUS "Custom wakeword enabled: wakeword/srmodels.bin -> build/srmodels/srmodels.bin")
    else()
        message(WARNING "wakeword/srmodels.bin exists but srmodels_bin target not found; custom wakeword override skipped")
    endif()
endif()

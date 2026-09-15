cmake_minimum_required(VERSION 3.25)
project(descent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The shipping build resolves dependencies through vcpkg (see CMakePresets.json).
# A Linux dev container has no vcpkg; cmake/LinuxDeps.cmake defines the same
# target names from distribution packages plus .deps/. Same dependency set
# either way -- see the allowlist in CLAUDE.md.
if(VCPKG_TOOLCHAIN OR MSVC)
    find_package(glfw3 CONFIG REQUIRED)
    find_package(glad CONFIG REQUIRED)
    find_package(glm CONFIG REQUIRED)
    find_package(imgui CONFIG REQUIRED)
    find_package(Stb REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)
    find_package(tinyobjloader CONFIG REQUIRED)
else()
    include(${CMAKE_SOURCE_DIR}/cmake/LinuxDeps.cmake)
endif()

add_executable(descent
    src/main.cpp
    src/audio/audio.cpp
    src/audio/device.cpp
    src/audio/mixer.cpp
    src/audio/sound_test.cpp
    src/audio/wav.cpp
    src/core/input.cpp
    src/core/alloc_count.cpp
    src/core/log.cpp
    src/core/options.cpp
    src/core/window.cpp
    src/game/asset_check.cpp
    src/game/collision_test.cpp
    src/game/input_actions.cpp
    src/game/level.cpp
    src/game/level_list.cpp
    src/game/light_test.cpp
    src/game/visibility.cpp
    src/game/loop_test.cpp
    src/game/menu.cpp
    src/game/progress.cpp
    src/game/proxies.cpp
    src/game/robots_test.cpp
    src/game/session.cpp
    src/game/settings.cpp
    src/game/weapons_test.cpp
    src/game/world_test.cpp
    src/render/debug.cpp
    src/render/billboards.cpp
    src/render/comm_clip.cpp
    src/render/debug_lines.cpp
    src/render/font.cpp
    src/render/gpu_timer.cpp
    src/render/hud.cpp
    src/render/level_mesh.cpp
    src/render/mesh.cpp
    src/render/model.cpp
    src/render/procedural.cpp
    src/render/renderer.cpp
    src/render/screenshot.cpp
    src/render/shader.cpp
    src/render/shadow.cpp
    src/render/signs.cpp
    src/render/sprites.cpp
    src/render/stb_impl.cpp
    src/render/target.cpp
    src/render/texture_array.cpp
    src/sim/collision.cpp
    src/sim/combat.cpp
    src/sim/flight.cpp
    src/sim/flight_test.cpp
    src/sim/gameflow.cpp
    src/sim/lighting.cpp
    src/sim/props.cpp
    src/sim/robots.cpp
    src/sim/ship.cpp
    src/sim/weapons.cpp
)

target_include_directories(descent PRIVATE src)
target_include_directories(descent SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
# miniaudio is vendored rather than resolved through vcpkg, which has no port
# for it, or through apt. See third_party/miniaudio/README.md. SYSTEM so its
# four megabytes of single-header C do not answer to this project's /W4.
target_include_directories(descent SYSTEM PRIVATE third_party/miniaudio)
target_link_libraries(descent PRIVATE
    glfw
    glad::glad
    glm::glm
    imgui::imgui
    nlohmann_json::nlohmann_json
    tinyobjloader::tinyobjloader
)
# What miniaudio's backends need. On Windows it reaches WASAPI through
# libraries the CRT already links; on Linux it dlopen()s ALSA and PulseAudio at
# runtime, so there is nothing to link but libdl, libm and pthreads - which is
# also why the Linux build needs no new -dev package for sound.
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(descent PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
target_compile_definitions(descent PRIVATE
    GLFW_INCLUDE_NONE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
    $<$<CONFIG:Debug>:DVK_DEBUG=1>
)
if(MSVC)
    target_compile_options(descent PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
else()
    target_compile_options(descent PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT descent)
set_target_properties(descent PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:descent>")

# Shaders and assets are loaded at runtime relative to the executable, so they
# are copied next to it.
#
# A separate always-run target rather than a POST_BUILD step on `descent`:
# POST_BUILD only fires when the executable relinks, so editing a level or a
# shader and rebuilding would leave the old copy in place and the change would
# appear not to have happened. That is an unusually annoying bug to chase,
# because the source on disk is right and the running program disagrees.
add_custom_target(descent_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:descent>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets"  "$<TARGET_FILE_DIR:descent>/assets"
    COMMENT "Copying shaders/ and assets/ next to the executable"
    VERBATIM
)
add_dependencies(descent_assets descent)

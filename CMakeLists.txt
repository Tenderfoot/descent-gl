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
    src/core/input.cpp
    src/core/log.cpp
    src/core/options.cpp
    src/core/window.cpp
    src/render/debug.cpp
    src/render/mesh.cpp
    src/render/renderer.cpp
    src/render/screenshot.cpp
    src/render/shader.cpp
    src/render/stb_impl.cpp
    src/render/target.cpp
    src/render/texture.cpp
)

target_include_directories(descent PRIVATE src)
target_include_directories(descent SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
target_link_libraries(descent PRIVATE
    glfw
    glad::glad
    glm::glm
    imgui::imgui
    nlohmann_json::nlohmann_json
    tinyobjloader::tinyobjloader
)
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

# Shaders and assets are loaded at runtime relative to the executable, so
# copy them next to it after every build.
add_custom_command(TARGET descent POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:descent>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets"  "$<TARGET_FILE_DIR:descent>/assets"
    COMMENT "Copying shaders/ and assets/ next to the executable"
)

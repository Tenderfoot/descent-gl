#!/usr/bin/env bash
# Fetch the dependencies a Linux build needs that are not usable distribution
# packages, into .deps/ (git-ignored). See cmake/LinuxDeps.cmake.
#
# The shipping build is MSVC + vcpkg; this exists so the project can be built
# and smoke-run in a Linux dev container. Requires root for apt.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
deps="$root/.deps"
imgui_version="v1.90.1"

echo "==> distribution packages"
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git python3-pip \
    libglfw3-dev libglm-dev nlohmann-json3-dev libstb-dev libtinyobjloader-dev liblua5.4-dev \
    libgl-dev libgl1-mesa-dri mesa-vulkan-drivers mesa-utils xvfb x11-utils \
    libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev

mkdir -p "$deps"

echo "==> glad (GL 4.6 core)"
if [ ! -f "$deps/glad/src/gl.c" ]; then
    pip install --quiet --break-system-packages glad2
    glad --api gl:core=4.6 --extensions GL_KHR_debug --out-path "$deps/glad" c
fi

# glad2 names its loader gladLoadGL/GLADloadfunc; vcpkg ships glad1, whose
# names src/main.cpp uses. This shim keeps that one call site identical on both
# paths rather than forking it behind an #ifdef.
cat > "$deps/glad/include/glad/glad.h" <<'SHIM'
#pragma once
// Compatibility shim: glad1 spelling on top of a glad2-generated loader.
// Present only in the Linux build; the vcpkg build uses glad1 directly.
#include <glad/gl.h>
typedef GLADloadfunc GLADloadproc;
#define gladLoadGLLoader(load) gladLoadGL((GLADloadfunc)(load))
SHIM

echo "==> Dear ImGui $imgui_version"
if [ ! -f "$deps/imgui/imgui.cpp" ]; then
    git clone --depth 1 --branch "$imgui_version" \
        https://github.com/ocornut/imgui.git "$deps/imgui"
fi

echo "==> done. Configure with: cmake --preset linux"

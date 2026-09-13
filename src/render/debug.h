#pragma once

namespace dvk::gl {

// Installs the KHR_debug callback. In Debug builds any GL_DEBUG_SEVERITY_HIGH
// message aborts, which is this project's equivalent of treating a Vulkan
// validation error as fatal: no milestone is complete with one outstanding.
// Requires the context to have been created with GLFW_OPENGL_DEBUG_CONTEXT.
void installDebugCallback(bool fatalOnHigh);

} // namespace dvk::gl

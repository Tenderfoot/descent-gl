#include "render/debug.h"

#include "core/log.h"
#include "render/gl.h"

namespace dvk::gl {

namespace {

bool g_fatalOnHigh = false;

const char* sourceName(GLenum v) {
    switch (v) {
        case GL_DEBUG_SOURCE_API: return "api";
        case GL_DEBUG_SOURCE_WINDOW_SYSTEM: return "window";
        case GL_DEBUG_SOURCE_SHADER_COMPILER: return "shader";
        case GL_DEBUG_SOURCE_THIRD_PARTY: return "thirdparty";
        case GL_DEBUG_SOURCE_APPLICATION: return "app";
        default: return "other";
    }
}

const char* typeName(GLenum v) {
    switch (v) {
        case GL_DEBUG_TYPE_ERROR: return "error";
        case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: return "deprecated";
        case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR: return "undefined";
        case GL_DEBUG_TYPE_PORTABILITY: return "portability";
        case GL_DEBUG_TYPE_PERFORMANCE: return "performance";
        case GL_DEBUG_TYPE_MARKER: return "marker";
        default: return "other";
    }
}

void GLAPIENTRY callback(GLenum source, GLenum type, GLuint id, GLenum severity,
                         GLsizei /*length*/, const GLchar* message, const void* /*user*/) {
    // NVIDIA emits this for every buffer that lives in video memory.
    if (id == 131185) return;

    if (severity == GL_DEBUG_SEVERITY_HIGH) {
        if (g_fatalOnHigh) {
            log::fatal("GL [{}/{}] id={} {}", sourceName(source), typeName(type), id, message);
        }
        log::error("GL [{}/{}] id={} {}", sourceName(source), typeName(type), id, message);
    } else if (severity == GL_DEBUG_SEVERITY_MEDIUM) {
        log::warn("GL [{}/{}] id={} {}", sourceName(source), typeName(type), id, message);
    } else if (severity == GL_DEBUG_SEVERITY_LOW) {
        log::info("GL [{}/{}] id={} {}", sourceName(source), typeName(type), id, message);
    }
    // GL_DEBUG_SEVERITY_NOTIFICATION is dropped; it is mostly driver chatter.
}

} // namespace

void installDebugCallback(bool fatalOnHigh) {
    GLint flags = 0;
    glGetIntegerv(GL_CONTEXT_FLAGS, &flags);
    if (!(flags & GL_CONTEXT_FLAG_DEBUG_BIT)) {
        log::warn("no debug context: KHR_debug output unavailable");
        return;
    }
    g_fatalOnHigh = fatalOnHigh;
    glEnable(GL_DEBUG_OUTPUT);
    // Synchronous so the abort stack points at the offending call.
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(callback, nullptr);
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
    log::info("KHR_debug installed (fatal on high severity: {})", fatalOnHigh);
}

} // namespace dvk::gl

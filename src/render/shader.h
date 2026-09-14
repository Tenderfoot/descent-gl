#pragma once

#include "render/gl.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <string>
#include <unordered_map>

namespace dvk::gl {

// A linked vertex+fragment program. Sources are read from disk at construction;
// a compile or link failure is fatal and prints the driver's info log.
class Shader {
public:
    Shader(const std::string& vertPath, const std::string& fragPath);
    ~Shader();
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    void bind() const { glUseProgram(program_); }

    void setMat4(const char* name, const glm::mat4& m) const;
    void setVec3(const char* name, const glm::vec3& v) const;
    void setVec4(const char* name, const glm::vec4& v) const;
    void setFloat(const char* name, float v) const;
    void setInt(const char* name, int v) const;

private:
    // Uniform locations are looked up once. The per-light passes set half a
    // dozen uniforms per light per frame, and glGetUniformLocation is a string
    // compare against the program's uniform table every time.
    GLint location(const char* name) const;

    GLuint program_ = 0;
    mutable std::unordered_map<std::string, GLint> locations_;
};

} // namespace dvk::gl

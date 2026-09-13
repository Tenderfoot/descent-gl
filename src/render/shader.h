#pragma once

#include "render/gl.h"

#include <glm/mat4x4.hpp>
#include <string>

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

private:
    GLuint program_ = 0;
};

} // namespace dvk::gl

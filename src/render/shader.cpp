#include "render/shader.h"

#include "core/log.h"

#include <glm/gtc/type_ptr.hpp>

#include <fstream>
#include <sstream>
#include <vector>

namespace dvk::gl {

namespace {

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) log::fatal("cannot open shader '{}' (is it next to the executable?)", path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

GLuint compile(GLenum stage, const std::string& src, const std::string& path) {
    const GLuint id = glCreateShader(stage);
    const char* p = src.c_str();
    const auto len = static_cast<GLint>(src.size());
    glShaderSource(id, 1, &p, &len);
    glCompileShader(id);

    GLint ok = GL_FALSE;
    glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint n = 0;
        glGetShaderiv(id, GL_INFO_LOG_LENGTH, &n);
        std::vector<char> log(static_cast<size_t>(n > 0 ? n : 1));
        glGetShaderInfoLog(id, n, nullptr, log.data());
        log::fatal("compiling '{}':\n{}", path, log.data());
    }
    return id;
}

} // namespace

Shader::Shader(const std::string& vertPath, const std::string& fragPath) {
    const GLuint vs = compile(GL_VERTEX_SHADER, readFile(vertPath), vertPath);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, readFile(fragPath), fragPath);

    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);

    GLint ok = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint n = 0;
        glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &n);
        std::vector<char> log(static_cast<size_t>(n > 0 ? n : 1));
        glGetProgramInfoLog(program_, n, nullptr, log.data());
        log::fatal("linking '{}' + '{}':\n{}", vertPath, fragPath, log.data());
    }

    glDetachShader(program_, vs);
    glDetachShader(program_, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
}

Shader::~Shader() {
    if (program_) glDeleteProgram(program_);
}

GLint Shader::location(const char* name) const {
    if (auto it = locations_.find(name); it != locations_.end()) return it->second;
    const GLint loc = glGetUniformLocation(program_, name);
    locations_.emplace(name, loc);
    return loc;
}

void Shader::setMat4(const char* name, const glm::mat4& m) const {
    const GLint loc = location(name);
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, glm::value_ptr(m));
}

void Shader::setVec3(const char* name, const glm::vec3& v) const {
    const GLint loc = location(name);
    if (loc >= 0) glUniform3f(loc, v.x, v.y, v.z);
}

void Shader::setVec2(const char* name, const glm::vec2& v) const {
    const GLint loc = location(name);
    if (loc >= 0) glUniform2f(loc, v.x, v.y);
}

void Shader::setVec4(const char* name, const glm::vec4& v) const {
    const GLint loc = location(name);
    if (loc >= 0) glUniform4f(loc, v.x, v.y, v.z, v.w);
}

void Shader::setFloat(const char* name, float v) const {
    const GLint loc = location(name);
    if (loc >= 0) glUniform1f(loc, v);
}

void Shader::setInt(const char* name, int v) const {
    const GLint loc = location(name);
    if (loc >= 0) glUniform1i(loc, v);
}

} // namespace dvk::gl

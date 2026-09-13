#include "render/renderer.h"

#include "core/log.h"
#include "game/state.h"
#include "render/camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>

namespace dvk::gl {

namespace {
constexpr float kFovY = glm::radians(70.0f);
constexpr float kZNear = 0.05f;
} // namespace

Renderer::Renderer() {
    log::info("GL {} | {} | {}", reinterpret_cast<const char*>(glGetString(GL_VERSION)),
              reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
              reinterpret_cast<const char*>(glGetString(GL_VENDOR)));

    // Reversed-Z: clip space z in [0,1], near = 1, far = 0. See camera.h.
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glDepthFunc(GL_GEQUAL);
    glClearDepth(0.0);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glEnable(GL_MULTISAMPLE);

    shader_ = std::make_unique<Shader>("shaders/textured.vert", "shaders/textured.frag");
    checker_ = std::make_unique<Texture2D>("assets/textures/checker.png");
    cube_ = std::make_unique<Mesh>(makeCube());

    glCreateBuffers(1, &cameraUbo_);
    glNamedBufferStorage(cameraUbo_, sizeof(CameraBlock), nullptr, GL_DYNAMIC_STORAGE_BIT);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, cameraUbo_);
}

void Renderer::beginFrame(int fbWidth, int fbHeight) {
    target_.resize(fbWidth, fbHeight, kSamples);
    target_.bind();
    glClearColor(0.02f, 0.025f, 0.035f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void Renderer::drawScene(const RenderState& rs) {
    const float aspect = target_.height() > 0
                             ? static_cast<float>(target_.width()) / static_cast<float>(target_.height())
                             : 1.0f;

    const glm::vec3 forward{
        std::cos(rs.camPitch) * std::sin(rs.camYaw),
        std::sin(rs.camPitch),
        -std::cos(rs.camPitch) * std::cos(rs.camYaw),
    };

    CameraBlock cam;
    cam.view = glm::lookAt(rs.camPos, rs.camPos + forward, glm::vec3(0.0f, 1.0f, 0.0f));
    cam.proj = infiniteReversedZ(kFovY, aspect, kZNear);
    glNamedBufferSubData(cameraUbo_, 0, sizeof(CameraBlock), &cam);

    shader_->bind();
    checker_->bind(0);

    const glm::mat4 model =
        glm::rotate(glm::mat4(1.0f), rs.cubeAngle, glm::normalize(glm::vec3(0.3f, 1.0f, 0.15f)));
    shader_->setMat4("uModel", model);
    cube_->draw();
}

void Renderer::resolve() {
    target_.resolveToDefault();
}

} // namespace dvk::gl

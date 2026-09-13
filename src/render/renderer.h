#pragma once

#include "render/gl.h"
#include "render/mesh.h"
#include "render/shader.h"
#include "render/target.h"
#include "render/texture.h"

#include <glm/mat4x4.hpp>
#include <memory>

namespace dvk {
struct RenderState;
}

namespace dvk::gl {

// Owns every GL object and draws one frame.
//
// Frame shape in M0:
//   bind MSAA target -> clear -> scene pass -> resolve (blit) to default
//   -> ImGui on the default framebuffer -> swap.
//
// ImGui is drawn after the resolve rather than into the MSAA target: its
// geometry is axis-aligned text and rectangles that gain nothing from
// multisampling, and keeping it out means the resolve carries only scene
// colour. M2 inserts the depth prepass and the per-light passes between the
// clear and the resolve.
class Renderer {
public:
    static constexpr GLsizei kSamples = 4;

    Renderer();

    void beginFrame(int fbWidth, int fbHeight);
    void drawScene(const RenderState& rs);
    void resolve();

private:
    struct CameraBlock {
        glm::mat4 view;
        glm::mat4 proj;
    };

    std::unique_ptr<Shader> shader_;
    std::unique_ptr<Texture2D> checker_;
    std::unique_ptr<Mesh> cube_;
    MsaaTarget target_;
    GLuint cameraUbo_ = 0;
};

} // namespace dvk::gl

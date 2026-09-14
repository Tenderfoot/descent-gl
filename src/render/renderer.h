#pragma once

#include "render/gl.h"
#include "render/level_mesh.h"
#include "render/mesh.h"
#include "render/model.h"
#include "render/shader.h"
#include "render/target.h"
#include "render/texture_array.h"

#include <glm/mat4x4.hpp>
#include <memory>
#include <vector>

namespace dvk {
struct Level;
struct RenderState;
class PortalWalk;
}

namespace dvk::gl {

// Owns every GL object and draws one frame.
//
// Frame shape from M2 on:
//
//   bind MSAA target, clear to black
//   depth prepass                    all visible geometry, depth only
//   for each light reaching the visible set:
//       [M3 inserts the stencil clear and the shadow volume pass here]
//       additive lighting pass       blend ONE,ONE; depth EQUAL; only this
//                                    light's segments
//   unlit pass                       emissive and debug geometry
//   resolve (blit) to default -> ImGui -> swap
//
// There is no ambient pass and there will never be one. A surface no light
// reaches stays at the clear colour.
//
// ImGui is drawn after the resolve rather than into the MSAA target: its
// geometry is axis-aligned text and rectangles that gain nothing from
// multisampling, and keeping it out means the resolve carries only scene
// colour.
class Renderer {
public:
    static constexpr GLsizei kSamples = 4;

    explicit Renderer(const Level& level);

    void beginFrame(int fbWidth, int fbHeight);
    void drawScene(const RenderState& rs, const Level& level, const PortalWalk& walk);
    void resolve();

    // The same view-projection the frame will be drawn with. The portal walk
    // needs it, and building it twice from two places is how the cull and the
    // draw quietly drift apart. Valid after beginFrame, which sizes the target.
    glm::mat4 viewProj(const RenderState& rs) const;

    // Per-frame counters for the debug overlay.
    struct Stats {
        int segmentsDrawn = 0;     // in the depth prepass
        int lightsConsidered = 0;
        int lightsDrawn = 0;
        int lightSegmentDraws = 0; // segment draw calls across every light pass
    };
    const Stats& stats() const { return stats_; }

    // Debug toggles, driven from ImGui.
    bool drawLightGizmos = true;

    size_t shipTriangleCount() const { return shipModel_.triangleCount(); }
    int levelTriangleCount() const { return levelMesh_->triangleCount(); }

private:
    struct CameraBlock {
        glm::mat4 view;
        glm::mat4 proj;
    };

    glm::mat4 buildView(const RenderState& rs) const;
    glm::vec3 eyePos(const RenderState& rs) const;
    glm::mat4 shipModelMatrix(const RenderState& rs) const;
    void depthPrepass(const RenderState& rs, const PortalWalk& walk);
    void lightPasses(const RenderState& rs, const Level& level, const PortalWalk& walk);
    void unlitPass(const Level& level, const PortalWalk& walk);

    std::unique_ptr<Shader> depthShader_;
    std::unique_ptr<Shader> wallShader_;
    std::unique_ptr<Shader> unlitShader_;

    std::unique_ptr<TextureArray> walls_;
    std::unique_ptr<LevelMesh> levelMesh_;
    std::unique_ptr<Mesh> ship_;
    std::unique_ptr<Mesh> gizmo_;

    ModelData shipModel_;

    // Reused across frames so the per-light loop allocates nothing.
    std::vector<int> lit_;

    MsaaTarget target_;
    GLuint cameraUbo_ = 0;
    Stats stats_;
};

} // namespace dvk::gl

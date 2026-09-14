#pragma once

#include "render/gl.h"
#include "render/billboards.h"
#include "render/debug_lines.h"
#include "render/hud.h"
#include "render/level_mesh.h"
#include "render/mesh.h"
#include "render/model.h"
#include "render/shader.h"
#include "render/shadow.h"
#include "render/target.h"
#include "render/texture_array.h"

#include <glm/mat4x4.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dvk {
struct Level;
namespace sim {
struct ContactLog;
struct RayHit;
struct RobotConfig;
}
struct Light;
struct RenderState;
class PortalWalk;
}

namespace dvk::gl {

// Owns every GL object and draws one frame.
//
// Frame shape from M3 on:
//
//   bind MSAA target, clear to black
//   depth prepass                    all visible geometry, depth only
//   for each light reaching the visible set:
//       clear stencil
//       shadow volume pass           no colour write, depth-fail counting
//       additive lighting pass       blend ONE,ONE; depth EQUAL; stencil == 0;
//                                    only this light's segments
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
// Everything the HUD draws, gathered by the game layer so the renderer does not
// reach into simulation state.
struct HudData {
    bool show = true;
    float shields = 100.0f;
    float energy = 100.0f;
    const char* weaponName = "laser";
    int laserLevel = 1;
    int ammo = -1;          // -1 when the weapon runs on energy
    int flares = 0;
    bool keys[3] = {false, false, false};
    float countdown = -1.0f;   // < 0 when the reactor is still standing
    int phase = 0;             // 0 playing, 1 countdown, 2 won, 3 lost
    const char* message = "";
    float elapsed = 0.0f;
    int robotsDestroyed = 0;
    int robotsTotal = 0;
};

class Renderer {
public:
    static constexpr GLsizei kSamples = 4;

    Renderer(const Level& level, const sim::RobotConfig& robots);

    void beginFrame(int fbWidth, int fbHeight);
    // `dynamicLights` holds everything that is not baked into the level: the
    // headlight, every bolt in flight, every burning flare, every explosion.
    // Passed in rather than stored because it is simulation state, rebuilt
    // every frame by the game layer.
    void drawScene(const RenderState& rs, const Level& level, const PortalWalk& walk,
                   const std::vector<Light>& dynamicLights, int shadowBudget,
                   const HudData& hud);

    // Rebuilds the debug line list for this frame. Call before drawScene.
    // Everything here is a toggle because the mine is dark and these lines are
    // the only way to see what collision believed.
    void buildDebugLines(const Level& level, const RenderState& rs,
                         const sim::ContactLog& contacts, const sim::RayHit& noseRay);
    void resolve();

    // The same view-projection the frame will be drawn with. The portal walk
    // needs it, and building it twice from two places is how the cull and the
    // draw quietly drift apart. Valid after beginFrame, which sizes the target.
    glm::mat4 viewProj(const RenderState& rs) const;

    // Per-frame counters for the debug overlay.
    struct Stats {
        int segmentsDrawn = 0;
        int lightsConsidered = 0;
        int lightsDrawn = 0;
        int lightSegmentDraws = 0;
        int shadowCasters = 0;
        int silhouetteEdges = 0;
        int volumeTriangles = 0;
        int shadowBudget = 0;      // the cap in force this frame
        int shadowLights = 0;      // how many of it were used
        int budgetDenied = 0;      // lights that lit but did not cast
        int billboards = 0;
    };
    const Stats& stats() const { return stats_; }

    // What each light contributed, for the per-light debug readout.
    struct LightStats {
        int silhouetteEdges = 0;
        int volumeTriangles = 0;
        int segments = 0;
    };
    const std::vector<LightStats>& lightStats() const { return lightStats_; }

    // Debug toggles, driven from ImGui.
    bool drawLightGizmos = true;
    bool shadowsEnabled = true;
    bool drawVolumeWireframe = false;
    bool showStencil = false;
    bool drawWallNormals = false;
    bool drawContacts = true;
    bool drawNoseRay = false;

    size_t shipTriangleCount() const { return shipModel_.triangleCount(); }
    int levelTriangleCount() const { return levelMesh_->triangleCount(); }
    size_t mineOccluderTriangles() const { return mineMesh_.triangleCount(); }

private:
    struct CameraBlock {
        glm::mat4 view;
        glm::mat4 proj;
    };

    // One drawable, shadow-casting thing that is not the mine.
    struct Prop {
        const OccluderMesh* occluderMesh = nullptr;
        const Mesh* mesh = nullptr;
        Occluder occluder;
        glm::mat4 model{1.0f};
        int segment = -1;
        int textureLayer = 0;
    };

    glm::mat4 buildView(const RenderState& rs) const;
    glm::vec3 eyePos(const RenderState& rs) const;
    glm::mat4 shipModelMatrix(const RenderState& rs) const;

    void loadProps(const Level& level, const sim::RobotConfig& robots);
    void loadRobotMeshes(const sim::RobotConfig& robots);
    void updateRobotOccluders(const RenderState& rs);
    void gatherLights(const Level& level, const std::vector<Light>& dynamicLights,
                      const glm::vec3& eye, const PortalWalk& walk, int shadowBudget);
    void buildBillboards(const RenderState& rs);
    void buildShadowVolumes(const RenderState& rs, const PortalWalk& walk);
    void depthPrepass(const RenderState& rs, const PortalWalk& walk);
    void lightPasses(const RenderState& rs, const PortalWalk& walk);
    void unlitPass(const RenderState& rs, const Level& level, const PortalWalk& walk);
    void buildDoors(const RenderState& rs, const Level& level);
    void buildHud(const HudData& hud);
    void hudPass();

    void stencilPass(ShadowVolumes::Range range);
    void visualiseStencil();

    std::unique_ptr<Shader> depthShader_;
    std::unique_ptr<Shader> wallShader_;
    std::unique_ptr<Shader> unlitShader_;
    std::unique_ptr<Shader> shadowShader_;
    std::unique_ptr<Shader> fullscreenShader_;
    std::unique_ptr<Shader> debugLineShader_;

    std::unique_ptr<TextureArray> walls_;
    std::unique_ptr<LevelMesh> levelMesh_;
    std::unique_ptr<Mesh> ship_;
    std::unique_ptr<Mesh> gizmo_;

    ModelData shipModel_;
    OccluderMesh shipOccluderMesh_;
    OccluderMesh mineMesh_;
    Occluder mineOccluder_;
    Occluder shipOccluder_;

    // Meshes shared by every instance of a given object type.
    std::unordered_map<std::string, ModelData> propModels_;
    std::unordered_map<std::string, Mesh> propMeshes_;
    std::unordered_map<std::string, OccluderMesh> propOccluders_;
    std::vector<Prop> props_;

    // One entry per robot type, and one occluder per robot slot. A slot's
    // occluder is only rebuilt when the robot in it changes type, which in
    // practice is once.
    struct RobotType {
        const Mesh* mesh = nullptr;
        const OccluderMesh* occluder = nullptr;
        int textureLayer = 0;
    };
    std::vector<RobotType> robotTypes_;
    std::vector<Occluder> robotOccluders_;
    std::vector<int> robotOccluderType_;
    const sim::RobotConfig* robotConfig_ = nullptr;

    // Every light being drawn this frame: the level's, then the headlight if
    // it is on. One list so the shadow build and the light passes cannot
    // disagree about what exists.
    std::vector<const Light*> active_;
    // Parallel to active_: the ship is never an occluder for its own
    // headlight, and the headlight's own index is how that is known.
    int headlightIndex_ = -1;
    // Where the level's own lights end and the dynamic ones begin. Only the
    // static ones get a debug gizmo: a bolt already draws itself as a
    // billboard, and a second marker on top of it just looks like a bug.
    size_t staticLightCount_ = 0;
    // Parallel to active_: whether this light won a slot in the shadow budget.
    std::vector<uint8_t> castsShadow_;
    // Scratch for the budget sort: index into active_ plus distance to the eye.
    std::vector<std::pair<float, int>> shadowOrder_;

    DebugLines debugLines_;
    Billboards billboards_;
    std::unique_ptr<Shader> billboardShader_;
    Hud hud_;
    std::unique_ptr<Shader> hudShader_;

    // Two sliding halves per door, plus one occluder each so a shut door casts
    // a shadow like the wall it is standing in for.
    struct DoorSlab {
        glm::mat4 model{1.0f};
        int segmentA = -1;
        int segmentB = -1;
    };
    std::unique_ptr<Mesh> slabMesh_;
    OccluderMesh slabOccluderMesh_;
    std::vector<DoorSlab> doorSlabs_;
    std::vector<Occluder> doorOccluders_;

    ShadowVolumes volumes_;
    std::vector<ShadowVolumes::Range> ranges_;   // one per light, same order
    std::vector<LightStats> lightStats_;
    std::vector<uint8_t> segmentMask_;
    std::vector<int> lit_;

    GLuint emptyVao_ = 0;   // for the full-screen stencil visualiser

    MsaaTarget target_;
    GLuint cameraUbo_ = 0;
    Stats stats_;
};

} // namespace dvk::gl

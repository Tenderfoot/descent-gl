#pragma once

#include "render/gl.h"
#include "render/billboards.h"
#include "render/debug_lines.h"
#include "render/gpu_timer.h"
#include "render/hud.h"
#include "render/level_mesh.h"
#include "render/mesh.h"
#include "render/model.h"
#include "render/shader.h"
#include "render/shadow.h"
#include "render/signs.h"
#include "render/target.h"
#include "render/texture_array.h"

#include <glm/mat4x4.hpp>
#include <array>
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
struct MenuView;
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
    const char* primaryWeaponName = "laser";
    int primaryLaserLevel = 1;
    int primaryAmmo = -1;          // -1 when the weapon runs on energy
    const char* secondaryWeaponName = "concussion";
    int secondaryAmmo = 0;
    int flares = 0;
    bool keys[3] = {false, false, false};
    float countdown = -1.0f;   // < 0 when the reactor is still standing
    int phase = 0;             // 0 playing, 1 countdown, 2 won, 3 lost
    const char* message = "";
    float elapsed = 0.0f;
    int robotsDestroyed = 0;
    int robotsTotal = 0;

    bool hasTarget = false;
    bool targetDisabled = false;
    const char* targetName = "";
    const char* targetStatus = "";
    float targetHealth01 = 0.0f;
    float targetEnergy01 = 0.0f;
    float targetFade = 0.0f;

    // The level-opening transmission is simulation-timed. The renderer turns
    // the time into a texture-array layer; no video decode happens per frame.
    bool commActive = false;
    float commTime = 0.0f;

    // Darkness, as the simulation understands it rather than as the screen
    // shows it. `illumination` is the light reaching the player and
    // `darkThreshold` is the value it has to stay under for the mine to hide
    // them; both arrive as plain floats because `sim::kDarkThreshold` lives
    // behind a simulation header this one must not include.
    //
    // `seen` is the separate question the robots actually act on, and it is not
    // a function of `illumination` alone: a player flying an unlit corridor with
    // the headlight on is carrying a lamp and is seen anyway. `seenByHeadlight`
    // says that is what happened, so the readout can point at the key that
    // fixes it.
    float illumination = 0.0f;
    float darkThreshold = 0.25f;
    bool seen = false;
    bool seenByHeadlight = false;

    // 1 the instant the hull is hit, 0 once the flinch has faded. Drawn even
    // when `show` is false: it is damage feedback rather than a readout, and a
    // screenshot taken with the HUD hidden should still show the player being
    // shot at.
    float damageFlash = 0.0f;

    // Where the guns are pointed, in world space, and whether that question has
    // an answer this frame. The reticle is drawn over this point rather than at
    // the centre of the window.
    //
    // In first person the two are the same pixel, because the camera sits on
    // the firing line and every point on it projects to the centre. The chase
    // camera does not: it looks at the ship from above and behind, so a reticle
    // nailed to the centre of the window marks a spot on the floor well short
    // of whatever the shot would hit. Left false on the automap, which has no
    // firing line on screen to mark.
    glm::vec3 aimPoint{0.0f};
    bool hasAimPoint = false;

    // The ship is under the disabled threshold and the player has no control of
    // it. Held rather than fading, because unlike the flinch it is a state and
    // not an event: the edges stay red and pulsing until something changes.
    bool shipDisabled = false;
    float shipDisabledTime = 0.0f;
};

// The automap, gathered by the game layer for the same reason `HudData` is: the
// renderer draws what it is handed and never reaches into `GameState`.
//
// Everything here is by value. The map is drawn from the segment graph the
// renderer already has, so all the game layer has to supply is which parts of it
// the player has already been through, and which markers they have earned the
// right to see.
struct AutomapData {
    static constexpr int kMaxSegments = 128;
    static constexpr int kMaxMarkers = 16;

    struct Marker {
        glm::vec3 position{0.0f};
        glm::vec3 color{1.0f};
        float size = 1.0f;
        bool hollow = false;   // seen but not yet collected
    };

    bool open = false;
    float yaw = 0.6f;
    float pitch = 0.5f;
    float zoom = 1.0f;

    int segmentCount = 0;
    int visitedCount = 0;
    // Every segment is drawn; `visited` only decides how. A segment the ship has
    // been inside is drawn bright, one it has not is drawn dim, and the portals
    // between the two are the frontier.
    std::array<bool, kMaxSegments> visited{};

    glm::vec3 shipPosition{0.0f};
    glm::vec3 shipForward{0.0f, 0.0f, -1.0f};
    int shipSegment = 0;

    std::array<Marker, kMaxMarkers> markers{};
    int markerCount = 0;
};

class Renderer {
public:
    static constexpr GLsizei kSamples = 4;

    Renderer(const Level& level, const sim::RobotConfig& robots);

    // Point the renderer at a different mine. Rebuilds the level mesh, the
    // mine occluder and the static props; everything else - shaders, wall
    // textures, the ship, the robot meshes - is level-independent and stays.
    // The GL resources of the previous level are released by the same RAII
    // that built them.
    void setLevel(const Level& level);

    void beginFrame(int fbWidth, int fbHeight);
    // `dynamicLights` holds everything that is not baked into the level: the
    // headlight, every bolt in flight, every burning flare, every explosion.
    // Passed in rather than stored because it is simulation state, rebuilt
    // every frame by the game layer.
    void drawScene(const RenderState& rs, const Level& level, const PortalWalk& walk,
                   const std::vector<Light>& dynamicLights, int shadowBudget,
                   const HudData& hud, const MenuView& menu);

    // Draws the automap instead of the mine. The simulation is still running
    // behind it - this replaces the view, not the frame.
    void drawAutomap(const AutomapData& map, const Level& level, const HudData& hud,
                     const MenuView& menu);

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
        // The heaviest single light's volume, and which light it was. M8's plan
        // suggested capping this per light; that is deliberately not done. A
        // truncated volume is an OPEN volume, and depth-fail counting against an
        // open volume is exactly the bug the shadow-closure work fixed - the
        // segment-subset optimization that left holes behind receivers. So the
        // cap is a number to look at, not a limit to enforce: if extraction ever
        // shows up in a profile, this says how big the problem is and where.
        int worstVolumeTriangles = 0;
        int worstVolumeLight = -1;
    };
    const Stats& stats() const { return stats_; }
    const GpuTimers& timers() const { return timers_; }

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
        // Index into Level::objects, which is also the index into the sim's
        // object health. Robots are skipped when props are built, so the
        // prop index and the object index are not the same number.
        int objectIndex = -1;
    };

    // Whether the level object a prop came from is still standing.
    static bool propAlive(const Prop& prop, const RenderState& rs);

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
    void buildDoorSigns(const RenderState& rs, const Level& level, const PortalWalk& walk);
    void buildHud(const HudData& hud, const glm::mat4& viewProj);
    void buildMenu(const MenuView& menu);
    void hudPass();
    void buildAutomapLines(const AutomapData& map, const Level& level);

    void stencilPass(ShadowVolumes::Range range);
    void visualiseStencil();

    std::unique_ptr<Shader> depthShader_;
    std::unique_ptr<Shader> wallShader_;
    std::unique_ptr<Shader> unlitShader_;
    std::unique_ptr<Shader> shadowShader_;
    std::unique_ptr<Shader> fullscreenShader_;
    std::unique_ptr<Shader> debugLineShader_;

    std::unique_ptr<TextureArray> walls_;
    std::unique_ptr<TextureArray> pickupSprites_;
    std::unique_ptr<TextureArray> commFrames_;
    int commFrameCount_ = 0;
    int commWidth_ = 1;
    int commHeight_ = 1;
    float commFramesPerSecond_ = 1.0f;
    float commDisplaySeconds_ = 0.0f;
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
    GpuTimers timers_;
    DebugLines mapLines_;   // separate from debugLines_, which the scene owns
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

    // The "access denied" / "access granted" strips on the keyed doors, rebuilt
    // every frame because they move with the slabs and change with the keys.
    DoorSigns doorSigns_;
    std::unique_ptr<Shader> signShader_;

    ShadowVolumes volumes_;
    std::vector<ShadowVolumes::Range> ranges_;   // one per light, same order
    std::vector<LightStats> lightStats_;
    std::vector<int> lit_;

    GLuint emptyVao_ = 0;   // for the full-screen stencil visualiser

    MsaaTarget target_;
    GLuint cameraUbo_ = 0;
    Stats stats_;
};

} // namespace dvk::gl

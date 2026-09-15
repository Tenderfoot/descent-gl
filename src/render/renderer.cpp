#include "render/renderer.h"

#include "core/log.h"
#include "game/level.h"
#include "game/menu.h"
#include "game/state.h"
#include "game/visibility.h"
#include "sim/collision.h"
#include "sim/robots.h"
#include "render/camera.h"
#include "render/comm_clip.h"
#include "render/procedural.h"
#include "render/sprites.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string_view>

namespace dvk::gl {

namespace {

constexpr float kAutomapFovY = glm::radians(70.0f);
constexpr float kZNear = 0.05f;
constexpr int kWallTileSize = 256;
constexpr int kPickupSpriteSize = 256;
constexpr const char* kCommManifestPath = "assets/comm/intro/comm.json";

constexpr glm::vec3 kChaseOffset{0.0f, 2.6f, 9.0f};
constexpr float kChaseLookAhead = 6.0f;

} // namespace

Renderer::Renderer(const Level& level, const sim::RobotConfig& robots)
    : robotConfig_(&robots) {
    log::info("GL {} | {} | {}", reinterpret_cast<const char*>(glGetString(GL_VERSION)),
              reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
              reinterpret_cast<const char*>(glGetString(GL_VENDOR)));

    // Reversed-Z: clip space z in [0,1], near = 1, far = 0. See camera.h. The
    // infinite far plane is what lets a shadow volume vertex at w = 0 land on
    // the far plane instead of being clipped.
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glDepthFunc(GL_GEQUAL);
    glClearDepth(0.0);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glEnable(GL_MULTISAMPLE);

    // Where the stencil differs between samples inside one pixel - which is
    // exactly a shadow edge - shade per sample rather than once per pixel.
    // Without it the multisampled stencil buys nothing and the hard edges
    // come out stair-stepped, which is the one artifact this technique is
    // supposed to be free of.
    glEnable(GL_SAMPLE_SHADING);
    glMinSampleShading(1.0f);

    depthShader_ = std::make_unique<Shader>("shaders/depth.vert", "shaders/depth.frag");
    wallShader_ = std::make_unique<Shader>("shaders/wall.vert", "shaders/wall.frag");
    unlitShader_ = std::make_unique<Shader>("shaders/unlit.vert", "shaders/unlit.frag");
    shadowShader_ = std::make_unique<Shader>("shaders/shadow.vert", "shaders/shadow.frag");
    fullscreenShader_ =
        std::make_unique<Shader>("shaders/fullscreen.vert", "shaders/fullscreen.frag");
    debugLineShader_ =
        std::make_unique<Shader>("shaders/debugline.vert", "shaders/debugline.frag");
    billboardShader_ =
        std::make_unique<Shader>("shaders/billboard.vert", "shaders/billboard.frag");
    hudShader_ = std::make_unique<Shader>("shaders/hud.vert", "shaders/hud.frag");
    // The HUD's fragment stage, unchanged, behind a vertex stage that puts the
    // quads in the mine instead of on the screen. See render/signs.h.
    signShader_ = std::make_unique<Shader>("shaders/sign.vert", "shaders/hud.frag");

    // One unit cube reused for every door half. A door is a box; there is no
    // reason for it to be anything else.
    //
    // The drawn mesh and the occluder come from ONE ModelData, the same way the
    // ship, the robots and the props do. They used to be two separately
    // hand-written cubes that happened to agree on four of the six faces and
    // split the other two across the opposite diagonal. Every vertex matched,
    // so the volume's front cap started from the same corners - but a quad
    // triangulated the other way has its depth plane set up from different
    // corners, the rasterized interior lands an ulp apart in a scattered
    // pattern, and under GL_GREATER those pixels stop counting the cap. The
    // slab's +Z face was bit-identical and clean; its -Z face fizzed with
    // false self-shadow wherever a light was close enough to make it visible.
    // Hence one cube: see cubeModel() in render/model.h.
    {
        const ModelData slab = cubeModel();
        slabMesh_ = std::make_unique<Mesh>(toMesh(slab));
        slabOccluderMesh_ = buildOccluder(slab.positions, slab.triangles, {}, "door slab");
    }
    doorSlabs_.resize(static_cast<size_t>(sim::kMaxDoors) * 2);
    doorOccluders_.resize(static_cast<size_t>(sim::kMaxDoors) * 2);
    for (Occluder& occluder : doorOccluders_) {
        occluder.reset(&slabOccluderMesh_, glm::mat4(1.0f));
    }
    // Worst case for the signage, so the draw path never grows the buffer:
    // every door keyed and visible, two halves, two faces, two plates each, and
    // a bezel, a plate and one quad per letter of the longest string a sign can
    // carry. It costs a megabyte once and keeps the frame report at "none".
    constexpr size_t kSignQuadsPerPlate = 2 + 24;
    doorSigns_.reserveQuads(static_cast<size_t>(sim::kMaxDoors) * 2 * 2 * 2 * kSignQuadsPerPlate);

    walls_ = std::make_unique<TextureArray>(kWallTileSize, makeWallTiles(kWallTileSize), "walls");
    // Clamped rather than repeating: a sprite is a picture with an edge, and
    // the art is authored far larger than kPickupSpriteSize, so it is resampled
    // once here instead of carrying twenty times the pixels for a quad that is
    // a hundred across on screen.
    pickupSprites_ = std::make_unique<TextureArray>(
        kPickupSpriteSize, loadPickupSprites(kPickupSpriteSize), "pickups",
        TextureArray::Wrap::ClampToEdge);
    {
        CommClip clip = loadCommClip(kCommManifestPath);
        commFrameCount_ = static_cast<int>(clip.frames.size());
        commWidth_ = clip.width;
        commHeight_ = clip.height;
        commFramesPerSecond_ = clip.framesPerSecond;
        commDisplaySeconds_ = clip.displaySeconds;
        commFrames_ = std::make_unique<TextureArray>(
            clip.width, clip.height, clip.frames, "comm intro", TextureArray::Wrap::ClampToEdge);
        // hud.frag is also used by world-space door signs. Bind the optional
        // sampler immediately so that shared program never observes an
        // incomplete texture on unit 3 before the first HUD pass.
        commFrames_->bind(3);
    }

    shipModel_ = loadObj("assets/models/ship.obj");
    ship_ = std::make_unique<Mesh>(toMesh(shipModel_));
    shipOccluderMesh_ =
        buildOccluder(shipModel_.positions, shipModel_.triangles, {}, "assets/models/ship.obj");
    shipOccluder_.reset(&shipOccluderMesh_, glm::mat4(1.0f));

    gizmo_ = std::make_unique<Mesh>(toMesh(cubeModel()));

    loadRobotMeshes(robots);
    setLevel(level);

    glCreateVertexArrays(1, &emptyVao_);

    glCreateBuffers(1, &cameraUbo_);
    glNamedBufferStorage(cameraUbo_, sizeof(CameraBlock), nullptr, GL_DYNAMIC_STORAGE_BIT);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, cameraUbo_);
}

// Everything in the renderer that is a function of which mine is loaded, in
// one place, so switching levels from the menu rebuilds exactly this and the
// shaders, textures, ship and robot meshes are left alone. The prop caches are
// keyed by model name and kept: two mines that both place a pillar load the
// pillar once, and the Prop pointers into those maps stay valid because the
// maps are node-based.
void Renderer::setLevel(const Level& level) {
    levelMesh_ = std::make_unique<LevelMesh>(level);

    // The mine itself is an occluder, and the most important one: without it a
    // light spills through walls into rooms it should never reach.
    //
    // The union of every segment's wall triangles is the closed interior
    // surface of the mine. That holds because segments share vertex indices
    // across portals, so the walls of two joined segments weld along the portal
    // rim instead of leaving a crack. buildOccluder fails loudly if it does
    // not hold, and a failure there means the level file is wrong, not the
    // renderer.
    {
        std::vector<glm::uvec3> triangles;
        std::vector<int> segments;
        triangles.reserve(level.walls.size());
        segments.reserve(level.walls.size());
        for (const WallTriangle& w : level.walls) {
            triangles.emplace_back(static_cast<unsigned>(w.vi[0]), static_cast<unsigned>(w.vi[1]),
                                   static_cast<unsigned>(w.vi[2]));
            segments.push_back(w.segment);
        }
        mineMesh_ = buildOccluder(level.vertices, triangles, segments, level.name + " walls");
        mineOccluder_.reset(&mineMesh_, glm::mat4(1.0f));
    }

    props_.clear();
    loadProps(level, *robotConfig_);

    // Reserves, not resizes: these are rebuilt every frame. A second, larger
    // mine grows them once here rather than during a draw.
    // The unlit pass's worst case, so its buffer never grows mid-frame: every
    // bolt, landed flare, effect, pickup, beam and robot eye at once. Debris
    // and drops ride the bolt list and are already counted by it.
    size_t eyes = 0;
    if (robotConfig_) {
        for (const sim::RobotDef& def : robotConfig_->types) {
            eyes = std::max(eyes, def.eyes.size());
        }
    }
    billboards_.reserveQuads(RenderState{}.bolts.size() + RenderState{}.flashes.size() +
                             RenderState{}.pickups.size() + RenderState{}.beams.size() +
                             RenderState{}.props.size() * 2 +
                             static_cast<size_t>(sim::kMaxRobots) * eyes);

    lit_.reserve(level.segments.size());
    ranges_.reserve(level.lights.size() + 1);
    lightStats_.reserve(level.lights.size() + 1);
}

void Renderer::loadRobotMeshes(const sim::RobotConfig& robots) {
    robotTypes_.reserve(robots.types.size());
    for (const sim::RobotDef& def : robots.types) {
        const std::string path = "assets/models/" + def.model + ".obj";
        if (propModels_.find(def.model) == propModels_.end()) {
            ModelData model = loadObj(path);
            OccluderMesh occluder = buildOccluder(model.positions, model.triangles, {}, path);
            propMeshes_.emplace(def.model, toMesh(model));
            propOccluders_.emplace(def.model, std::move(occluder));
            propModels_.emplace(def.model, std::move(model));
        }
        RobotType type;
        type.mesh = &propMeshes_.at(def.model);
        type.occluder = &propOccluders_.at(def.model);
        type.textureLayer = WallPanel;
        robotTypes_.push_back(type);
    }

    robotOccluders_.resize(sim::kMaxRobots);
    robotOccluderType_.assign(sim::kMaxRobots, -1);
}

bool Renderer::propAlive(const Prop& prop, const RenderState& rs) {
    if (prop.objectIndex < 0 || prop.objectIndex >= rs.objectCount) return true;
    return rs.objectAlive[static_cast<size_t>(prop.objectIndex)];
}

void Renderer::loadProps(const Level& level, const sim::RobotConfig& robots) {
    for (size_t oi = 0; oi < level.objects.size(); ++oi) {
        const LevelObject& obj = level.objects[oi];
        // An object whose type names a robot definition is not scenery: the
        // simulation owns it and it is drawn from the robot list instead.
        if (sim::objectIsRobot(robots, obj.type)) continue;
        if (propModels_.find(obj.type) == propModels_.end()) {
            const std::string path = "assets/models/" + obj.type + ".obj";
            ModelData model = loadObj(path);
            OccluderMesh occluder = buildOccluder(model.positions, model.triangles, {}, path);
            propMeshes_.emplace(obj.type, toMesh(model));
            propOccluders_.emplace(obj.type, std::move(occluder));
            propModels_.emplace(obj.type, std::move(model));
        }

        Prop prop;
        prop.mesh = &propMeshes_.at(obj.type);
        prop.occluderMesh = &propOccluders_.at(obj.type);
        prop.segment = obj.segment;
        prop.objectIndex = static_cast<int>(oi);
        prop.textureLayer = obj.type == "pillar" ? WallRock : WallPanel;
        prop.model = glm::translate(glm::mat4(1.0f), obj.position) *
                     glm::mat4_cast(obj.orientation) * glm::scale(glm::mat4(1.0f), glm::vec3(obj.scale));
        // Static, so the world-space positions and planes are computed once.
        prop.occluder.reset(prop.occluderMesh, prop.model);
        props_.push_back(std::move(prop));
    }
    if (!props_.empty()) log::info("{} static objects placed", props_.size());
}

void Renderer::gatherLights(const Level& level, const std::vector<Light>& dynamicLights,
                            const glm::vec3& eye, const PortalWalk& walk, int shadowBudget) {
    active_.clear();
    for (const Light& light : level.lights) active_.push_back(&light);
    staticLightCount_ = active_.size();

    headlightIndex_ = -1;
    for (size_t i = 0; i < dynamicLights.size(); ++i) {
        // By convention the game layer puts the headlight first, and it is the
        // one the ship must never shadow.
        if (i == 0 && dynamicLights[i].type == LightType::Spot) {
            headlightIndex_ = static_cast<int>(active_.size());
        }
        active_.push_back(&dynamicLights[i]);
    }

    // --- shadow budget ---------------------------------------------------
    //
    // Every light still lights. The budget only decides which of them also
    // cast, because casting is what costs: a silhouette extraction and a
    // stencil pass per light per frame. Under sustained vulcan fire there can
    // be thirty bolts in the air, and thirty stencil passes is not a frame.
    //
    // A light that lights without casting shines straight through every
    // robot, pillar and wall inside its segment set, so whichever lights lose
    // the budget leak. The order decides who leaks:
    //
    // 1. The headlight. It is the light the player is looking along, and the
    //    one whose shadows move with every turn. It used to be claimed after
    //    the level lights, so in any room with eight visible level lights it
    //    got no slot at all and lit through everything - which read as light
    //    leaks and was why a budget of 32 always looked better than 8.
    // 2. The level's own lights that the portal walk can see, nearest to the
    //    camera first. An off-screen light casting into nothing is the
    //    cheapest thing to give up, and a distant one is the next cheapest.
    // 3. Projectile, flare and explosion lights, nearest first - a bolt across
    //    the room contributes a shadow nobody can resolve.
    castsShadow_.assign(active_.size(), 0);
    stats_.shadowBudget = shadowBudget;

    int used = 0;
    const auto claim = [&](size_t index) {
        if (used >= shadowBudget) return false;
        if (!active_[index]->castsShadows) return false;
        castsShadow_[index] = 1;
        ++used;
        return true;
    };

    if (headlightIndex_ >= 0) claim(static_cast<size_t>(headlightIndex_));

    const size_t staticCount = level.lights.size();
    shadowOrder_.clear();
    for (size_t i = 0; i < staticCount && i < active_.size(); ++i) {
        bool visible = false;
        for (int segment : active_[i]->segments) {
            if (walk.isVisible(segment)) {
                visible = true;
                break;
            }
        }
        if (!visible) continue;
        shadowOrder_.emplace_back(glm::length(active_[i]->position - eye), static_cast<int>(i));
    }
    std::sort(shadowOrder_.begin(), shadowOrder_.end());
    for (const auto& [distance, index] : shadowOrder_) {
        (void)distance;
        claim(static_cast<size_t>(index));
    }

    shadowOrder_.clear();
    for (size_t i = staticCount; i < active_.size(); ++i) {
        if (static_cast<int>(i) == headlightIndex_) continue;
        shadowOrder_.emplace_back(glm::length(active_[i]->position - eye), static_cast<int>(i));
    }
    std::sort(shadowOrder_.begin(), shadowOrder_.end());
    for (const auto& [distance, index] : shadowOrder_) {
        (void)distance;
        claim(static_cast<size_t>(index));
    }

    stats_.shadowLights = used;
    stats_.budgetDenied = 0;
    for (size_t i = 0; i < active_.size(); ++i) {
        if (!castsShadow_[i] && active_[i]->castsShadows) ++stats_.budgetDenied;
    }
}

// Robots move, so their occluders are re-transformed every frame. The meshes
// are tens of triangles, so this is cheaper than any scheme for avoiding it.
void Renderer::updateRobotOccluders(const RenderState& rs) {
    for (int i = 0; i < rs.robotCount; ++i) {
        const RenderState::RobotView& view = rs.robots[static_cast<size_t>(i)];
        const auto slot = static_cast<size_t>(view.slot);
        if (slot >= robotOccluders_.size()) continue;
        if (view.type < 0 || view.type >= static_cast<int>(robotTypes_.size())) continue;

        const glm::mat4 model = glm::translate(glm::mat4(1.0f), view.position) *
                                glm::mat4_cast(view.orientation);
        if (robotOccluderType_[slot] != view.type) {
            robotOccluders_[slot].reset(robotTypes_[static_cast<size_t>(view.type)].occluder,
                                        model);
            robotOccluderType_[slot] = view.type;
        } else {
            robotOccluders_[slot].setTransform(model);
        }
    }
}

namespace {

// A door slab is oversized by this much at the frame and at the top and bottom,
// so its outer faces sit inside the rock rather than exactly in the plane of the
// adjacent wall. Exactly coplanar faces from two different meshes z-fight in the
// depth prepass and then disagree in every light pass, which showed as a fizzing
// strip around the door frame.
constexpr float kSlabMargin = 0.25f;

// The unit cube is scaled to 0.9 across the doorway's normal, so a slab's face
// stands this far out from the plane of the opening.
constexpr float kSlabHalfThickness = 0.45f;

// The opening's own frame, derived from its four corners rather than from the
// segment, so a deformed doorway still gets a slab and a sign that fit it.
struct DoorFrame {
    glm::vec3 across{1.0f, 0.0f, 0.0f};   // unit, left to right across the opening
    glm::vec3 up{0.0f, 1.0f, 0.0f};       // unit
    glm::vec3 normal{0.0f, 0.0f, 1.0f};   // unit, out of the doorway
    float width = 0.0f;                   // the whole opening, not a half
    float height = 0.0f;
    // Half-extent of one slab across the opening, and how far that slab travels
    // to end up entirely buried in the wall.
    float halfWidth = 0.0f;
    float travel = 0.0f;
    bool valid = false;
};

DoorFrame doorFrame(const Door& door) {
    DoorFrame frame;
    const glm::vec3 acrossRaw = (door.corners[1] - door.corners[0]) +
                                (door.corners[2] - door.corners[3]);
    const glm::vec3 upRaw = (door.corners[3] - door.corners[0]) +
                            (door.corners[2] - door.corners[1]);
    frame.width = glm::length(acrossRaw) * 0.5f;
    frame.height = glm::length(upRaw) * 0.5f;
    if (frame.width < 1e-3f || frame.height < 1e-3f) return frame;

    frame.across = acrossRaw / (frame.width * 2.0f);
    frame.up = upRaw / (frame.height * 2.0f);
    frame.normal = glm::normalize(glm::cross(frame.across, frame.up));
    frame.halfWidth = frame.width * 0.25f + kSlabMargin * 0.5f;
    frame.travel = frame.width * 0.5f + kSlabMargin;
    frame.valid = true;
    return frame;
}

// How far half `half` of a door has slid from where it sits when shut.
float doorSlide(const DoorFrame& frame, int half, float openness) {
    const float side = half == 0 ? -1.0f : 1.0f;
    return side * openness * frame.travel;
}

}   // namespace

// Places the two halves of every door. A door slides apart across the opening,
// so the halves stay inside the doorway's own plane and end up buried in the
// wall to either side rather than poking through the ceiling.
//
// Each half is oversized by kSlabMargin; see the constant for why. The halves
// still meet exactly at the centre seam: those two faces face away from each
// other, so only one is ever drawn.
//
// The slabs are always drawn and always cast, whatever the openness. Fully
// open they are buried in the wall, where the depth test hides them and
// their volumes fall inside the mine's own shadow; while opening they are the
// only thing occluding the doorway, since the portal walk and the light flood
// see through a door from the moment it cracks open.
void Renderer::buildDoors(const RenderState& rs, const Level& level) {
    const size_t count = std::min(level.doors.size(), static_cast<size_t>(sim::kMaxDoors));
    for (size_t di = 0; di < count; ++di) {
        const Door& door = level.doors[di];
        const DoorFrame frame = doorFrame(door);
        if (!frame.valid) continue;
        const float openness =
            di < static_cast<size_t>(rs.doorCount) ? rs.doorOpenness[di] : 0.0f;

        glm::mat4 basis(1.0f);
        basis[0] = glm::vec4(frame.across, 0.0f);
        basis[1] = glm::vec4(frame.up, 0.0f);
        basis[2] = glm::vec4(frame.normal, 0.0f);

        for (int half = 0; half < 2; ++half) {
            const float side = half == 0 ? -1.0f : 1.0f;
            const float offset = side * frame.halfWidth + doorSlide(frame, half, openness);
            const glm::vec3 centre = door.center + frame.across * offset;

            glm::mat4 model = glm::translate(glm::mat4(1.0f), centre) * basis;
            model = glm::scale(model, glm::vec3(frame.halfWidth * 2.0f,
                                                frame.height + kSlabMargin * 2.0f, 0.9f));

            DoorSlab& slab = doorSlabs_[di * 2 + static_cast<size_t>(half)];
            slab.model = model;
            slab.segmentA = door.segment;
            slab.segmentB = door.neighbour;
            doorOccluders_[di * 2 + static_cast<size_t>(half)].setTransform(model);
        }
    }
    for (size_t di = count; di < static_cast<size_t>(sim::kMaxDoors); ++di) {
        doorSlabs_[di * 2].segmentA = -1;
        doorSlabs_[di * 2].segmentB = -1;
        doorSlabs_[di * 2 + 1].segmentA = -1;
        doorSlabs_[di * 2 + 1].segmentB = -1;
    }
}

namespace {

// Where the signage sits on a doorway, as fractions of the opening, and what it
// says. Two plates: whether the door will open, and which key it wants.
constexpr float kSignHalfWidth = 0.42f;      // of the opening's width
constexpr float kSignHeight = 0.20f;         // of its height, above the centre
constexpr float kStatusHalfHeight = 0.075f;
constexpr float kKeyHalfHeight = 0.055f;
constexpr float kSignGap = 0.012f;
// Clear of the slab face, so the sign is never coplanar with what it is stuck to.
constexpr float kSignStandoff = 0.02f;

constexpr glm::vec4 kDeniedPlate{0.72f, 0.05f, 0.09f, 1.0f};
constexpr glm::vec4 kGrantedPlate{0.09f, 0.66f, 0.20f, 1.0f};
// Not pure black: the lettering is stamped into a lit plate, not cut out of it.
constexpr glm::vec4 kInk{0.05f, 0.04f, 0.04f, 1.0f};

struct KeyStyle {
    glm::vec4 plate;
    const char* text;
};

KeyStyle keyStyle(KeyColor key) {
    switch (key) {
        case KeyColor::Blue:   return {{0.16f, 0.45f, 0.95f, 1.0f}, "BLUE KEY REQUIRED"};
        case KeyColor::Yellow: return {{0.95f, 0.80f, 0.12f, 1.0f}, "YELLOW KEY REQUIRED"};
        case KeyColor::Red:    return {{0.90f, 0.16f, 0.14f, 1.0f}, "RED KEY REQUIRED"};
        default:               return {{0.6f, 0.6f, 0.6f, 1.0f}, ""};
    }
}

}   // namespace

// The signage on the keyed doors: a strip saying whether the door in front of
// the player is going to open, and under it one in the key's own colour saying
// which key it wants. Without them a locked door is a striped panel that looks
// exactly like a door the player can already open, and the only feedback for
// the difference is a refusal noise after flying into it.
//
// Both faces of both halves, so the sign is there whichever side the player
// arrives from and parts with the door as it opens. The lettering is laid out
// across the whole doorway and cut at the seam rather than per half, so a letter
// that straddles the join is split down the middle instead of being drawn twice.
void Renderer::buildDoorSigns(const RenderState& rs, const Level& level, const PortalWalk& walk) {
    doorSigns_.clear();

    const size_t count = std::min(level.doors.size(), static_cast<size_t>(sim::kMaxDoors));
    for (size_t di = 0; di < count; ++di) {
        const Door& door = level.doors[di];
        const int key = static_cast<int>(door.key);
        if (key < 0 || key >= static_cast<int>(rs.keys.size())) continue;
        if (!walk.isVisible(door.segment) && !walk.isVisible(door.neighbour)) continue;

        const DoorFrame frame = doorFrame(door);
        if (!frame.valid) continue;
        const float openness =
            di < static_cast<size_t>(rs.doorCount) ? rs.doorOpenness[di] : 0.0f;

        const bool have = rs.keys[static_cast<size_t>(key)];
        const KeyStyle style = keyStyle(door.key);

        const float halfWidth = frame.width * kSignHalfWidth;
        const float statusHalfH = frame.height * kStatusHalfHeight;
        const float keyHalfH = frame.height * kKeyHalfHeight;
        const float statusY = frame.height * kSignHeight;
        const float keyY = statusY - statusHalfH - frame.height * kSignGap - keyHalfH;

        for (int half = 0; half < 2; ++half) {
            // The sign is glued to its slab, so it rides the same slide. Its
            // own centre stays over the middle of the opening while the door is
            // shut, which is what puts the seam at u = 0.5.
            const glm::vec3 slid =
                door.center + frame.across * doorSlide(frame, half, openness);

            for (int face = 0; face < 2; ++face) {
                const float facing = face == 0 ? 1.0f : -1.0f;
                const glm::vec3 offset =
                    frame.normal * (facing * (kSlabHalfThickness + kSignStandoff));

                DoorSigns::Panel panel;
                panel.right = frame.across * facing;
                panel.up = frame.up;
                panel.halfWidth = halfWidth;
                // Seen from behind, the door's left-hand half is on the
                // reader's right, so the slice this half carries swaps over.
                const bool firstSlice = (half == 0) == (facing > 0.0f);
                panel.uMin = firstSlice ? 0.0f : 0.5f;
                panel.uMax = firstSlice ? 0.5f : 1.0f;

                panel.halfHeight = statusHalfH;
                panel.center = slid + offset + frame.up * statusY;
                doorSigns_.add(panel, have ? "ACCESS GRANTED" : "ACCESS DENIED",
                               have ? kGrantedPlate : kDeniedPlate, kInk);

                panel.halfHeight = keyHalfH;
                panel.center = slid + offset + frame.up * keyY;
                doorSigns_.add(panel, style.text, style.plate, kInk);
            }
        }
    }

    doorSigns_.upload();
}

void Renderer::buildHud(const HudData& hud, const glm::mat4& viewProj) {
    // Four bands per edge with falling alpha, because Hud::rect is a flat
    // colour and this is cheaper and more predictable than a gradient shader
    // for something on screen for a third of a second.
    const auto damageTint = [&]() {
        // A disabled ship holds the vignette on rather than letting it fade:
        // the flinch is an event, being disabled is a state, and the player
        // needs to be able to tell at a glance which one they are in.
        float flash = hud.damageFlash;
        if (hud.shipDisabled) {
            flash = std::max(flash,
                             0.55f + 0.45f * std::sin(hud.shipDisabledTime * 5.5f));
        }
        if (flash <= 0.0f) return;
        const float w = static_cast<float>(target_.width());
        const float h = static_cast<float>(target_.height());
        const float depth = h * 0.16f;
        // Eight rather than four: at four the "gradient" reads as four
        // rectangles, which looks like a bug rather than a flinch.
        constexpr int kBands = 8;
        for (int b = 0; b < kBands; ++b) {
            const float t = static_cast<float>(b) / kBands;
            const float band = depth * (1.0f - t);
            const glm::vec4 red(0.9f, 0.12f, 0.1f, flash * 0.055f * (1.0f - t * 0.5f));
            hud_.rect(0.0f, 0.0f, w, band, red);
            hud_.rect(0.0f, h - band, w, band, red);
            hud_.rect(0.0f, 0.0f, band, h, red);
            hud_.rect(w - band, 0.0f, band, h, red);
        }
    };

    if (!hud.show) {
        hud_.begin(target_.width(), target_.height());
        damageTint();
        return;
    }

    const float w = static_cast<float>(target_.width());
    const float h = static_cast<float>(target_.height());
    // One scale for the whole HUD, tied to height, so it reads the same on any
    // window size instead of shrinking to nothing at 4K.
    const float s = std::max(1.5f, h / 260.0f);
    hud_.begin(target_.width(), target_.height());
    damageTint();

    const glm::vec4 white(1.0f, 1.0f, 1.0f, 0.92f);
    const glm::vec4 dim(0.75f, 0.8f, 0.9f, 0.8f);

    const float cx = w * 0.5f;
    const float cy = h * 0.5f;

    // Incoming transmission, below the key row in the otherwise open upper
    // right. The authored frames loop for the duration of the message; the
    // short fades keep it from arriving and vanishing as a hard rectangle.
    if (hud.commActive && hud.commTime < commDisplaySeconds_ && commFrameCount_ > 0) {
        const float fadeIn = std::clamp(hud.commTime / 0.18f, 0.0f, 1.0f);
        const float fadeOut = std::clamp((commDisplaySeconds_ - hud.commTime) / 0.45f,
                                         0.0f, 1.0f);
        const float alpha = std::min(fadeIn, fadeOut);
        const float panelW = std::min(w * 0.28f, 92.0f * s);
        const float panelH = panelW * static_cast<float>(commHeight_) /
                             static_cast<float>(commWidth_);
        const float x = w - 8.0f * s - panelW;
        const float y = 20.0f * s;
        const float frame = 1.5f * s;
        const glm::vec4 cyan(0.35f, 0.92f, 1.0f, alpha);
        hud_.rect(x - frame, y - frame, panelW + frame * 2.0f,
                  panelH + frame * 2.0f, glm::vec4(0.02f, 0.22f, 0.28f, alpha * 0.9f));
        const int layer = static_cast<int>(std::floor(hud.commTime * commFramesPerSecond_)) %
                          commFrameCount_;
        hud_.image(x, y, panelW, panelH, layer, glm::vec4(1.0f, 1.0f, 1.0f, alpha));
        hud_.rect(x - frame, y - frame, panelW + frame * 2.0f, frame, cyan);
        hud_.text("incoming transmission", x, y + panelH + 3.0f * s, s * 0.62f, cyan);
    }

    // Reticle: a gap in the middle so it frames the target rather than
    // covering it.
    //
    // It goes over the aim point, not over the centre of the window. Those
    // coincide in first person and do not in the chase camera, which watches
    // the ship from above and behind; see HudData::aimPoint. The projection is
    // the same matrix the scene is drawn with, so the reticle lands on the same
    // pixel as the thing it marks. Everything else centred below - the
    // countdown, the end-of-level card - stays on the window, because those are
    // screen furniture and not part of the world.
    float rx = cx;
    float ry = cy;
    if (hud.hasAimPoint) {
        const glm::vec4 clip = viewProj * glm::vec4(hud.aimPoint, 1.0f);
        // w is the distance in front of the camera under this projection. At or
        // behind the eye there is no sane pixel to draw on, so fall back to the
        // centre rather than mirroring the reticle to the wrong side.
        if (clip.w > 1e-3f) {
            rx = (clip.x / clip.w * 0.5f + 0.5f) * w;
            ry = (0.5f - clip.y / clip.w * 0.5f) * h;
        }
    }
    const float arm = 5.0f * s;
    const float gap = 3.0f * s;
    const float thin = std::max(1.0f, s * 0.6f);
    const glm::vec4 reticle(0.6f, 1.0f, 0.7f, 0.75f);
    hud_.rect(rx - gap - arm, ry - thin * 0.5f, arm, thin, reticle);
    hud_.rect(rx + gap, ry - thin * 0.5f, arm, thin, reticle);
    hud_.rect(rx - thin * 0.5f, ry - gap - arm, thin, arm, reticle);
    hud_.rect(rx - thin * 0.5f, ry + gap, thin, arm, reticle);

    // Shields and energy, bottom left.
    const float barW = 44.0f * s;
    const float barH = 4.0f * s;
    const float left = 8.0f * s;
    float y = h - 8.0f * s - barH;
    hud_.text("energy", left, y - 7.0f * s - 2.0f * s, s * 0.7f, dim);
    hud_.bar(left, y, barW, barH, hud.energy / 100.0f, glm::vec4(0.3f, 0.75f, 1.0f, 0.9f));
    y -= barH + 12.0f * s;
    hud_.text("shields", left, y - 7.0f * s - 2.0f * s, s * 0.7f, dim);
    hud_.bar(left, y, barW, barH, hud.shields / 100.0f, glm::vec4(0.4f, 1.0f, 0.55f, 0.9f));

    // Darkness, as a third gauge on the same stack. It belongs with energy and
    // shields rather than off on its own: the mine has no ambient term, so the
    // light on the player is a resource they spend and recover by flying,
    // exactly as those two are.
    //
    // Three parts, because one number cannot say it. The bar is the light
    // reaching the player. The tick inside it is the threshold that light has to
    // stay under to be hidden, drawn in the bar so the player can see how close
    // to the edge they are flying rather than only which side of it they are
    // on. The word is the robots' own conclusion, which is the thing that
    // actually matters and which the bar alone would get wrong: with the
    // headlight on the bar can sit deep in the dark and the answer still be
    // "seen".
    y -= barH + 12.0f * s;
    {
        // Scaled so the threshold sits a quarter of the way along. The range
        // worth reading is the one either side of it; the rest of the bar only
        // has to say "not close".
        const float full = std::max(hud.darkThreshold, 1e-4f) * 4.0f;
        const glm::vec4 hidden(0.35f, 0.5f, 0.95f, 0.9f);
        const glm::vec4 exposed(1.0f, 0.8f, 0.3f, 0.9f);
        const glm::vec4 c = hud.seen ? exposed : hidden;

        // The state word stands where "energy" and "shields" put their captions,
        // and is the caption: a fixed noun plus the word would not fit across
        // the bar at this scale, and of the two the answer is worth more than
        // the name of the thing being measured. The headlight case names itself
        // rather than reading "exposed", because the player can do something
        // about that one and the key that does it is one press away.
        const char* word = !hud.seen ? "hidden" : (hud.seenByHeadlight ? "headlight" : "exposed");
        hud_.text(word, left, y - 7.0f * s - 2.0f * s, s * 0.7f, c);
        hud_.bar(left, y, barW, barH, hud.illumination / full, c);

        // Drawn after the bar so it sits on top of the fill, and taller than the
        // bar so it stays findable once the fill has passed it.
        const float tickW = std::max(1.0f, s * 0.5f);
        hud_.rect(left + barW * (hud.darkThreshold / full) - tickW * 0.5f, y - 1.5f * s, tickW,
                  barH + 3.0f * s, glm::vec4(0.85f, 0.9f, 1.0f, 0.65f));
    }

    // Primary and secondary loadout, bottom right.
    {
        std::string primary = "primary  ";
        primary += hud.primaryWeaponName;
        if (hud.primaryLaserLevel > 0 && std::string_view(hud.primaryWeaponName) == "laser") {
            primary += " " + std::to_string(hud.primaryLaserLevel);
        }
        if (hud.primaryAmmo >= 0) primary += "  " + std::to_string(hud.primaryAmmo);
        const float primaryW = hud_.textWidth(primary, s * 0.8f);
        hud_.text(primary, w - left - primaryW, h - 8.0f * s - 7.0f * s, s * 0.8f, white);

        std::string secondary = "secondary  ";
        secondary += hud.secondaryWeaponName;
        if (hud.secondaryAmmo >= 0) secondary += "  " + std::to_string(hud.secondaryAmmo);
        const float secondaryW = hud_.textWidth(secondary, s * 0.8f);
        hud_.text(secondary, w - left - secondaryW, h - 8.0f * s - 17.0f * s,
                  s * 0.8f, white);

        const std::string flares = "flares " + std::to_string(hud.flares);
        const float fw = hud_.textWidth(flares, s * 0.7f);
        hud_.text(flares, w - left - fw, h - 8.0f * s - 27.0f * s, s * 0.7f, dim);
    }

    // Keys, top right: lit when held, a dark outline when not, so their
    // absence is as readable as their presence.
    {
        const glm::vec4 colors[3] = {{0.35f, 0.55f, 1.0f, 1.0f},
                                     {1.0f, 0.9f, 0.3f, 1.0f},
                                     {1.0f, 0.35f, 0.3f, 1.0f}};
        const float size = 6.0f * s;
        for (int k = 0; k < 3; ++k) {
            const float x = w - left - (3 - k) * (size + 3.0f * s);
            const glm::vec4 c = hud.keys[k] ? colors[k]
                                            : glm::vec4(colors[k].r * 0.18f, colors[k].g * 0.18f,
                                                        colors[k].b * 0.18f, 0.75f);
            hud_.rect(x, left, size, size, c);
        }
    }

    // The countdown, large and centred, because it is the only number that
    // matters once it is running.
    if (hud.countdown >= 0.0f) {
        const int seconds = static_cast<int>(hud.countdown);
        const int hundredths = static_cast<int>((hud.countdown - static_cast<float>(seconds)) * 100.0f);
        std::string text = std::to_string(seconds) + ":" +
                           (hundredths < 10 ? "0" : "") + std::to_string(hundredths);
        const float big = s * 3.2f;
        const float tw = hud_.textWidth(text, big);
        // Red and pulsing under ten seconds.
        const float urgency = hud.countdown < 10.0f ? 1.0f : 0.0f;
        const glm::vec4 c(1.0f, 0.85f - 0.6f * urgency, 0.4f - 0.35f * urgency, 0.95f);
        hud_.text(text, cx - tw * 0.5f, 14.0f * s, big, c);
        const std::string label = "get out";
        hud_.text(label, cx - hud_.textWidth(label, s) * 0.5f, 14.0f * s + big * 8.0f, s, c);
    }

    if (hud.phase >= 2) {
        const std::string title = hud.phase == 2 ? "mine destroyed" : "you are dead";
        const float big = s * 2.6f;
        hud_.text(title, cx - hud_.textWidth(title, big) * 0.5f, cy - big * 10.0f, big,
                  hud.phase == 2 ? glm::vec4(0.6f, 1.0f, 0.7f, 1.0f)
                                 : glm::vec4(1.0f, 0.45f, 0.35f, 1.0f));

        const std::string reason = hud.message;
        hud_.text(reason, cx - hud_.textWidth(reason, s) * 0.5f, cy - big * 3.0f, s, dim);

        const std::string stats = "time " + std::to_string(static_cast<int>(hud.elapsed)) +
                                  "s   robots " + std::to_string(hud.robotsDestroyed) + "/" +
                                  std::to_string(hud.robotsTotal);
        hud_.text(stats, cx - hud_.textWidth(stats, s) * 0.5f, cy + big, s, white);

        const std::string again = "press any key";
        hud_.text(again, cx - hud_.textWidth(again, s) * 0.5f, cy + big * 4.0f, s, dim);
    }

}

void Renderer::buildMenu(const MenuView& menu) {
    if (menu.screen == MenuScreen::Hidden) return;

    const float width = static_cast<float>(target_.width());
    const float height = static_cast<float>(target_.height());
    // Menu panels stop growing once they are comfortably readable. Keep the
    // bitmap font under the same kind of ceiling: scaling it from height alone
    // made 1440p labels wider than their fixed-width rows.
    const float scale = std::clamp(height / 260.0f, 1.5f, 4.0f);
    const glm::vec4 white(0.92f, 0.96f, 1.0f, 1.0f);
    const glm::vec4 dim(0.58f, 0.66f, 0.75f, 0.95f);
    const glm::vec4 accent(0.35f, 1.0f, 0.62f, 0.95f);

    hud_.rect(0.0f, 0.0f, width, height, glm::vec4(0.0f, 0.015f, 0.025f, 0.78f));

    const float panelWidth = std::clamp(width * 0.48f, 360.0f, 680.0f);
    const float panelLeft = (width - panelWidth) * 0.5f;
    const float panelTop = height * 0.12f;
    const float panelHeight = height * 0.78f;
    hud_.rect(panelLeft, panelTop, panelWidth, panelHeight,
              glm::vec4(0.025f, 0.045f, 0.06f, 0.96f));
    hud_.rect(panelLeft, panelTop, panelWidth, std::max(2.0f, scale * 0.8f), accent);

    const float titleScale = scale * 2.4f;
    const float titleWidth = hud_.textWidth(menu.title, titleScale);
    hud_.text(menu.title, (width - titleWidth) * 0.5f, height * 0.20f, titleScale, white);
    // A level's own blurb is the subtitle of the level list, and a blurb is
    // written to be read, not to fit a panel. Cut it to what fits - at a word
    // boundary, with an ellipsis - rather than letting it run off both edges.
    // No std::string: this runs every frame the menu is up, and the draw path
    // is one of the two places this project does not allocate in.
    const float subtitleScale = scale * 0.75f;
    const float subtitleRoom = panelWidth * 0.92f;
    const float glyphWidth = std::max(hud_.textWidth(" ", subtitleScale), 1.0f);
    std::string_view subtitle(menu.subtitle);
    const char* kEllipsis = "...";
    bool elided = false;
    if (hud_.textWidth(subtitle, subtitleScale) > subtitleRoom) {
        const size_t room = static_cast<size_t>(subtitleRoom / glyphWidth);
        subtitle = subtitle.substr(0, room > 4 ? room - 3 : 1);
        const size_t space = subtitle.find_last_of(' ');
        // Only back up to a space that is near the cut; a subtitle with one
        // very long word would otherwise lose most of itself.
        if (space != std::string_view::npos && space * 4 > subtitle.size() * 3) {
            subtitle = subtitle.substr(0, space);
        }
        elided = true;
    }
    const float subtitleWidth =
        hud_.textWidth(subtitle, subtitleScale) +
        (elided ? hud_.textWidth(kEllipsis, subtitleScale) : 0.0f);
    const float subtitleLeft = (width - subtitleWidth) * 0.5f;
    const float subtitleY = height * (menu.screen == MenuScreen::Settings ? 0.27f : 0.30f);
    hud_.text(subtitle, subtitleLeft, subtitleY, subtitleScale, dim);
    if (elided) {
        hud_.text(kEllipsis, subtitleLeft + hud_.textWidth(subtitle, subtitleScale),
                  subtitleY, subtitleScale, dim);
    }

    if (menu.screen == MenuScreen::Summary) {
        const float reasonWidth = hud_.textWidth(menu.reason, scale);
        hud_.text(menu.reason, (width - reasonWidth) * 0.5f, height * 0.39f, scale, dim);
        char stats[96];
        std::snprintf(stats, sizeof(stats), "time %ds   robots %d/%d", menu.elapsedSeconds,
                      menu.robotsDestroyed, menu.robotsTotal);
        const float statsWidth = hud_.textWidth(stats, scale);
        hud_.text(stats, (width - statsWidth) * 0.5f, height * 0.45f, scale, white);
        if (menu.recordNote[0] != '\0') {
            const float noteWidth = hud_.textWidth(menu.recordNote, scale);
            hud_.text(menu.recordNote, (width - noteWidth) * 0.5f, height * 0.51f, scale,
                      accent);
        }
    }

    if (menu.screen == MenuScreen::DisplayConfirmation) {
        char countdown[64];
        std::snprintf(countdown, sizeof(countdown), "reverting in %d seconds",
                      menu.confirmationSeconds);
        const float countdownWidth = hud_.textWidth(countdown, scale);
        hud_.text(countdown, (width - countdownWidth) * 0.5f, height * 0.39f, scale, dim);
    }

    // Only when the list is longer than the rows on screen: which slice of it
    // this is. A list that fits says nothing, because there is nothing to say.
    if (menu.screen == MenuScreen::LevelSelect && menu.levelsShown > 0 &&
        menu.levelCount > menu.levelsShown) {
        char range[64];
        std::snprintf(range, sizeof(range), "%d-%d of %d", menu.firstLevelShown + 1,
                      menu.firstLevelShown + menu.levelsShown, menu.levelCount);
        const float rangeScale = scale * 0.7f;
        hud_.text(range, (width - hud_.textWidth(range, rangeScale)) * 0.5f, height * 0.335f,
                  rangeScale, dim);
    }

    for (int i = 0; i < menu.buttonCount; ++i) {
        const MenuButtonView& button = menu.buttons[static_cast<size_t>(i)];
        const glm::vec4 fill = button.selected ? glm::vec4(0.08f, 0.28f, 0.20f, 0.98f)
                                               : glm::vec4(0.04f, 0.08f, 0.11f, 0.96f);
        const glm::vec4 text = button.selected ? accent : white;
        hud_.rect(button.bounds.x, button.bounds.y, button.bounds.width, button.bounds.height,
                  fill);
        if (button.selected) {
            hud_.rect(button.bounds.x, button.bounds.y, std::max(3.0f, scale),
                      button.bounds.height, accent);
        }
        const float labelScale = scale * 0.9f;
        const bool hasValue = button.value[0] != '\0';
        const float labelWidth = hud_.textWidth(button.label, labelScale);
        const float labelX = hasValue ? button.bounds.x + button.bounds.width * 0.06f
                                      : button.bounds.x + (button.bounds.width - labelWidth) * 0.5f;
        const float labelY = button.bounds.y + (button.bounds.height - 7.0f * labelScale) * 0.5f;
        hud_.text(button.label, labelX, labelY, labelScale, text);
        if (hasValue) {
            const float valueWidth = hud_.textWidth(button.value.data(), labelScale);
            const float valueX = button.bounds.x + button.bounds.width * 0.94f - valueWidth;
            hud_.text(button.value.data(), valueX, labelY, labelScale, text);
        }
    }

    if (menu.screen == MenuScreen::Controls && menu.controlsShown > 0 &&
        menu.controlCount > menu.controlsShown) {
        char range[64];
        std::snprintf(range, sizeof(range), "%d-%d of %d", menu.firstControlShown + 1,
                      menu.firstControlShown + menu.controlsShown, menu.controlCount);
        const float rangeScale = scale * 0.7f;
        hud_.text(range, (width - hud_.textWidth(range, rangeScale)) * 0.5f, height * 0.335f,
                  rangeScale, dim);
    }
}

void Renderer::hudPass() {
    if (hud_.quadCount() == 0) return;
    // Drawn on the resolved framebuffer, after the blit, for the same reason
    // ImGui is: it is axis-aligned and gains nothing from multisampling.
    hudShader_->bind();
    if (commFrames_) commFrames_->bind(3);
    hudShader_->setVec2("uScreen", glm::vec2(static_cast<float>(target_.width()),
                                             static_cast<float>(target_.height())));
    glDisable(GL_DEPTH_TEST);
    // Culling off: the shader flips y to put the HUD origin at the top-left,
    // which reverses the winding of every quad. Leaving it on culls the entire
    // HUD and produces exactly nothing, with no error anywhere.
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    hud_.draw();
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
}

// The red a disabled ship shows, pulsing. One function so the robot's albedo,
// the robot's eyes and the player's own hull all pulse together.
namespace {
glm::vec3 disabledTint(float seconds) {
    const float pulse = 0.5f + 0.5f * std::sin(seconds * 5.5f);
    return glm::mix(glm::vec3(0.55f, 0.16f, 0.13f), glm::vec3(1.0f, 0.28f, 0.20f), pulse);
}
}   // namespace

void Renderer::buildBillboards(const RenderState& rs) {
    billboards_.clear();

    // The camera's right and up, read straight out of the view matrix, so every
    // quad faces the viewer without per-sprite work.
    const glm::mat4 view = buildView(rs);
    const glm::vec3 right{view[0][0], view[1][0], view[2][0]};
    const glm::vec3 up{view[0][1], view[1][1], view[2][1]};

    for (int i = 0; i < rs.boltCount; ++i) {
        const RenderState::Bolt& b = rs.bolts[static_cast<size_t>(i)];
        billboards_.add(b.position, right, up, b.size, b.color);
    }
    for (int i = 0; i < rs.flashCount; ++i) {
        const RenderState::Flash& f = rs.flashes[static_cast<size_t>(i)];
        billboards_.add(f.position, right, up, f.size, f.color * f.fade);
    }

    // Floating balls and swinging lamps. Emissive, because that is what they
    // are: the light is the object. The cord a lamp hangs from is a very thin
    // beam, which is all a cord is.
    const glm::vec3 eye = eyePos(rs);
    for (int i = 0; i < rs.propCount; ++i) {
        const RenderState::PropView& prop = rs.props[static_cast<size_t>(i)];
        billboards_.add(prop.position, right, up, prop.size, prop.color);
        if (prop.corded) {
            billboards_.addBeam(prop.anchor, prop.position, eye, right, 0.05f,
                                prop.color * 0.28f);
        }
    }

    // Hitscan tracers, into the same buffer as the sprites so the unlit pass
    // keeps to one draw.
    for (int i = 0; i < rs.beamCount; ++i) {
        const RenderState::BeamView& beam = rs.beams[static_cast<size_t>(i)];
        glm::vec3 visualStart = beam.start;
        if (rs.camera == CameraMode::FirstPerson) {
            // The trace leaves the ship's centreline so it hits the crosshair.
            // Drawing from there looks straight down the ribbon and collapses
            // its length. Offset only the visible muzzle; the hit point and all
            // gameplay remain on the centreline.
            visualStart = rs.shipPos + right * 0.34f - up * 0.22f +
                          glm::mat3_cast(rs.shipOrientation) * glm::vec3(0.0f, 0.0f, -0.65f);
        }
        billboards_.addBeam(visualStart, beam.end, eye, right, beam.width, beam.color);
    }

    // Pickups. Emissive so they can be found in a dark corridor, and pulsing
    // so they are not mistaken for a wall detail.
    for (int i = 0; i < rs.pickupCount; ++i) {
        const RenderState::PickupView& pickup = rs.pickups[static_cast<size_t>(i)];
        if (!pickup.present) continue;
        if (pickup.sprite >= 0) {
            billboards_.addSprite(pickup.position, right, up, pickup.size, pickup.color,
                                  pickup.sprite);
        } else {
            billboards_.add(pickup.position, right, up, pickup.size, pickup.color);
        }
    }

    // Robot eyes. Emissive, so they read in total darkness - which matters
    // more than it sounds in a mine with no ambient light, because the eyes
    // are usually the first thing the player sees of a robot.
    if (robotConfig_) {
        for (int i = 0; i < rs.robotCount; ++i) {
            const RenderState::RobotView& robot = rs.robots[static_cast<size_t>(i)];
            if (robot.type < 0 || robot.type >= static_cast<int>(robotConfig_->types.size())) {
                continue;
            }
            const sim::RobotDef& def = robotConfig_->types[static_cast<size_t>(robot.type)];
            const glm::mat3 basis = glm::mat3_cast(robot.orientation);

            // A disabled robot's eyes go red and dim. This is the cue that
            // matters: the albedo tint below only shows where light already
            // falls, and there is no ambient term, so a robot drifting in an
            // unlit corridor would otherwise give no sign at all.
            glm::vec3 eyeColor = def.eyeColor;
            float eyeSize = def.eyeSize;
            if (robot.disabledTime > 0.0f) {
                eyeColor = disabledTint(robot.disabledTime) * 0.8f;
                eyeSize = def.eyeSize * 0.7f;
            }
            for (const glm::vec3& eye : def.eyes) {
                billboards_.add(robot.position + basis * eye, right, up, eyeSize, eyeColor);
            }
        }
    }

    billboards_.upload();
    stats_.billboards = billboards_.count();
}

void Renderer::buildDebugLines(const Level& level, const RenderState& rs,
                               const sim::ContactLog& contacts, const sim::RayHit& noseRay) {
    debugLines_.clear();

    if (drawWallNormals && rs.shipSegment >= 0 && rs.shipSegment < level.segmentCount()) {
        const int first = level.wallFirst[static_cast<size_t>(rs.shipSegment)];
        const int count = level.wallCount[static_cast<size_t>(rs.shipSegment)];
        for (int wi = first; wi < first + count; ++wi) {
            const WallTriangle& w = level.walls[static_cast<size_t>(wi)];
            const glm::vec3 centre = (w.p[0] + w.p[1] + w.p[2]) / 3.0f;
            debugLines_.line(centre, centre + w.normal * 2.5f, glm::vec3(0.2f, 0.8f, 1.0f));
        }
    }

    if (drawContacts) {
        for (int i = 0; i < contacts.count; ++i) {
            const sim::Contact& c = contacts.contacts[static_cast<size_t>(i)];
            debugLines_.cross(c.point, 0.5f, glm::vec3(1.0f, 0.25f, 0.15f));
            debugLines_.line(c.point, c.point + c.normal * 2.0f, glm::vec3(1.0f, 0.6f, 0.1f));
        }
    }

    if (drawNoseRay) {
        const glm::vec3 forward = glm::mat3_cast(rs.shipOrientation) * glm::vec3(0.0f, 0.0f, -1.0f);
        const glm::vec3 origin = rs.shipPos;
        const glm::vec3 end = noseRay.hit ? noseRay.point : origin + forward * 120.0f;
        debugLines_.line(origin + forward * 2.0f, end, glm::vec3(0.3f, 1.0f, 0.4f));
        if (noseRay.hit) {
            debugLines_.cross(noseRay.point, 0.8f, glm::vec3(1.0f, 1.0f, 0.2f));
            debugLines_.line(noseRay.point, noseRay.point + noseRay.normal * 3.0f,
                             glm::vec3(1.0f, 1.0f, 0.2f));
        }
    }

    debugLines_.upload();
}

void Renderer::beginFrame(int fbWidth, int fbHeight) {
    target_.resize(fbWidth, fbHeight, kSamples);
    target_.bind();
    // Black, and it stays black wherever no light pass touches it. This is the
    // no-ambient bet made literal.
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    stats_ = Stats{};
    timers_.beginFrame();
}

glm::mat4 Renderer::buildView(const RenderState& rs) const {
    const glm::mat3 basis = glm::mat3_cast(rs.shipOrientation);
    const glm::vec3 forward = basis * glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 up = basis * glm::vec3(0.0f, 1.0f, 0.0f);

    // The shake is added to the eye and not to the look-at target, so the mine
    // appears to move rather than the ship appearing to aim somewhere else.
    if (rs.camera == CameraMode::Chase) {
        const glm::vec3 eye = rs.shipPos + basis * kChaseOffset + rs.shakeOffset;
        return glm::lookAt(eye, rs.shipPos + forward * kChaseLookAhead, up);
    }
    return glm::lookAt(rs.shipPos + rs.shakeOffset, rs.shipPos + rs.shakeOffset + forward, up);
}

glm::vec3 Renderer::eyePos(const RenderState& rs) const {
    if (rs.camera == CameraMode::Chase) {
        return rs.shipPos + glm::mat3_cast(rs.shipOrientation) * kChaseOffset;
    }
    return rs.shipPos;
}

glm::mat4 Renderer::shipModelMatrix(const RenderState& rs) const {
    return glm::translate(glm::mat4(1.0f), rs.shipPos) * glm::mat4_cast(rs.shipOrientation);
}

glm::mat4 Renderer::viewProj(const RenderState& rs) const {
    const float aspect =
        target_.height() > 0
            ? static_cast<float>(target_.width()) / static_cast<float>(target_.height())
            : 1.0f;
    return infiniteReversedZ(glm::radians(rs.verticalFov), aspect, kZNear) * buildView(rs);
}

// Silhouettes are extracted on the CPU once per light per frame, before any
// drawing, and every light's volumes go into one buffer that is uploaded once.
void Renderer::buildShadowVolumes(const RenderState& rs, const PortalWalk& walk) {
    volumes_.beginFrame();
    ranges_.clear();
    lightStats_.clear();

    if (!shadowsEnabled) {
        ranges_.assign(active_.size(), ShadowVolumes::Range{});
        lightStats_.assign(active_.size(), LightStats{});
        return;
    }

    shipOccluder_.setTransform(shipModelMatrix(rs));

    for (size_t li = 0; li < active_.size(); ++li) {
        const Light& light = *active_[li];
        const int before = volumes_.silhouetteEdges();
        volumes_.beginLight();

        LightStats ls;
        ls.segments = static_cast<int>(light.segments.size());

        bool anyVisible = false;
        for (int segment : light.segments) {
            if (walk.isVisible(segment)) {
                anyVisible = true;
                break;
            }
        }

        const bool casts = li < castsShadow_.size() && castsShadow_[li] != 0;
        if (anyVisible && casts) {
            // Depth-fail needs the whole closed mine, including walls beyond light range.
            volumes_.add(mineOccluder_, light.position);
            ++stats_.shadowCasters;

            for (size_t pi = 0; pi < props_.size(); ++pi) {
                const Prop& prop = props_[pi];
                if (!propAlive(prop, rs)) continue;
                if (prop.segment >= 0 &&
                    !std::binary_search(light.segments.begin(), light.segments.end(),
                                        prop.segment)) {
                    continue;
                }
                volumes_.add(prop.occluder, light.position);
                ++stats_.shadowCasters;
            }

            for (int i = 0; i < rs.robotCount; ++i) {
                const RenderState::RobotView& view = rs.robots[static_cast<size_t>(i)];
                const auto slot = static_cast<size_t>(view.slot);
                if (slot >= robotOccluders_.size() || !robotOccluders_[slot].mesh()) continue;
                const int segment = view.slot < static_cast<int>(rs.robotSegments.size())
                                        ? rs.robotSegments[static_cast<size_t>(view.slot)]
                                        : -1;
                if (segment >= 0 && !std::binary_search(light.segments.begin(),
                                                        light.segments.end(), segment)) {
                    continue;
                }
                volumes_.add(robotOccluders_[slot], light.position);
                ++stats_.shadowCasters;
            }

            for (size_t si = 0; si < doorSlabs_.size(); ++si) {
                const DoorSlab& slab = doorSlabs_[si];
                if (slab.segmentA < 0) continue;
                const bool reached =
                    std::binary_search(light.segments.begin(), light.segments.end(),
                                       slab.segmentA) ||
                    std::binary_search(light.segments.begin(), light.segments.end(),
                                       slab.segmentB);
                if (!reached) continue;
                volumes_.add(doorOccluders_[si], light.position);
                ++stats_.shadowCasters;
            }

            // The ship casts only when it is actually on screen, which is the
            // chase camera. In first person the camera sits INSIDE the hull, so
            // its shadow volume begins at the eye and blacks out most of the
            // view - the player shadowing themselves with a mesh they cannot
            // even see. The ship is not drawn in first person either, so this
            // keeps what casts a shadow and what is visible in agreement.
            //
            // It is never an occluder for the headlight at all: a lamp bolted
            // to the hull would spend every frame shadowing the hull it is
            // bolted to, and the view would be a black cone.
            const bool isHeadlight = static_cast<int>(li) == headlightIndex_;
            if (!isHeadlight && rs.camera == CameraMode::Chase &&
                std::binary_search(light.segments.begin(), light.segments.end(), rs.shipSegment)) {
                volumes_.add(shipOccluder_, light.position);
                ++stats_.shadowCasters;
            }
        }

        const ShadowVolumes::Range range = volumes_.endLight();
        ls.silhouetteEdges = volumes_.silhouetteEdges() - before;
        ls.volumeTriangles = range.count / 3;
        if (ls.volumeTriangles > stats_.worstVolumeTriangles) {
            stats_.worstVolumeTriangles = ls.volumeTriangles;
            stats_.worstVolumeLight = static_cast<int>(li);
        }
        ranges_.push_back(range);
        lightStats_.push_back(ls);
    }

    volumes_.upload();
    stats_.silhouetteEdges = volumes_.silhouetteEdges();
    stats_.volumeTriangles = volumes_.volumeTriangles();
}

void Renderer::depthPrepass(const RenderState& rs, const PortalWalk& walk) {
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_GEQUAL);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_CULL_FACE);
    // Depth only: no colour is written, so the light passes below do the
    // shading exactly once per visible fragment however many lights reach it.
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    depthShader_->bind();
    depthShader_->setMat4("uModel", glm::mat4(1.0f));

    levelMesh_->bind();
    for (int segment : walk.visible()) {
        levelMesh_->drawSegment(segment);
        ++stats_.segmentsDrawn;
    }

    for (size_t pi = 0; pi < props_.size(); ++pi) {
        const Prop& prop = props_[pi];
        if (prop.segment >= 0 && !walk.isVisible(prop.segment)) continue;
        if (!propAlive(prop, rs)) continue;
        depthShader_->setMat4("uModel", prop.model);
        prop.mesh->draw();
    }

    for (int i = 0; i < rs.robotCount; ++i) {
        const RenderState::RobotView& view = rs.robots[static_cast<size_t>(i)];
        if (view.type < 0 || view.type >= static_cast<int>(robotTypes_.size())) continue;
        depthShader_->setMat4("uModel", glm::translate(glm::mat4(1.0f), view.position) *
                                            glm::mat4_cast(view.orientation));
        robotTypes_[static_cast<size_t>(view.type)].mesh->draw();
    }

    for (const DoorSlab& slab : doorSlabs_) {
        if (slab.segmentA < 0) continue;
        if (!walk.isVisible(slab.segmentA) && !walk.isVisible(slab.segmentB)) continue;
        depthShader_->setMat4("uModel", slab.model);
        slabMesh_->draw();
    }

    if (rs.camera == CameraMode::Chase) {
        depthShader_->setMat4("uModel", shipModelMatrix(rs));
        ship_->draw();
    }

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

// Depth-fail stencil counting, the "Carmack's reverse" arrangement.
//
// Depth-fail rather than depth-pass because depth-pass breaks whenever the
// camera is inside a shadow volume - which in a mine lit by a headlight is most
// of the time. Counting what is BEHIND the visible surface instead of what is
// in front of it makes the near plane irrelevant.
void Renderer::stencilPass(ShadowVolumes::Range range) {
    if (range.count <= 0) return;

    glClear(GL_STENCIL_BUFFER_BIT);
    glEnable(GL_STENCIL_TEST);

    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_FALSE);

    // GL_GREATER here, not the GL_GEQUAL the rest of the frame uses, and this
    // one comparison is the difference between working shadows and a black
    // screen.
    //
    // Depth-fail counts volume faces that lie BEHIND the visible surface. A
    // volume's front cap is the occluder's own light-facing triangles, exactly
    // coincident with the surface being lit, and it has to be counted so that
    // it cancels the back cap at infinity - otherwise every lit surface finds
    // one uncancelled face behind it and shadows itself. In a conventional
    // GL_LESS depth buffer a coincident fragment fails the test and that
    // cancellation is automatic. Reversed-Z with GL_GEQUAL makes equal PASS
    // instead, so the cap silently stops counting and the whole mine goes
    // black. GL_GREATER restores the strict comparison depth-fail assumes.
    glDepthFunc(GL_GREATER);

    // Both faces of the volume are needed, so culling is off and the two sides
    // are separated by the stencil ops instead.
    glDisable(GL_CULL_FACE);

    glStencilFunc(GL_ALWAYS, 0, 0xFFu);
    // sfail, dpfail, dppass. The counting happens in the dpfail slot: a volume
    // face behind the visible surface. Back faces add, front faces subtract, so
    // a fragment with a non-zero balance has more volume behind it entering
    // than leaving, and is therefore inside.
    //
    // INCR_WRAP / DECR_WRAP rather than the saturating versions: with many
    // overlapping volumes a saturating counter sticks at 255 or 0 and stops
    // balancing, and the shadow inverts.
    glStencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR_WRAP, GL_KEEP);
    glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR_WRAP, GL_KEEP);

    // Each occluder's volume is in its own space and is drawn with the same
    // model matrix the mesh was drawn with in the prepass, so the front cap's
    // depth is bit-identical to the surface it sits on. See appendShadowVolume.
    shadowShader_->bind();
    volumes_.bind();
    volumes_.draw(range, [&](const glm::mat4& model) { shadowShader_->setMat4("uModel", model); });

    glEnable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

void Renderer::lightPasses(const RenderState& rs, const PortalWalk& walk) {
    for (size_t li = 0; li < active_.size(); ++li) {
        const Light& light = *active_[li];
        ++stats_.lightsConsidered;

        // A light only ever touches the segments in its set, and only the ones
        // the portal walk says are on screen. A light in a room you cannot see
        // does no work at all.
        lit_.clear();
        for (int segment : light.segments) {
            if (walk.isVisible(segment)) lit_.push_back(segment);
        }
        if (lit_.empty()) continue;
        ++stats_.lightsDrawn;

        const ShadowVolumes::Range range =
            li < ranges_.size() ? ranges_[li] : ShadowVolumes::Range{};
        const int timed = static_cast<int>(li);
        timers_.stencilBegin(timed);
        stencilPass(range);
        timers_.stencilEnd(timed);

        if (showStencil) {
            visualiseStencil();
            continue;
        }

        // Shade only the fragments the prepass decided are visible, and only
        // where the stencil balance came out zero.
        timers_.lightBegin(timed);
        glDepthMask(GL_FALSE);
        glDepthFunc(GL_EQUAL);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glEnable(GL_CULL_FACE);

        if (range.count > 0) {
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_EQUAL, 0, 0xFFu);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        } else {
            glDisable(GL_STENCIL_TEST);
        }

        wallShader_->bind();
        walls_->bind(0);
        wallShader_->setVec3("uEyePos", eyePos(rs));
        wallShader_->setVec3("uLightPos", light.position);
        // Static lights flicker while the mine is coming apart. Dynamic ones
        // are bolts and flares, which have nothing to do with the reactor.
        const float scale = li < staticLightCount_ ? rs.staticLightScale : 1.0f;
        wallShader_->setVec3("uLightColor", light.color * light.intensity * scale);
        wallShader_->setFloat("uLightRadius", light.radius);
        wallShader_->setInt("uIsSpot", light.type == LightType::Spot ? 1 : 0);
        if (light.type == LightType::Spot) {
            wallShader_->setVec3("uLightDir", glm::normalize(light.direction));
            const float outer = glm::radians(light.coneDegrees * 0.5f);
            wallShader_->setFloat("uCosOuter", std::cos(outer));
            wallShader_->setFloat("uCosInner", std::cos(outer * 0.75f));
        }

        wallShader_->setInt("uLayerOverride", -1);
        wallShader_->setVec3("uAlbedoTint", glm::vec3(1.0f));
        wallShader_->setMat4("uModel", glm::mat4(1.0f));
        levelMesh_->bind();
        for (int segment : lit_) {
            levelMesh_->drawSegment(segment);
            ++stats_.lightSegmentDraws;
        }

        for (size_t pi = 0; pi < props_.size(); ++pi) {
            const Prop& prop = props_[pi];
            if (prop.segment >= 0 && !walk.isVisible(prop.segment)) continue;
            if (!propAlive(prop, rs)) continue;
            if (prop.segment >= 0 && !std::binary_search(light.segments.begin(),
                                                         light.segments.end(), prop.segment)) {
                continue;
            }
            wallShader_->setInt("uLayerOverride", prop.textureLayer);
            wallShader_->setMat4("uModel", prop.model);
            prop.mesh->draw();
        }

        for (int i = 0; i < rs.robotCount; ++i) {
            const RenderState::RobotView& view = rs.robots[static_cast<size_t>(i)];
            if (view.type < 0 || view.type >= static_cast<int>(robotTypes_.size())) continue;
            const int segment = view.slot < static_cast<int>(rs.robotSegments.size())
                                    ? rs.robotSegments[static_cast<size_t>(view.slot)]
                                    : -1;
            if (segment >= 0 && !std::binary_search(light.segments.begin(), light.segments.end(),
                                                    segment)) {
                continue;
            }
            wallShader_->setInt("uLayerOverride",
                                robotTypes_[static_cast<size_t>(view.type)].textureLayer);
            wallShader_->setVec3("uAlbedoTint", view.disabledTime > 0.0f
                                                    ? disabledTint(view.disabledTime)
                                                    : glm::vec3(1.0f));
            wallShader_->setMat4("uModel", glm::translate(glm::mat4(1.0f), view.position) *
                                                glm::mat4_cast(view.orientation));
            robotTypes_[static_cast<size_t>(view.type)].mesh->draw();
        }
        wallShader_->setVec3("uAlbedoTint", glm::vec3(1.0f));

        for (const DoorSlab& slab : doorSlabs_) {
            if (slab.segmentA < 0) continue;
            if (!walk.isVisible(slab.segmentA) && !walk.isVisible(slab.segmentB)) continue;
            const bool reached =
                std::binary_search(light.segments.begin(), light.segments.end(), slab.segmentA) ||
                std::binary_search(light.segments.begin(), light.segments.end(), slab.segmentB);
            if (!reached) continue;
            wallShader_->setInt("uLayerOverride", WallHazard);
            wallShader_->setMat4("uModel", slab.model);
            slabMesh_->draw();
        }

        // The ship is lit by the same pass. It carries no texture-layer
        // attribute, so the override selects its tile from the same array.
        if (rs.camera == CameraMode::Chase && walk.isVisible(rs.shipSegment) &&
            std::binary_search(light.segments.begin(), light.segments.end(), rs.shipSegment)) {
            wallShader_->setInt("uLayerOverride", WallHull);
            wallShader_->setVec3("uAlbedoTint",
                                 rs.shipDisabled ? disabledTint(rs.shipDisabledTime)
                                                 : glm::vec3(1.0f));
            wallShader_->setMat4("uModel", shipModelMatrix(rs));
            ship_->draw();
            wallShader_->setVec3("uAlbedoTint", glm::vec3(1.0f));
        }
        timers_.lightEnd(timed);
    }

    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_GEQUAL);
}

// Tints everything the stencil currently marks as shadowed. Drawn per light,
// so the visualiser shows the last light's counts rather than a sum.
void Renderer::visualiseStencil() {
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_NOTEQUAL, 0, 0xFFu);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);

    fullscreenShader_->bind();
    fullscreenShader_->setVec4("uColor", glm::vec4(0.35f, 0.0f, 0.0f, 1.0f));
    glBindVertexArray(emptyVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

void Renderer::unlitPass(const RenderState& rs, const Level& level, const PortalWalk& walk) {
    (void)level;
    glDisable(GL_STENCIL_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_GEQUAL);

    if (drawVolumeWireframe && !ranges_.empty()) {
        // Every light's volumes at once, as lines, so the extrusion can be
        // seen rather than inferred from whether the shadow looks right.
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_FALSE);
        shadowShader_->bind();
        shadowShader_->setVec3("uColor", glm::vec3(0.15f, 0.9f, 0.35f));
        volumes_.bind();
        for (const ShadowVolumes::Range& range : ranges_) {
            volumes_.draw(range,
                          [&](const glm::mat4& model) { shadowShader_->setMat4("uModel", model); });
        }
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glEnable(GL_CULL_FACE);
        glDepthMask(GL_TRUE);
    }

    if (!doorSigns_.empty()) {
        // Alpha-blended, depth-tested, not depth-writing: signage is a decal on
        // a door, so anything in front of it hides it and it occludes nothing
        // itself - including the copy of it on the door's far face, which the
        // slab's own depth rejects. Nothing inside the buffer is offset from
        // anything else, because with no depth writes the layering is simply the
        // order the quads were emitted in: bezel, plate, then letters.
        //
        // Culling off rather than winding both faces: the far-side panels are
        // wound the other way round, and a decal has no inside to cull.
        signShader_->bind();
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        doorSigns_.draw();
        glEnable(GL_CULL_FACE);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    if (!billboards_.empty()) {
        // Additive, depth-tested but not depth-writing: a bolt lights what is
        // behind it and never occludes anything.
        billboardShader_->bind();
        pickupSprites_->bind(2);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        billboards_.draw();
        glEnable(GL_CULL_FACE);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    if (!debugLines_.empty()) {
        // Depth-tested so a line behind a wall is hidden, which is what makes
        // them readable as being in the world rather than drawn over it.
        debugLineShader_->bind();
        glDepthMask(GL_FALSE);
        debugLines_.draw();
        glDepthMask(GL_TRUE);
    }

    if (!drawLightGizmos) return;

    // Small emissive markers at each visible light. M5 replaces this pass's
    // contents with bolts, flares and explosions; the pass itself is the same.
    unlitShader_->bind();
    for (size_t li = 0; li < staticLightCount_ && li < active_.size(); ++li) {
        const Light& light = *active_[li];
        if (!walk.isVisible(light.segment)) continue;
        glm::mat4 m = glm::translate(glm::mat4(1.0f), light.position);
        m = glm::scale(m, glm::vec3(0.6f));
        unlitShader_->setMat4("uModel", m);
        unlitShader_->setVec3("uColor", light.color);
        gizmo_->draw();
    }
    (void)rs;
}

void Renderer::drawScene(const RenderState& rs, const Level& level, const PortalWalk& walk,
                         const std::vector<Light>& dynamicLights, int shadowBudget,
                         const HudData& hud, const MenuView& menu) {
    const float aspect = target_.height() > 0
                             ? static_cast<float>(target_.width()) / static_cast<float>(target_.height())
                             : 1.0f;

    CameraBlock cam;
    cam.view = buildView(rs);
    cam.proj = infiniteReversedZ(glm::radians(rs.verticalFov), aspect, kZNear);
    glNamedBufferSubData(cameraUbo_, 0, sizeof(CameraBlock), &cam);

    gatherLights(level, dynamicLights, eyePos(rs), walk, shadowBudget);
    updateRobotOccluders(rs);
    buildDoors(rs, level);
    buildDoorSigns(rs, level, walk);
    buildBillboards(rs);
    buildHud(hud, cam.proj * cam.view);
    buildMenu(menu);
    hud_.upload();
    buildShadowVolumes(rs, walk);
    timers_.passBegin(GpuTimers::Prepass);
    depthPrepass(rs, walk);
    timers_.passEnd(GpuTimers::Prepass);
    timers_.passBegin(GpuTimers::Lights);
    lightPasses(rs, walk);
    timers_.passEnd(GpuTimers::Lights);
    timers_.passBegin(GpuTimers::Unlit);
    unlitPass(rs, level, walk);
    timers_.passEnd(GpuTimers::Unlit);
}

void Renderer::buildAutomapLines(const AutomapData& map, const Level& level) {
    mapLines_.clear();

    // Colours chosen so the map reads at a glance: the room you are in is warm
    // and everything else is cold, which is the one question a player opens the
    // map to answer.
    constexpr glm::vec3 kWall(0.30f, 0.45f, 0.62f);
    constexpr glm::vec3 kHere(0.95f, 0.70f, 0.25f);
    constexpr glm::vec3 kFrontier(0.16f, 0.26f, 0.38f);

    const int n = std::min(map.segmentCount, level.segmentCount());
    for (int si = 0; si < n; ++si) {
        if (si >= AutomapData::kMaxSegments || !map.visited[static_cast<size_t>(si)]) continue;
        const Segment& seg = level.segments[static_cast<size_t>(si)];
        const bool here = si == map.shipSegment;

        for (int side = 0; side < SideCount; ++side) {
            const SideData& sd = seg.sides[static_cast<size_t>(side)];

            // A wall is drawn as its outline; a portal is drawn as nothing at
            // all, so a corridor reads as a gap rather than a line to bump into.
            // The exception is a portal into somewhere unvisited: a faint
            // outline there marks the way on without giving away what is beyond.
            glm::vec3 color = kWall;
            if (sd.isPortal()) {
                const int nb = sd.neighbour;
                const bool known = nb >= 0 && nb < AutomapData::kMaxSegments &&
                                   map.visited[static_cast<size_t>(nb)];
                if (known) continue;
                color = kFrontier;
            } else if (here) {
                color = kHere;
            }

            const std::array<int, 4>& corners = kSideVertices[static_cast<size_t>(side)];
            for (int e = 0; e < 4; ++e) {
                const glm::vec3& a =
                    level.vertices[static_cast<size_t>(seg.verts[static_cast<size_t>(corners[e])])];
                const glm::vec3& b =
                    level.vertices[static_cast<size_t>(
                        seg.verts[static_cast<size_t>(corners[(e + 1) % 4])])];
                mapLines_.line(a, b, color);
            }
        }
    }

    // The ship: a cross for where, and a stalk for which way it is pointing.
    mapLines_.cross(map.shipPosition, 1.6f, glm::vec3(1.0f, 1.0f, 1.0f));
    mapLines_.line(map.shipPosition, map.shipPosition + map.shipForward * 4.0f,
                   glm::vec3(1.0f, 1.0f, 1.0f));

    for (int mi = 0; mi < map.markerCount && mi < AutomapData::kMaxMarkers; ++mi) {
        const AutomapData::Marker& marker = map.markers[static_cast<size_t>(mi)];
        mapLines_.cross(marker.position, marker.size, marker.color);
        if (marker.hollow) continue;
        // A collected or destroyed marker gets a box around its cross, so the
        // two states are distinguishable without colour, which is already
        // carrying the key's identity.
        const float h = marker.size * 0.7f;
        const glm::vec3 c = marker.position;
        const glm::vec3 x(h, 0.0f, 0.0f), y(0.0f, h, 0.0f);
        mapLines_.line(c - x - y, c + x - y, marker.color);
        mapLines_.line(c + x - y, c + x + y, marker.color);
        mapLines_.line(c + x + y, c - x + y, marker.color);
        mapLines_.line(c - x + y, c - x - y, marker.color);
    }

    mapLines_.upload();
}

void Renderer::drawAutomap(const AutomapData& map, const Level& level, const HudData& hud,
                           const MenuView& menu) {
    buildAutomapLines(map, level);
    // The map has its own camera and no firing line in it, so the reticle stays
    // on the centre of the window; HudData::hasAimPoint is false here.
    buildHud(hud, glm::mat4(1.0f));
    buildMenu(menu);
    hud_.upload();

    // Frame the visited part of the mine rather than the whole level, so the
    // map stays legible early on instead of showing one lit room in the corner
    // of an empty box.
    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    const int n = std::min(map.segmentCount, level.segmentCount());
    for (int si = 0; si < n; ++si) {
        if (si >= AutomapData::kMaxSegments || !map.visited[static_cast<size_t>(si)]) continue;
        const Segment& seg = level.segments[static_cast<size_t>(si)];
        for (int v = 0; v < 8; ++v) {
            const glm::vec3& p = level.vertices[static_cast<size_t>(seg.verts[static_cast<size_t>(v)])];
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
    }
    if (lo.x > hi.x) {   // nothing visited yet, which should not happen
        lo = hi = map.shipPosition;
    }

    const glm::vec3 focus = (lo + hi) * 0.5f;
    const float radius = std::max(glm::length(hi - lo) * 0.5f, 4.0f);
    const float distance = radius * 1.7f / std::max(map.zoom, 0.2f);

    const float cp = std::cos(map.pitch);
    const glm::vec3 eye = focus + glm::vec3(std::cos(map.yaw) * cp, std::sin(map.pitch),
                                            std::sin(map.yaw) * cp) * distance;

    const float aspect = target_.height() > 0
                             ? static_cast<float>(target_.width()) / static_cast<float>(target_.height())
                             : 1.0f;

    CameraBlock cam;
    cam.view = glm::lookAt(eye, focus, glm::vec3(0.0f, 1.0f, 0.0f));
    // The project's own reversed-Z projection, not a second convention: the
    // depth test is still GL_GEQUAL and the clear is still 0.
    cam.proj = infiniteReversedZ(kAutomapFovY, aspect, kZNear);
    glNamedBufferSubData(cameraUbo_, 0, sizeof(CameraBlock), &cam);

    glDisable(GL_STENCIL_TEST);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_GEQUAL);

    // Timed as the unlit pass, which is what it is: lines, no lighting, no
    // stencil. Without this the timing panel reads as "no results" in map view.
    timers_.passBegin(GpuTimers::Unlit);
    if (!mapLines_.empty()) {
        debugLineShader_->bind();
        mapLines_.draw();
    }
    timers_.passEnd(GpuTimers::Unlit);

}

void Renderer::resolve() {
    target_.resolveToDefault();
    hudPass();
    // After the last GL work of the frame, so the ring advances once per frame
    // whichever view was drawn.
    timers_.endFrame();
}

} // namespace dvk::gl

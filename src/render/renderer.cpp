#include "render/renderer.h"

#include "core/log.h"
#include "game/level.h"
#include "game/state.h"
#include "game/visibility.h"
#include "sim/collision.h"
#include "sim/robots.h"
#include "render/camera.h"
#include "render/procedural.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace dvk::gl {

namespace {

constexpr float kFovY = glm::radians(70.0f);
constexpr float kZNear = 0.05f;
constexpr int kWallTileSize = 256;

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

    // One unit cube reused for every door half. A door is a box; there is no
    // reason for it to be anything else.
    slabMesh_ = std::make_unique<Mesh>(makeCube());
    {
        ModelData slab;
        slab.positions = {
            {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f},
            {-0.5f, -0.5f, 0.5f},  {0.5f, -0.5f, 0.5f},  {0.5f, 0.5f, 0.5f},  {-0.5f, 0.5f, 0.5f}};
        slab.triangles = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                          {1, 2, 6}, {1, 6, 5}, {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}};
        slabOccluderMesh_ = buildOccluder(slab.positions, slab.triangles, {}, "door slab");
    }
    doorSlabs_.resize(static_cast<size_t>(sim::kMaxDoors) * 2);
    doorOccluders_.resize(static_cast<size_t>(sim::kMaxDoors) * 2);
    for (Occluder& occluder : doorOccluders_) {
        occluder.reset(&slabOccluderMesh_, glm::mat4(1.0f));
    }

    walls_ = std::make_unique<TextureArray>(kWallTileSize, makeWallTiles(kWallTileSize), "walls");
    levelMesh_ = std::make_unique<LevelMesh>(level);

    shipModel_ = loadObj("assets/models/ship.obj");
    ship_ = std::make_unique<Mesh>(toMesh(shipModel_));
    shipOccluderMesh_ =
        buildOccluder(shipModel_.positions, shipModel_.triangles, {}, "assets/models/ship.obj");
    shipOccluder_.reset(&shipOccluderMesh_, glm::mat4(1.0f));

    gizmo_ = std::make_unique<Mesh>(makeCube());

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

    loadRobotMeshes(robots);
    loadProps(level, robots);

    lit_.reserve(level.segments.size());
    ranges_.reserve(level.lights.size() + 1);
    lightStats_.reserve(level.lights.size() + 1);

    glCreateVertexArrays(1, &emptyVao_);

    glCreateBuffers(1, &cameraUbo_);
    glNamedBufferStorage(cameraUbo_, sizeof(CameraBlock), nullptr, GL_DYNAMIC_STORAGE_BIT);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, cameraUbo_);
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

// Places the two halves of every door. A door slides apart across the opening,
// so the halves stay inside the doorway's own plane and end up buried in the
// wall to either side rather than poking through the ceiling.
//
// Each half is oversized by kSlabMargin at the frame and at the top and
// bottom, so its outer faces sit inside the rock rather than exactly in the
// plane of the adjacent wall. Exactly coplanar faces from two different
// meshes z-fight in the depth prepass and then disagree in every light pass,
// which showed as a fizzing strip around the door frame. The halves still
// meet exactly at the centre seam: those two faces face away from each other,
// so only one is ever drawn.
//
// The slabs are always drawn and always cast, whatever the openness. Fully
// open they are buried in the wall, where the depth test hides them and
// their volumes fall inside the mine's own shadow; while opening they are the
// only thing occluding the doorway, since the portal walk and the light flood
// see through a door from the moment it cracks open.
void Renderer::buildDoors(const RenderState& rs, const Level& level) {
    constexpr float kSlabMargin = 0.25f;
    const size_t count = std::min(level.doors.size(), static_cast<size_t>(sim::kMaxDoors));
    for (size_t di = 0; di < count; ++di) {
        const Door& door = level.doors[di];
        const float openness =
            di < static_cast<size_t>(rs.doorCount) ? rs.doorOpenness[di] : 0.0f;

        // A frame from the opening's own edges, so a deformed doorway still
        // gets a slab that fits it.
        const glm::vec3 acrossRaw = (door.corners[1] - door.corners[0]) +
                                    (door.corners[2] - door.corners[3]);
        const glm::vec3 upRaw = (door.corners[3] - door.corners[0]) +
                                (door.corners[2] - door.corners[1]);
        const float width = glm::length(acrossRaw) * 0.5f;
        const float height = glm::length(upRaw) * 0.5f;
        if (width < 1e-3f || height < 1e-3f) continue;

        const glm::vec3 across = acrossRaw / (width * 2.0f);
        const glm::vec3 up = upRaw / (height * 2.0f);
        const glm::vec3 normal = glm::normalize(glm::cross(across, up));

        glm::mat4 basis(1.0f);
        basis[0] = glm::vec4(across, 0.0f);
        basis[1] = glm::vec4(up, 0.0f);
        basis[2] = glm::vec4(normal, 0.0f);

        // Half the opening plus the margin, as a half-extent, and how far the
        // half slides to end up entirely inside the wall when fully open.
        const float halfWidth = width * 0.25f + kSlabMargin * 0.5f;
        const float travel = width * 0.5f + kSlabMargin;
        for (int half = 0; half < 2; ++half) {
            const float side = half == 0 ? -1.0f : 1.0f;
            const float slide = side * (halfWidth + openness * travel);
            const glm::vec3 centre = door.center + across * slide;

            glm::mat4 model = glm::translate(glm::mat4(1.0f), centre) * basis;
            model = glm::scale(model,
                               glm::vec3(halfWidth * 2.0f, height + kSlabMargin * 2.0f, 0.9f));

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

void Renderer::buildHud(const HudData& hud) {
    if (!hud.show) {
        hud_.begin(target_.width(), target_.height());
        hud_.upload();
        return;
    }

    const float w = static_cast<float>(target_.width());
    const float h = static_cast<float>(target_.height());
    // One scale for the whole HUD, tied to height, so it reads the same on any
    // window size instead of shrinking to nothing at 4K.
    const float s = std::max(1.5f, h / 260.0f);
    hud_.begin(target_.width(), target_.height());

    const glm::vec4 white(1.0f, 1.0f, 1.0f, 0.92f);
    const glm::vec4 dim(0.75f, 0.8f, 0.9f, 0.8f);

    // Reticle: a gap in the middle so it frames the target rather than
    // covering it.
    const float cx = w * 0.5f;
    const float cy = h * 0.5f;
    const float arm = 5.0f * s;
    const float gap = 3.0f * s;
    const float thin = std::max(1.0f, s * 0.6f);
    const glm::vec4 reticle(0.6f, 1.0f, 0.7f, 0.75f);
    hud_.rect(cx - gap - arm, cy - thin * 0.5f, arm, thin, reticle);
    hud_.rect(cx + gap, cy - thin * 0.5f, arm, thin, reticle);
    hud_.rect(cx - thin * 0.5f, cy - gap - arm, thin, arm, reticle);
    hud_.rect(cx - thin * 0.5f, cy + gap, thin, arm, reticle);

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

    // Weapon and ammo, bottom right.
    {
        std::string line = hud.weaponName;
        if (hud.laserLevel > 0 && line == "laser") line += " " + std::to_string(hud.laserLevel);
        if (hud.ammo >= 0) line += "  " + std::to_string(hud.ammo);
        const float tw = hud_.textWidth(line, s);
        hud_.text(line, w - left - tw, h - 8.0f * s - 7.0f * s, s, white);

        const std::string flares = "flares " + std::to_string(hud.flares);
        const float fw = hud_.textWidth(flares, s * 0.7f);
        hud_.text(flares, w - left - fw, h - 8.0f * s - 18.0f * s, s * 0.7f, dim);
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

    hud_.upload();
}

void Renderer::hudPass() {
    if (hud_.quadCount() == 0) return;
    // Drawn on the resolved framebuffer, after the blit, for the same reason
    // ImGui is: it is axis-aligned and gains nothing from multisampling.
    hudShader_->bind();
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

    // Pickups. Emissive so they can be found in a dark corridor, and pulsing
    // so they are not mistaken for a wall detail.
    for (int i = 0; i < rs.pickupCount; ++i) {
        const RenderState::PickupView& pickup = rs.pickups[static_cast<size_t>(i)];
        if (!pickup.present) continue;
        billboards_.add(pickup.position, right, up, pickup.size, pickup.color);
    }

    // Robot eyes. Emissive, so they read in total darkness - which matters
    // more than it sounds in a mine with no ambient light, because the eyes
    // are usually the first thing the player sees of a robot.
    if (robotConfig_) {
        for (int i = 0; i < rs.robotCount; ++i) {
            const RenderState::RobotView& view = rs.robots[static_cast<size_t>(i)];
            if (view.type < 0 || view.type >= static_cast<int>(robotConfig_->types.size())) {
                continue;
            }
            const sim::RobotDef& def = robotConfig_->types[static_cast<size_t>(view.type)];
            const glm::mat3 basis = glm::mat3_cast(view.orientation);
            for (const glm::vec3& eye : def.eyes) {
                billboards_.add(view.position + basis * eye, right, up, def.eyeSize, def.eyeColor);
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
    return infiniteReversedZ(kFovY, aspect, kZNear) * buildView(rs);
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
        stencilPass(range);

        if (showStencil) {
            visualiseStencil();
            continue;
        }

        // Shade only the fragments the prepass decided are visible, and only
        // where the stencil balance came out zero.
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
            wallShader_->setMat4("uModel", glm::translate(glm::mat4(1.0f), view.position) *
                                                glm::mat4_cast(view.orientation));
            robotTypes_[static_cast<size_t>(view.type)].mesh->draw();
        }

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
            wallShader_->setMat4("uModel", shipModelMatrix(rs));
            ship_->draw();
        }
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

    if (!billboards_.empty()) {
        // Additive, depth-tested but not depth-writing: a bolt lights what is
        // behind it and never occludes anything.
        billboardShader_->bind();
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
                         const HudData& hud) {
    const float aspect = target_.height() > 0
                             ? static_cast<float>(target_.width()) / static_cast<float>(target_.height())
                             : 1.0f;

    CameraBlock cam;
    cam.view = buildView(rs);
    cam.proj = infiniteReversedZ(kFovY, aspect, kZNear);
    glNamedBufferSubData(cameraUbo_, 0, sizeof(CameraBlock), &cam);

    gatherLights(level, dynamicLights, eyePos(rs), walk, shadowBudget);
    updateRobotOccluders(rs);
    buildDoors(rs, level);
    buildBillboards(rs);
    buildHud(hud);
    buildShadowVolumes(rs, walk);
    depthPrepass(rs, walk);
    lightPasses(rs, walk);
    unlitPass(rs, level, walk);
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

void Renderer::drawAutomap(const AutomapData& map, const Level& level, const HudData& hud) {
    buildAutomapLines(map, level);
    buildHud(hud);

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
    cam.proj = infiniteReversedZ(kFovY, aspect, kZNear);
    glNamedBufferSubData(cameraUbo_, 0, sizeof(CameraBlock), &cam);

    glDisable(GL_STENCIL_TEST);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_GEQUAL);

    if (!mapLines_.empty()) {
        debugLineShader_->bind();
        mapLines_.draw();
    }

}

void Renderer::resolve() {
    target_.resolveToDefault();
    hudPass();
}

} // namespace dvk::gl

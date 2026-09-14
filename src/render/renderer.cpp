#include "render/renderer.h"

#include "core/log.h"
#include "game/level.h"
#include "game/state.h"
#include "game/visibility.h"
#include "sim/collision.h"
#include "render/camera.h"
#include "render/procedural.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>

namespace dvk::gl {

namespace {

constexpr float kFovY = glm::radians(70.0f);
constexpr float kZNear = 0.05f;
constexpr int kWallTileSize = 256;

constexpr glm::vec3 kChaseOffset{0.0f, 2.6f, 9.0f};
constexpr float kChaseLookAhead = 6.0f;

} // namespace

Renderer::Renderer(const Level& level) {
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

    loadProps(level);

    segmentMask_.assign(level.segments.size(), 0);
    lit_.reserve(level.segments.size());
    ranges_.reserve(level.lights.size() + 1);
    lightStats_.reserve(level.lights.size() + 1);

    glCreateVertexArrays(1, &emptyVao_);

    glCreateBuffers(1, &cameraUbo_);
    glNamedBufferStorage(cameraUbo_, sizeof(CameraBlock), nullptr, GL_DYNAMIC_STORAGE_BIT);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, cameraUbo_);
}

void Renderer::loadProps(const Level& level) {
    for (const LevelObject& obj : level.objects) {
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
    // The level's own lights and the headlight take slots first, because they
    // are the ones a player has time to look at. Projectile, flare and
    // explosion lights compete for what is left, nearest to the camera first -
    // a bolt across the room contributes a shadow nobody can resolve.
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

    const size_t staticCount = level.lights.size();
    for (size_t i = 0; i < staticCount && i < active_.size(); ++i) {
        // Only if it can be seen at all: an off-screen light casting into
        // nothing is the cheapest thing to give up.
        bool visible = false;
        for (int segment : active_[i]->segments) {
            if (walk.isVisible(segment)) {
                visible = true;
                break;
            }
        }
        if (visible) claim(i);
    }
    if (headlightIndex_ >= 0) claim(static_cast<size_t>(headlightIndex_));

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

    if (rs.camera == CameraMode::Chase) {
        const glm::vec3 eye = rs.shipPos + basis * kChaseOffset;
        return glm::lookAt(eye, rs.shipPos + forward * kChaseLookAhead, up);
    }
    return glm::lookAt(rs.shipPos, rs.shipPos + forward, up);
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
            std::fill(segmentMask_.begin(), segmentMask_.end(), uint8_t{0});
            for (int segment : light.segments) {
                segmentMask_[static_cast<size_t>(segment)] = 1;
            }

            // The mine's own walls, restricted to this light's segments.
            volumes_.add(mineOccluder_, light.position, &segmentMask_);
            ++stats_.shadowCasters;

            for (size_t pi = 0; pi < props_.size(); ++pi) {
                const Prop& prop = props_[pi];
                if (pi < static_cast<size_t>(rs.objectCount) && !rs.objectAlive[pi]) continue;
                if (prop.segment >= 0 &&
                    !std::binary_search(light.segments.begin(), light.segments.end(),
                                        prop.segment)) {
                    continue;
                }
                volumes_.add(prop.occluder, light.position, nullptr);
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
                volumes_.add(shipOccluder_, light.position, nullptr);
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
        if (pi < static_cast<size_t>(rs.objectCount) && !rs.objectAlive[pi]) continue;
        depthShader_->setMat4("uModel", prop.model);
        prop.mesh->draw();
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

    shadowShader_->bind();
    shadowShader_->setMat4("uModel", glm::mat4(1.0f));
    volumes_.bind();
    volumes_.draw(range);

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
        wallShader_->setVec3("uLightColor", light.color * light.intensity);
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
            if (pi < static_cast<size_t>(rs.objectCount) && !rs.objectAlive[pi]) continue;
            if (prop.segment >= 0 && !std::binary_search(light.segments.begin(),
                                                         light.segments.end(), prop.segment)) {
                continue;
            }
            wallShader_->setInt("uLayerOverride", prop.textureLayer);
            wallShader_->setMat4("uModel", prop.model);
            prop.mesh->draw();
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

void Renderer::unlitPass(const RenderState& rs, const PortalWalk& walk) {
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
        shadowShader_->setMat4("uModel", glm::mat4(1.0f));
        shadowShader_->setVec3("uColor", glm::vec3(0.15f, 0.9f, 0.35f));
        volumes_.bind();
        for (const ShadowVolumes::Range& range : ranges_) volumes_.draw(range);
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
                         const std::vector<Light>& dynamicLights, int shadowBudget) {
    const float aspect = target_.height() > 0
                             ? static_cast<float>(target_.width()) / static_cast<float>(target_.height())
                             : 1.0f;

    CameraBlock cam;
    cam.view = buildView(rs);
    cam.proj = infiniteReversedZ(kFovY, aspect, kZNear);
    glNamedBufferSubData(cameraUbo_, 0, sizeof(CameraBlock), &cam);

    gatherLights(level, dynamicLights, eyePos(rs), walk, shadowBudget);
    buildBillboards(rs);
    buildShadowVolumes(rs, walk);
    depthPrepass(rs, walk);
    lightPasses(rs, walk);
    unlitPass(rs, walk);
}

void Renderer::resolve() {
    target_.resolveToDefault();
}

} // namespace dvk::gl

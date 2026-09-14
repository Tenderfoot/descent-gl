#include "render/renderer.h"

#include "core/log.h"
#include "game/level.h"
#include "game/state.h"
#include "game/visibility.h"
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

// Chase camera offset in the ship's own frame: behind and slightly above.
constexpr glm::vec3 kChaseOffset{0.0f, 2.6f, 9.0f};
constexpr float kChaseLookAhead = 6.0f;

} // namespace

Renderer::Renderer(const Level& level) {
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

    depthShader_ = std::make_unique<Shader>("shaders/depth.vert", "shaders/depth.frag");
    wallShader_ = std::make_unique<Shader>("shaders/wall.vert", "shaders/wall.frag");
    unlitShader_ = std::make_unique<Shader>("shaders/unlit.vert", "shaders/unlit.frag");

    walls_ = std::make_unique<TextureArray>(kWallTileSize, makeWallTiles(kWallTileSize), "walls");
    levelMesh_ = std::make_unique<LevelMesh>(level);

    shipModel_ = loadObj("assets/models/ship.obj");
    ship_ = std::make_unique<Mesh>(toMesh(shipModel_));
    gizmo_ = std::make_unique<Mesh>(makeCube());

    lit_.reserve(level.segments.size());

    glCreateBuffers(1, &cameraUbo_);
    glNamedBufferStorage(cameraUbo_, sizeof(CameraBlock), nullptr, GL_DYNAMIC_STORAGE_BIT);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, cameraUbo_);
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

// The camera is the ship. In first person the view is the inverse of the ship's
// own transform, which is what makes a 6DOF roll roll the world rather than
// swinging a separately-tracked camera around.
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

// Where the camera actually is, which the chase view moves away from the ship.
// The specular term needs it, and so does anything later that reasons about
// distance to the viewer.
glm::vec3 Renderer::eyePos(const RenderState& rs) const {
    if (rs.camera == CameraMode::Chase) {
        return rs.shipPos + glm::mat3_cast(rs.shipOrientation) * kChaseOffset;
    }
    return rs.shipPos;
}

glm::mat4 Renderer::shipModelMatrix(const RenderState& rs) const {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), rs.shipPos);
    return m * glm::mat4_cast(rs.shipOrientation);
}

void Renderer::depthPrepass(const RenderState& rs, const PortalWalk& walk) {
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_GEQUAL);
    glDisable(GL_BLEND);
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

    if (rs.camera == CameraMode::Chase) {
        depthShader_->setMat4("uModel", shipModelMatrix(rs));
        ship_->draw();
    }

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

void Renderer::lightPasses(const RenderState& rs, const Level& level, const PortalWalk& walk) {
    // Shade only the fragments the prepass decided are visible. GL_EQUAL
    // against the prepass depth, with depth writes off so the passes do not
    // fight each other.
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_EQUAL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);

    wallShader_->bind();
    walls_->bind(0);
    wallShader_->setVec3("uEyePos", eyePos(rs));
    wallShader_->setMat4("uModel", glm::mat4(1.0f));

    for (const Light& light : level.lights) {
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

        // ------------------------------------------------------------------
        // M3 INSERTS THE SHADOW VOLUMES HERE.
        //
        //   glClear(GL_STENCIL_BUFFER_BIT);
        //   <draw this light's shadow volumes, colour write off, depth-fail
        //    two-sided stencil counting>
        //   glStencilFunc(GL_EQUAL, 0, ~0);   // then the pass below
        //
        // The loop is shaped this way on purpose so that milestone inserts
        // rather than restructures. Nothing below depends on the stencil being
        // untouched, and the state it will need (multisampled stencil, an
        // infinite far plane) already exists.
        // ------------------------------------------------------------------

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
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_GEQUAL);
}

void Renderer::unlitPass(const Level& level, const PortalWalk& walk) {
    if (!drawLightGizmos) return;

    // Small emissive markers at each visible light. M5 replaces this pass's
    // contents with bolts, flares and explosions; the pass itself is the same.
    unlitShader_->bind();
    for (const Light& light : level.lights) {
        if (!walk.isVisible(light.segment)) continue;
        glm::mat4 m = glm::translate(glm::mat4(1.0f), light.position);
        m = glm::scale(m, glm::vec3(0.6f));
        unlitShader_->setMat4("uModel", m);
        unlitShader_->setVec3("uColor", light.color);
        gizmo_->draw();
    }
}

glm::mat4 Renderer::viewProj(const RenderState& rs) const {
    const float aspect =
        target_.height() > 0
            ? static_cast<float>(target_.width()) / static_cast<float>(target_.height())
            : 1.0f;
    return infiniteReversedZ(kFovY, aspect, kZNear) * buildView(rs);
}

void Renderer::drawScene(const RenderState& rs, const Level& level, const PortalWalk& walk) {
    const float aspect = target_.height() > 0
                             ? static_cast<float>(target_.width()) / static_cast<float>(target_.height())
                             : 1.0f;

    CameraBlock cam;
    cam.view = buildView(rs);
    cam.proj = infiniteReversedZ(kFovY, aspect, kZNear);
    glNamedBufferSubData(cameraUbo_, 0, sizeof(CameraBlock), &cam);

    depthPrepass(rs, walk);
    lightPasses(rs, level, walk);
    unlitPass(level, walk);
}

void Renderer::resolve() {
    target_.resolveToDefault();
}

} // namespace dvk::gl

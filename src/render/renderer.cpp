#include "render/renderer.h"

#include "core/log.h"
#include "game/state.h"
#include "render/camera.h"
#include "render/procedural.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <random>

namespace dvk::gl {

namespace {

constexpr float kFovY = glm::radians(70.0f);
constexpr float kZNear = 0.05f;

// The reference volume M1 flies around in. Large enough that the ship's top
// speed is legible (ten seconds corner to corner) without being so large that
// the props thin out to nothing.
constexpr float kRoomSize = 200.0f;
constexpr int kPropCount = 48;

// Chase camera offset in the ship's own frame: behind and slightly above.
constexpr glm::vec3 kChaseOffset{0.0f, 2.6f, 9.0f};
constexpr float kChaseLookAhead = 6.0f;

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

    const std::vector<uint8_t> grid = makeGridTile(512, 8);
    gridTex_ = std::make_unique<Texture2D>(512, 512, grid.data(), "grid");
    checker_ = std::make_unique<Texture2D>("assets/textures/checker.png");

    const std::vector<uint8_t> hull = makeHullTile(256);
    hullTex_ = std::make_unique<Texture2D>(256, 256, hull.data(), "hull");

    room_ = std::make_unique<Mesh>(makeInsideBox(kRoomSize, 0.25f));
    cube_ = std::make_unique<Mesh>(makeCube());

    shipModel_ = loadObj("assets/models/ship.obj");
    ship_ = std::make_unique<Mesh>(toMesh(shipModel_));

    // Props are scattered once, from a fixed seed, so the reference environment
    // is identical from run to run and a screenshot can be compared against an
    // earlier one.
    std::mt19937 rng(1995u);
    std::uniform_real_distribution<float> place(-kRoomSize * 0.45f, kRoomSize * 0.45f);
    std::uniform_real_distribution<float> scale(2.0f, 9.0f);
    std::uniform_real_distribution<float> angle(0.0f, glm::two_pi<float>());

    props_.reserve(kPropCount);
    for (int i = 0; i < kPropCount; ++i) {
        glm::vec3 pos{place(rng), place(rng), place(rng)};
        // Keep the spawn point clear so the ship does not start inside a cube.
        if (glm::length(pos) < 25.0f) pos += glm::normalize(pos) * 25.0f;

        glm::mat4 m = glm::translate(glm::mat4(1.0f), pos);
        m = glm::rotate(m, angle(rng), glm::normalize(glm::vec3(0.3f, 1.0f, 0.2f)));
        m = glm::rotate(m, angle(rng), glm::normalize(glm::vec3(1.0f, 0.15f, 0.4f)));
        props_.push_back(glm::scale(m, glm::vec3(scale(rng))));
    }

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

void Renderer::drawScene(const RenderState& rs) {
    const float aspect = target_.height() > 0
                             ? static_cast<float>(target_.width()) / static_cast<float>(target_.height())
                             : 1.0f;

    CameraBlock cam;
    cam.view = buildView(rs);
    cam.proj = infiniteReversedZ(kFovY, aspect, kZNear);
    glNamedBufferSubData(cameraUbo_, 0, sizeof(CameraBlock), &cam);

    shader_->bind();

    gridTex_->bind(0);
    shader_->setMat4("uModel", glm::mat4(1.0f));
    room_->draw();

    checker_->bind(0);
    for (const glm::mat4& m : props_) {
        shader_->setMat4("uModel", m);
        cube_->draw();
    }

    // In first person the ship would fill the screen from the inside, so it is
    // drawn only from the chase camera.
    if (rs.camera == CameraMode::Chase) {
        hullTex_->bind(0);
        glm::mat4 model = glm::translate(glm::mat4(1.0f), rs.shipPos);
        model *= glm::mat4_cast(rs.shipOrientation);
        shader_->setMat4("uModel", model);
        ship_->draw();
    }
}

void Renderer::resolve() {
    target_.resolveToDefault();
}

} // namespace dvk::gl

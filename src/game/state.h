#pragma once

#include "sim/combat.h"
#include "sim/gameflow.h"
#include "sim/props.h"
#include "sim/robots.h"
#include "sim/ship.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace dvk {

// Which camera the renderer builds its view matrix from. First person is the
// game; chase exists so the ship model is visible while its flight constants
// are being tuned.
enum class CameraMode { FirstPerson, Chase };

// Where the chase camera's eye wants to sit, in the ship's own space, and how
// far ahead of the ship it looks.
//
// Wants, not does: the mine is frequently in the way, and the eye is pulled in
// along this same line until it is not. That resolve needs the level and the
// doors, so it happens in prepareRenderFrame and its answer arrives here as
// RenderState::chaseDistance.
inline constexpr glm::vec3 kChaseOffset{0.0f, 2.6f, 9.0f};
inline constexpr float kChaseLookAhead = 6.0f;

// The unit vector along kChaseOffset, which is what a resolved distance is
// measured in. A function rather than a constant because normalising is not
// constexpr, and a namespace-scope variable needing a dynamic initialiser is a
// static-order hazard for the sake of three arithmetic operations.
inline glm::vec3 chaseDirection() { return glm::normalize(kChaseOffset); }

// State advanced by the fixed 120 Hz step. Everything the renderer needs to
// interpolate lives here and nowhere else.
struct SimState {
    sim::ShipState ship;
    // Fixed-capacity pools held by value: SimState is copied every step, and a
    // vector here would allocate in the simulation loop.
    sim::CombatState combat;
    sim::RobotState robots;
    sim::PropState props;
    sim::GameState game;
};

// What the renderer draws: SimState interpolated between the two most recent
// steps. Keeping it a distinct type is what stops render code from quietly
// reading or mutating simulation state.
//
// The camera mode is set by main after interpolation rather than simulated: it
// is a view of the world, not part of it.
struct RenderState {
    glm::vec3 shipPos{0.0f};
    glm::quat shipOrientation{1.0f, 0.0f, 0.0f, 0.0f};
    CameraMode camera = CameraMode::FirstPerson;
    float verticalFov = 70.0f;   // degrees
    // The player setting, carried here like the field of view so the renderer
    // needs no settings of its own.
    bool weaponHitboxes = false;
    int shipSegment = 0;

    // Readouts, not used to draw the scene.
    float speed = 0.0f;         // units/s
    float angularSpeed = 0.0f;  // rad/s

    // What the unlit pass draws. Interpolated by pool slot, which is stable
    // because the pools never compact.
    struct Bolt {
        glm::vec3 position{0.0f};
        glm::vec3 color{1.0f};
        float size = 0.5f;
    };
    struct Flash {
        glm::vec3 position{0.0f};
        glm::vec3 color{1.0f};
        float size = 1.0f;
        float fade = 1.0f;   // 1 at birth, 0 at death
        // An explosion's shockwave: how far out it has grown this frame, or 0
        // for a flash that has none.
        float ring = 0.0f;
    };
    std::array<Bolt, sim::kMaxProjectiles> bolts{};
    int boltCount = 0;
    std::array<Flash, sim::kMaxEffects> flashes{};
    int flashCount = 0;

    // Hitscan tracers. Not interpolated: a beam is struck at one instant and
    // lives for a tenth of a second, so blending it between two steps would
    // smear it rather than smooth it.
    struct BeamView {
        glm::vec3 start{0.0f};
        glm::vec3 end{0.0f};
        glm::vec3 color{1.0f};
        float width = 0.12f;
    };
    std::array<BeamView, sim::kMaxBeams> beams{};
    int beamCount = 0;

    // Which level objects are still standing, so a destroyed target stops
    // being drawn and stops casting.
    std::array<bool, sim::kMaxObjects> objectAlive{};
    int objectCount = 0;

    // Robots, interpolated by slot.
    struct RobotView {
        glm::vec3 position{0.0f};
        glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
        int type = 0;
        int slot = 0;
        // Seconds this robot has been disabled, or 0 while it is healthy. It
        // drives the red pulse, and it comes from the simulation rather than a
        // render clock so every robot's pulse is its own.
        float disabledTime = 0.0f;
        // Seconds into a special attack's wind-up, and where its orb sits.
        // Zero when no wind-up is running, which includes having nothing to
        // wind up. The renderer turns it into a fraction with the type's
        // SpecialAttackDef::windup.
        float windup = 0.0f;
        glm::vec3 muzzle{0.0f};
    };
    std::array<RobotView, sim::kMaxRobots> robots{};
    // Indexed by robot slot, so the light and shadow passes can cull a robot
    // to the segments a light actually reaches.
    std::array<int, sim::kMaxRobots> robotSegments{};
    int robotCount = 0;

    // Robot attachments, on their robots and knocked loose, as one list:
    // they are the same mesh and are lit and shadowed the same way either way.
    // `key` is stable per attachment - the robot slot while on, kMaxRobots plus
    // that slot once loose - so the renderer can keep one occluder for each.
    struct AttachmentView {
        glm::vec3 position{0.0f};
        glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
        int type = 0;
        int segment = -1;
        int key = 0;
        float disabledTime = 0.0f;
    };
    std::array<AttachmentView, sim::kMaxRobots * 2> attachments{};
    int attachmentCount = 0;

    // Pickups still on the floor, as emissive markers: the level's own first,
    // in its order, then whatever the robots have dropped.
    struct PickupView {
        glm::vec3 position{0.0f};
        // Multiplies the sprite where there is one, so the white key template
        // comes out blue, yellow or purple; the whole colour where there is not.
        glm::vec3 color{1.0f};
        float size = 1.0f;
        // A layer of the pickup sprite array, or -1 for the procedural glow
        // the kinds with no art still use. See render/sprites.h.
        int sprite = -1;
        bool present = false;
    };
    std::array<PickupView, sim::kMaxPickups + sim::kMaxDrops> pickups{};
    int pickupCount = 0;

    // How far each door has slid open, so the renderer can place the slabs.
    std::array<float, sim::kMaxDoors> doorOpenness{};
    int doorCount = 0;

    // Blue, yellow, purple: which keys the player is carrying. HudData carries the
    // same three flags for the readout, but this is not a readout - the signage
    // on a locked door is part of the world, and the renderer has to know what
    // the door would do if the player flew at it.
    std::array<bool, 3> keys{};

    // The reactor is gone. Here for the same reason `keys` is: an emergency
    // hatch reads it the way a keyed door reads a key, and its signage has to
    // say whether the hatch in front of the player is going to open.
    bool exitOpen = false;

    // Floating lights and swinging lamps, as the unlit pass draws them. The
    // lights themselves go into the dynamic light list; these are the visible
    // fixtures.
    struct PropView {
        glm::vec3 position{0.0f};
        glm::vec3 color{1.0f};
        float size = 1.0f;
        // Swinging lamps only: where the cord is anchored, so the renderer can
        // draw the cord the lamp is hanging from.
        bool corded = false;
        glm::vec3 anchor{0.0f};
    };
    std::array<PropView, sim::kMaxFloatingLights + sim::kMaxSwingingLights> props{};
    int propCount = 0;

    // The mine coming apart during the countdown: a world-space offset added
    // to the camera, and a multiplier on every static light so they flicker.
    glm::vec3 shakeOffset{0.0f};
    float staticLightScale = 1.0f;

    // The player's ship is under the disabled threshold: tinted in the chase
    // camera, and the reason the HUD is showing a warning. The clock is the
    // level's own elapsed time, used only to give the pulse a phase.
    bool shipDisabled = false;
    float shipDisabledTime = 0.0f;

    // Seconds since the level started, straight from the fixed-step
    // simulation. Anything that animates in a shader phases off this rather
    // than a wall clock, so a run of --frames N renders the same pictures every
    // time and a screenshot still compares against an earlier one.
    float elapsed = 0.0f;

    // Chase camera only, and meaningless in first person.
    //
    // How far along kChaseOffset the eye actually ended up once the mine had
    // its say, and whether the ship is close enough to the eye that drawing it
    // would put the camera inside its own hull. Both are resolved once per
    // frame and read here rather than recomputed, because the view matrix, the
    // light culling and the specular term all ask where the eye is and a
    // disagreement between them is a frame that shades against one camera and
    // draws from another.
    float chaseDistance = 0.0f;
    bool drawShip = false;
};

inline RenderState interpolate(const SimState& a, const SimState& b, float t) {
    RenderState r;
    r.shipPos = glm::mix(a.ship.body.position, b.ship.body.position, t);
    // slerp, not mix: the shortest arc on the unit sphere. Component-wise
    // interpolation of a quaternion is only approximately a rotation, and the
    // error shows as a hitch at high turn rates.
    r.shipOrientation = glm::slerp(a.ship.orientation, b.ship.orientation, t);
    r.shipSegment = b.ship.body.segment;
    r.speed = glm::length(glm::mix(a.ship.body.velocity, b.ship.body.velocity, t));
    r.angularSpeed = glm::length(glm::mix(a.ship.angularVelocity(), b.ship.angularVelocity(), t));

    // Projectiles interpolate by slot. A slot that was empty last step has no
    // previous position to blend from, so it is drawn where it is - which is
    // correct, since it was just created.
    for (size_t i = 0; i < sim::kMaxProjectiles; ++i) {
        const sim::Projectile& cur = b.combat.projectiles.items[i];
        if (!cur.alive) continue;
        const sim::Projectile& prev = a.combat.projectiles.items[i];
        RenderState::Bolt bolt;
        bolt.position = prev.alive ? glm::mix(prev.position, cur.position, t) : cur.position;
        bolt.color = cur.color;
        bolt.size = cur.kind == sim::ProjectileKind::Flare ? 0.8f : cur.radius * 2.2f;
        r.bolts[static_cast<size_t>(r.boltCount++)] = bolt;
    }

    // Flares that have landed do not move, so they need no interpolation - but
    // they do need drawing, and they live in a different pool.
    for (const sim::Flare& f : b.combat.flares.items) {
        if (!f.alive || r.boltCount >= static_cast<int>(sim::kMaxProjectiles)) continue;
        RenderState::Bolt bolt;
        bolt.position = f.position;
        const float remaining = f.lifetime - f.age;
        bolt.color = glm::vec3(1.0f, 0.72f, 0.36f) * std::min(1.0f, remaining / 4.0f);
        bolt.size = 0.7f;
        r.bolts[static_cast<size_t>(r.boltCount++)] = bolt;
    }

    for (const sim::Beam& beam : b.combat.beams.items) {
        if (!beam.alive) continue;
        RenderState::BeamView view;
        view.start = beam.start;
        view.end = beam.end;
        // Fades over its short life, so a railgun tracer hangs for a moment
        // and then is gone rather than blinking out.
        const float k = beam.lifetime > 0.0f ? beam.age / beam.lifetime : 1.0f;
        view.color = beam.color * std::max(0.0f, 1.0f - k);
        view.width = beam.width;
        r.beams[static_cast<size_t>(r.beamCount++)] = view;
    }

    for (const sim::Effect& e : b.combat.effects.items) {
        if (!e.alive) continue;
        RenderState::Flash flash;
        flash.position = e.position;
        flash.color = e.color;
        const float k = e.lifetime > 0.0f ? e.age / e.lifetime : 1.0f;
        flash.fade = std::max(0.0f, 1.0f - k);
        // Expanding as it fades reads as an explosion rather than a fading dot.
        flash.size = e.size * (0.55f + 0.85f * k);
        // The shockwave eases out to the full blast radius over the first 40%
        // of the flash and holds there while it fades, so the edge of the
        // damage is on screen long enough to be read.
        if (e.ringRadius > 0.0f) {
            const float g = std::min(1.0f, k / 0.4f);
            flash.ring = e.ringRadius * (1.0f - (1.0f - g) * (1.0f - g));
        }
        r.flashes[static_cast<size_t>(r.flashCount++)] = flash;
    }

    for (int i = 0; i < b.robots.count; ++i) {
        const sim::Robot& cur = b.robots.robots[static_cast<size_t>(i)];
        if (!cur.alive) continue;
        const sim::Robot& prev = a.robots.robots[static_cast<size_t>(i)];
        RenderState::RobotView view;
        view.type = cur.type;
        view.slot = i;
        view.disabledTime = cur.mode == sim::RobotMode::Disabled ? cur.modeTime : 0.0f;
        view.position =
            prev.alive ? glm::mix(prev.body.position, cur.body.position, t) : cur.body.position;
        view.orientation =
            prev.alive ? glm::slerp(prev.orientation, cur.orientation, t) : cur.orientation;
        // The robot's model matrix above is interpolated, so the muzzle and
        // the attachment are too, or the attachment would lag its robot.
        const bool hadAttachment = prev.alive && prev.attachmentOn;
        view.muzzle = hadAttachment ? glm::mix(prev.muzzle, cur.muzzle, t) : cur.muzzle;
        view.windup = cur.windingUp ? cur.windupTime : 0.0f;
        r.robotSegments[static_cast<size_t>(i)] = cur.body.segment;
        r.robots[static_cast<size_t>(r.robotCount++)] = view;

        if (cur.attachmentOn) {
            RenderState::AttachmentView part;
            part.position = hadAttachment
                                ? glm::mix(prev.attachmentOrigin, cur.attachmentOrigin, t)
                                : cur.attachmentOrigin;
            part.orientation = view.orientation;
            part.type = cur.type;
            part.segment = cur.body.segment;
            part.key = i;
            part.disabledTime = view.disabledTime;
            r.attachments[static_cast<size_t>(r.attachmentCount++)] = part;
        }
    }

    for (int i = 0; i < sim::kMaxRobots; ++i) {
        const sim::LooseAttachment& cur = b.robots.looseAttachments[static_cast<size_t>(i)];
        if (!cur.alive) continue;
        const sim::LooseAttachment& prev = a.robots.looseAttachments[static_cast<size_t>(i)];
        RenderState::AttachmentView part;
        part.position = prev.alive ? glm::mix(prev.body.position, cur.body.position, t)
                                   : cur.body.position;
        part.orientation = prev.alive ? glm::slerp(prev.orientation, cur.orientation, t)
                                      : cur.orientation;
        part.type = cur.type;
        part.segment = cur.body.segment;
        part.key = sim::kMaxRobots + i;
        r.attachments[static_cast<size_t>(r.attachmentCount++)] = part;
    }

    // Debris rides the bolt list: small emissive things the unlit pass draws
    // identically. Drops do not; they are pickups, and frame_prepare draws them
    // as pickups.
    for (const sim::Debris& d : b.robots.debris) {
        if (!d.alive || r.boltCount >= static_cast<int>(sim::kMaxProjectiles)) continue;
        RenderState::Bolt bolt;
        bolt.position = d.body.position;
        bolt.color = d.color * std::max(0.0f, 1.0f - d.age / d.lifetime);
        bolt.size = 0.45f;
        r.bolts[static_cast<size_t>(r.boltCount++)] = bolt;
    }

    r.doorCount = static_cast<int>(sim::kMaxDoors);
    for (int i = 0; i < r.doorCount; ++i) {
        r.doorOpenness[static_cast<size_t>(i)] =
            glm::mix(a.game.doors[static_cast<size_t>(i)].openness,
                     b.game.doors[static_cast<size_t>(i)].openness, t);
    }
    // Not interpolated: picking a key up happens on one step, and half a key
    // is not a thing a door can be told about.
    for (size_t i = 0; i < r.keys.size() && i < b.game.keys.size(); ++i) {
        r.keys[i] = b.game.keys[i];
    }
    r.exitOpen = b.game.exitOpen;

    for (int i = 0; i < b.props.floatingCount; ++i) {
        const sim::FloatingLight& ball = b.props.floating[static_cast<size_t>(i)];
        if (!ball.alive) continue;
        const sim::FloatingLight& was = a.props.floating[static_cast<size_t>(i)];
        RenderState::PropView view;
        view.position = was.alive ? glm::mix(was.body.position, ball.body.position, t)
                                  : ball.body.position;
        view.color = ball.color;
        view.size = ball.body.radius * 1.5f;
        r.props[static_cast<size_t>(r.propCount++)] = view;
    }
    for (int i = 0; i < b.props.swingingCount; ++i) {
        const sim::SwingingLight& lamp = b.props.swinging[static_cast<size_t>(i)];
        if (!lamp.alive) continue;
        const sim::SwingingLight& was = a.props.swinging[static_cast<size_t>(i)];
        RenderState::PropView view;
        view.position = was.alive ? glm::mix(was.position, lamp.position, t) : lamp.position;
        view.color = lamp.color;
        view.size = 0.9f;
        view.corded = true;
        view.anchor = lamp.anchor;
        r.props[static_cast<size_t>(r.propCount++)] = view;
    }

    r.shipDisabled = b.ship.disabled;
    r.shipDisabledTime = b.game.elapsed;
    r.elapsed = b.game.elapsed;
    r.objectCount = b.combat.objects.count;
    for (int i = 0; i < r.objectCount; ++i) {
        r.objectAlive[static_cast<size_t>(i)] = b.combat.objects.alive[static_cast<size_t>(i)];
    }

    return r;
}

} // namespace dvk

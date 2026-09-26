#include "render/frame_prepare.h"

#include "audio/audio.h"
#include "game/level.h"
#include "render/sprites.h"
#include "sim/combat.h"
#include "sim/flight.h"
#include "sim/lighting.h"
#include "sim/robots.h"
#include "sim/weapons.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace dvk {
namespace {

constexpr float kEngineSilentBelow = 0.5f;
constexpr float kEngineGain = 0.1f;
constexpr float kEngineLowPitch = 0.72f;
constexpr float kEngineHighPitch = 1.28f;

// The headlight is the player's primary way of seeing the mine: broad, soft,
// and slightly ahead of the hull so the cone does not begin inside the ship.
// Unlimited range: the flood fill follows every open portal, and in the
// radius-relative falloff t = dist/radius is zero everywhere.
constexpr float kHeadlightRadius = std::numeric_limits<float>::infinity();
constexpr float kHeadlightCone = 60.0f;
constexpr float kHeadlightIntensity = 0.5f;
constexpr float kHeadlightForward = 2.2f;

// Keep the chase-camera reticle stable near walls and bounded in open space.
constexpr float kAimNearDistance = 12.0f;
constexpr float kAimFarDistance = kDefaultAimDistance;

// How much clear air is left between the chase camera's eye and the wall it
// was stopped by.
//
// A ray and a pad rather than a sphere sweep. kZNear is 0.05, so even at the
// widest FOV the setting allows the near plane's far corner sits under 0.2
// units from the eye: padding a point hit by more than that is geometrically
// indistinguishable from sweeping the frustum itself. And a ray cannot grind
// to a halt the way conservative advancement does when it runs nearly tangent
// to a surface - which is exactly what an up-and-back chase direction does
// under a low ceiling, where sweepSphere would exhaust its step budget and
// report no hit at all. Failing open there means the camera in the ceiling,
// which is the bug this exists to fix.
constexpr float kChaseWallPad = 0.25f;

// Coming back out is eased; going in is not.
//
// The asymmetry is the whole point. A camera that eases INTO its new distance
// is a camera inside the wall for the length of the ease, which is the same
// bug, only briefer. Nothing is in the way on the way out, so that half can
// take its time and does.
constexpr float kChaseReturnRate = 5.0f;   // 1/s

// The ship stops being drawn once the eye is this close to it.
//
// At three units the hull is a little over a metre ahead of the near plane and
// still reads as a ship; any closer and the camera is inside the mesh, looking
// at culled backfaces through a near plane that has sliced the fuselage open.
// First person does not draw the ship for exactly this reason.
//
// A band rather than a single cut, because the resolved distance eases back
// out after every snap: a ship grinding along a wall that held the camera near
// one threshold would blink on and off several times a second.
constexpr float kChaseShipHide = 3.0f;
constexpr float kChaseShipShow = 3.6f;

// How a pickup of `kind` is drawn: its sprite, its tint and its size. Shared
// by the level's own pickups and the ones robots drop, so a shield is a shield
// whoever left it there.
void dressPickup(RenderState::PickupView& view, PickupKind kind, float pulse) {
    // A kind with hand-drawn art names its sprite layer and tints it;
    // the rest keep the colour-only glow and leave `sprite` at -1,
    // which is flares alone now that everything else has been drawn.
    // The drawn sprites carry their own colour, so white is what shows
    // them as they were painted - the pulse still rides on top. The key
    // art is a white template, so there the tint is the whole point and
    // the three keys differ by nothing else.
    view.sprite = -1;
    switch (kind) {
        case PickupKind::Shield:
            view.sprite = gl::SpriteHealth;
            view.color = glm::vec3(1.0f);
            break;
        case PickupKind::Energy:
            view.sprite = gl::SpriteEnergy;
            view.color = glm::vec3(1.0f);
            break;
        case PickupKind::LaserUpgrade:
            view.sprite = gl::SpriteLaser;
            view.color = glm::vec3(1.0f);
            break;
        case PickupKind::Concussion:
            view.sprite = gl::SpriteConcussion;
            view.color = glm::vec3(1.0f);
            break;
        case PickupKind::Homing:
            view.sprite = gl::SpriteHoming;
            view.color = glm::vec3(1.0f);
            break;
        case PickupKind::Flares: view.color = glm::vec3(1.0f, 0.8f, 0.4f); break;
        case PickupKind::RailgunAmmo:
            view.sprite = gl::SpriteRailgun;
            view.color = glm::vec3(1.0f);
            break;
        case PickupKind::LightningAmmo:
            view.sprite = gl::SpriteLightning;
            view.color = glm::vec3(1.0f);
            break;
        case PickupKind::VulcanAmmo:
            view.sprite = gl::SpriteVulcan;
            view.color = glm::vec3(1.0f);
            break;
        case PickupKind::KeyBlue:
            view.sprite = gl::SpriteKey;
            view.color = glm::vec3(0.35f, 0.55f, 1.0f);
            break;
        case PickupKind::KeyYellow:
            view.sprite = gl::SpriteKey;
            view.color = glm::vec3(1.0f, 0.9f, 0.3f);
            break;
        case PickupKind::KeyPurple:
            view.sprite = gl::SpriteKey;
            view.color = glm::vec3(0.75f, 0.4f, 1.0f);
            break;
        case PickupKind::Count: break;
    }
    view.color *= pulse;
    // A sprite is drawn larger than a glow of the same weight: the art
    // is padded inside its square, so the shape itself covers rather
    // less of the quad than the falloff does.
    view.size = kind >= PickupKind::KeyBlue ? 1.6f
                : view.sprite >= 0              ? 1.3f
                                                : 0.85f;
}

} // namespace

PreparedFrame prepareRenderFrame(FramePrepareContext& ctx) {
    const SimState& prev = ctx.previous;
    SimState& cur = ctx.current;
    const float alpha = ctx.alpha;
    const CameraMode camera = ctx.camera;
    const PlayerSettings& settings = ctx.settings;
    const double nowSeconds = ctx.nowSeconds;
    const double elapsed = ctx.frameSeconds;
    audio::Audio& audio = ctx.audio;
    const sim::EventQueue& soundEvents = ctx.soundEvents;
    Level& level = ctx.level;
    const sim::FlightConfig& flight = ctx.flight;
    const sim::WeaponConfig& weapons = ctx.weapons;
    const sim::RobotConfig& robotConfig = ctx.robots;
    LightFlood& flood = ctx.flood;
    int& lightDoorSignature = ctx.lightDoorSignature;
    std::vector<Light>& dynamicLights = ctx.dynamicLights;
    const sim::SolidDoors& solidDoors = ctx.solidDoors;
    const sim::OccluderSet& occluders = ctx.occluders;
    sim::Illumination& playerLight = ctx.playerLight;
    sim::PlayerVisibility& playerSeen = ctx.playerSeen;
    float& aimDistance = ctx.aimDistance;
    ChaseCamera& chase = ctx.chase;

        dvk::RenderState rs = dvk::interpolate(prev, cur, alpha);
        rs.camera = camera;
        rs.verticalFov = camera == dvk::CameraMode::FirstPerson ? settings.firstPersonFov
                                                                : settings.thirdPersonFov;

        // The mine coming apart. Driven from wall-clock rather than the sim, so
        // the shake is smooth at any frame rate and never accumulates into the
        // ship's actual position.
        // A missile leaving the tube shoves the view back, and only the view:
        // the ship's own motion is the simulation's business and a camera
        // effect must not reach into it. Squared so it snaps and then eases,
        // which reads as a shove rather than a wobble.
        if (cur.combat.player.recoilTimer > 0.0f) {
            const float k = cur.combat.player.recoilTimer / dvk::sim::kRecoilTime;
            const glm::vec3 back =
                glm::mat3_cast(rs.shipOrientation) * glm::vec3(0.0f, 0.0f, 1.0f);
            rs.shakeOffset += back * (0.55f * k * k);
        }

        if (cur.game.shake > 0.0f) {
            const auto t = static_cast<float>(nowSeconds);
            const float amp = cur.game.shake * 0.9f;
            rs.shakeOffset = glm::vec3(std::sin(t * 41.0f) + std::sin(t * 17.0f) * 0.6f,
                                       std::sin(t * 37.0f + 1.7f) + std::sin(t * 23.0f) * 0.5f,
                                       std::sin(t * 29.0f + 0.4f)) * amp * 0.5f;
            // A flicker that dips rather than brightens: the lights are failing.
            const float flicker = 0.72f + 0.28f * std::sin(t * 26.0f) * std::sin(t * 7.3f);
            rs.staticLightScale = 1.0f - cur.game.shake * (1.0f - flicker);
        }

        // --- the chase camera against the mine ---------------------------
        //
        // The eye wants to sit at kChaseOffset in the ship's own space. Where
        // the mine is in the way it is pulled in along that same line - all the
        // way to the ship's own position if it has to, which is the one point
        // on the line that is guaranteed clear, because the ship's own
        // collision is what keeps it clear.
        //
        // Pulling in along the line rather than sliding along the wall: the
        // line always has an answer and the answer is continuous, where sliding
        // has to pick a direction at a corner and picks a different one the
        // next frame.
        rs.chaseDistance = 0.0f;
        rs.drawShip = false;
        if (camera == dvk::CameraMode::Chase) {
            const glm::vec3 ideal = glm::mat3_cast(rs.shipOrientation) * dvk::kChaseOffset;
            const float reach = glm::length(ideal);
            const dvk::sim::RayHit back =
                dvk::sim::raycast(level, rs.shipPos, ideal, reach, rs.shipSegment, &solidDoors);

            // The shake is added to the eye by the renderer, downstream of
            // everything here, and would walk a pinned camera straight back
            // through the wall it was just pulled out of. So it is paid for
            // here, out of the same budget, rather than being allowed to escape
            // the clamp.
            const float target =
                std::clamp((back.hit ? back.distance : reach) - kChaseWallPad -
                               glm::length(rs.shakeOffset),
                           0.0f, reach);

            float& held = chase.distance;
            if (target < held) {
                held = target;
            } else {
                held += (target - held) *
                        (1.0f - std::exp(-kChaseReturnRate * static_cast<float>(elapsed)));
            }

            chase.shipDrawn = held >= (chase.shipDrawn ? kChaseShipHide : kChaseShipShow);
            rs.chaseDistance = held;
            rs.drawShip = chase.shipDrawn;
        }

        // --- sound -------------------------------------------------------
        //
        // The listener is the ship, not the camera. In chase view the camera
        // sits several units behind the hull, and hearing the mine from back
        // there would put the player's own guns slightly in front of them. The
        // chase camera is a view of the ship; it is not where the pilot is.
        //
        // It is set before the events are submitted so that this frame's sounds
        // are positioned against this frame's listener rather than the last
        // one's - at 200 units/s that is the difference between a bolt sounding
        // ahead of the ship and behind it.
        audio.setListener(rs.shipPos, rs.shipOrientation);
        audio.submit(soundEvents);

        // The drive. Gained and pitched by speed rather than by the throttle
        // key, so a ship coasting with the throttle released still sounds like
        // it is moving, which is what a player watching the walls go past
        // expects to hear.
        //
        // k is measured from kEngineSilentBelow rather than from zero, so the
        // drive fades in from nothing as the ship starts moving instead of
        // snapping on at some audible level. A gain of zero stops the channel;
        // because the ramp starts at zero, the restart when the ship moves off
        // again happens at silence and cannot be heard.
        {
            const float top = std::max(kEngineSilentBelow + 1.0f, flight.maxSpeed);
            const float k = std::clamp((rs.speed - kEngineSilentBelow) /
                                           (top - kEngineSilentBelow),
                                       0.0f, 1.0f);
            audio.setAmbient(dvk::audio::Ambient::Engine, kEngineGain * k,
                             kEngineLowPitch + (kEngineHighPitch - kEngineLowPitch) * k);
        }
        // The klaxon runs for exactly as long as the countdown does. Gained by
        // how far the mine has come apart, so it arrives under the first
        // explosion rather than on top of it.
        audio.setAmbient(dvk::audio::Ambient::Alarm,
                         cur.game.phase == dvk::sim::GamePhase::Countdown
                             ? 0.1f + 0.2f * cur.game.shake
                             : 0.0f);

        rs.pickupCount = static_cast<int>(
            std::min(level.pickups.size(), static_cast<size_t>(dvk::sim::kMaxPickups)));
        const auto pulseAt = [&](size_t phase) {
            return 0.85f + 0.15f * std::sin(static_cast<float>(nowSeconds) * 3.0f +
                                            static_cast<float>(phase));
        };
        for (int i = 0; i < rs.pickupCount; ++i) {
            const dvk::Pickup& pickup = level.pickups[static_cast<size_t>(i)];
            dvk::RenderState::PickupView& view = rs.pickups[static_cast<size_t>(i)];
            view.present = !cur.game.collected[static_cast<size_t>(i)];
            view.position = pickup.position;
            dressPickup(view, pickup.kind, pulseAt(static_cast<size_t>(i)));
        }
        // Then whatever the robots have dropped. A drop does not move, so there
        // is nothing to interpolate; its phase is keyed to its slot, which does
        // not change while it lies there.
        for (size_t di = 0; di < cur.robots.drops.size(); ++di) {
            const dvk::sim::Drop& drop = cur.robots.drops[di];
            if (!drop.alive) continue;
            dvk::RenderState::PickupView& view =
                rs.pickups[static_cast<size_t>(rs.pickupCount++)];
            view.present = true;
            view.position = drop.position;
            dressPickup(view,
                        drop.kind == dvk::sim::DropKind::Energy ? dvk::PickupKind::Energy
                                                                : dvk::PickupKind::Shield,
                        pulseAt(di));
        }

        // Every light that is not baked into the level, rebuilt each frame.
        // The vector keeps its capacity and each Light keeps its segment list's
        // capacity, so after the first few frames this allocates nothing.
        //
        // The headlight is always index 0: the renderer uses that to know which
        // light the ship must never shadow.
        const glm::mat3 shipBasis = glm::mat3_cast(rs.shipOrientation);
        const glm::vec3 shipForward = shipBasis * glm::vec3(0.0f, 0.0f, -1.0f);

        size_t dynamicUsed = 0;
        const auto nextLight = [&]() -> dvk::Light& {
            if (dynamicUsed == dynamicLights.size()) dynamicLights.emplace_back();
            return dynamicLights[dynamicUsed++];
        };

        {
            dvk::Light& hl = nextLight();
            hl.type = dvk::LightType::Spot;
            hl.radius = kHeadlightRadius;
            hl.coneDegrees = kHeadlightCone;
            hl.color = glm::vec3(1.0f, 0.97f, 0.9f);
            hl.intensity = cur.combat.player.headlightOn ? kHeadlightIntensity : 0.0f;
            hl.castsShadows = cur.combat.player.headlightOn;
            hl.position = rs.shipPos + shipForward * kHeadlightForward;
            hl.direction = shipForward;
            hl.segment = rs.shipSegment;
            hl.segments.clear();
            if (cur.combat.player.headlightOn) {
                dvk::rebuildLightSegments(level, hl, flood, &cur.game.doorField);
            }
        }

        if (cur.game.doorField.opaqueCount() != lightDoorSignature) {
            lightDoorSignature = cur.game.doorField.opaqueCount();
            dvk::rebuildAllLightSegments(level, flood, &cur.game.doorField);
        }

        // Every bolt in flight is a moving light. A laser going down a corridor
        // sweeping shadows off everything in it is the single most cinematic
        // thing the technique buys, and it costs one flood fill per bolt over a
        // radius small enough that the fill stops almost immediately.
        const auto addPointLight = [&](const glm::vec3& position, int segment,
                                       const glm::vec3& color, float radius, float intensity) {
            if (radius <= 0.0f || intensity <= 0.0f) return;
            dvk::Light& l = nextLight();
            l.type = dvk::LightType::Point;
            l.position = position;
            l.segment = segment;
            l.color = color;
            l.intensity = intensity;
            l.radius = radius;
            l.castsShadows = true;
            l.segments.clear();
            dvk::rebuildLightSegments(level, l, flood, &cur.game.doorField);
        };

        // The floating balls and the swinging lamps. Both move every frame, so
        // both are dynamic lights with their segment sets rebuilt each frame -
        // the same path the bolts take, which is what makes a swinging spot
        // sweep real stencil shadows across the room it hangs in.
        for (int i = 0; i < cur.props.floatingCount; ++i) {
            const dvk::sim::FloatingLight& ball = cur.props.floating[static_cast<size_t>(i)];
            if (!ball.alive) continue;
            addPointLight(ball.body.position, ball.body.segment, ball.color, ball.lightRadius,
                          ball.lightIntensity);
        }
        for (int i = 0; i < cur.props.swingingCount; ++i) {
            const dvk::sim::SwingingLight& lamp = cur.props.swinging[static_cast<size_t>(i)];
            if (!lamp.alive) continue;
            if (lamp.lightIntensity <= 0.0f || lamp.lightRadius <= 0.0f) continue;
            dvk::Light& l = nextLight();
            l.type = dvk::LightType::Spot;
            l.position = lamp.position;
            l.direction = lamp.direction;
            l.coneDegrees = lamp.coneDegrees;
            l.segment = lamp.segment;
            l.color = lamp.color;
            l.intensity = lamp.lightIntensity;
            l.radius = lamp.lightRadius;
            l.castsShadows = true;
            l.segments.clear();
            dvk::rebuildLightSegments(level, l, flood, &cur.game.doorField);
        }

        for (const dvk::sim::Projectile& p : cur.combat.projectiles.items) {
            if (!p.alive) continue;
            addPointLight(p.position, p.segment, p.color, p.lightRadius, p.lightIntensity);
        }
        for (const dvk::sim::Flare& f : cur.combat.flares.items) {
            if (!f.alive) continue;
            // Fades over the last few seconds rather than snapping out.
            const float remaining = f.lifetime - f.age;
            const float fade = std::clamp(remaining / weapons.flareFade, 0.0f, 1.0f);
            addPointLight(f.position, f.segment, weapons.flareColor, weapons.flareRadius,
                          weapons.flareIntensity * fade);
        }
        // A shoulder railgun's charge: a light at the muzzle that grows with
        // it, from nothing to the type's full radius and intensity, so the
        // corridor brightens for five seconds before the shot.
        for (int i = 0; i < cur.robots.count; ++i) {
            const dvk::sim::Robot& robot = cur.robots.robots[static_cast<size_t>(i)];
            if (!robot.alive || !robot.gunAttached || !robot.charging) continue;
            if (robot.type < 0 || robot.type >= static_cast<int>(robotConfig.types.size())) {
                continue;
            }
            const dvk::sim::RobotDef& def = robotConfig.types[static_cast<size_t>(robot.type)];
            const float k = def.railgunChargeTime > 0.0f
                                ? std::clamp(robot.chargeTime / def.railgunChargeTime, 0.0f, 1.0f)
                                : 1.0f;
            addPointLight(robot.muzzle, robot.body.segment, def.railgunColor,
                          def.railgunLightRadius * k, def.railgunLightIntensity * k);
        }
        for (const dvk::sim::Effect& e : cur.combat.effects.items) {
            if (!e.alive || e.lightIntensity <= 0.0f) continue;
            const float k = e.lifetime > 0.0f ? e.age / e.lifetime : 1.0f;
            addPointLight(e.position, e.segment, e.color, e.lightRadius,
                          e.lightIntensity * std::max(0.0f, 1.0f - k));
        }

        dynamicLights.resize(dynamicUsed);

        // How much light is actually falling on the player, as the simulation
        // understands it rather than as the screen shows it. Asked once per
        // frame, after both light lists exist, over the ship's collision sphere
        // so the answer does not flicker as a shadow edge crosses a point.
        // Nothing consumes it yet beyond the debug readout; it is here because
        // "is the player in darkness" is a question about the world that the
        // world should be able to answer.
        dvk::sim::IlluminationQuery lightQuery;
        lightQuery.point = rs.shipPos;
        lightQuery.segment = rs.shipSegment;
        lightQuery.bodyRadius = cur.ship.body.radius;
        // Same exclusion as playerVisibility: this is a query about the ship,
        // sampled on the ship, so the ship's own proxy has to be left out.
        lightQuery.owner = dvk::sim::OccluderId{dvk::sim::OccluderKind::Ship, -1};
        playerLight = dvk::sim::Illumination{};
        dvk::sim::addIllumination(playerLight, lightQuery, level, level.lights, &solidDoors,
                                  &occluders);
        dvk::sim::addIllumination(playerLight, lightQuery, level, dynamicLights, &solidDoors,
                                  &occluders);

        // And the conclusion the robots draw from it, which is a different
        // question with a different answer: a point rather than the hull, and
        // true whatever the light if the player is carrying a headlight. Asked
        // here rather than inside the debug panel so the HUD and the panel read
        // one answer instead of two, and so it is still asked with the panel
        // shut.
        playerSeen = dvk::sim::playerVisibility(level, cur.combat, weapons, rs.shipPos,
                                                rs.shipSegment, &solidDoors, &cur.props,
                                                &occluders);

        // A ray straight out of the nose, for the debug overlay and as a live
        // check that the query everything from M5 on depends on agrees with
        // what is on screen.
        const dvk::sim::RayHit noseRay =
            dvk::sim::raycast(level, rs.shipPos, shipForward, 300.0f, rs.shipSegment);

        // The same ray says where the reticle belongs. The guns fire from the
        // ship along this line, so the point the reticle should cover is the
        // first thing the line meets - or a point far along it when it meets
        // nothing, which is the honest answer to "where would this shot go".
        //
        // Chased rather than set: crossing a corridor mouth steps the hit from
        // a near wall to a far one in a single frame, and in the chase camera
        // that is a reticle jumping across the screen. In first person none of
        // this shows, because the camera sits on the firing line and the whole
        // line projects to one pixel whatever the distance.
        const float aimTarget =
            std::clamp(noseRay.hit ? noseRay.distance : kAimFarDistance,
                       kAimNearDistance, kAimFarDistance);
        aimDistance += (aimTarget - aimDistance) *
                       std::min(1.0f, static_cast<float>(elapsed) * 12.0f);

    return {std::move(rs), shipForward, noseRay};
}

} // namespace dvk

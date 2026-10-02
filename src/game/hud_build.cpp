#include "game/hud_build.h"

#include "audio/audio.h"
#include "game/hud_slots.h"
#include "game/music.h"
#include "game/script.h"
#include "game/state.h"
#include "sim/combat.h"
#include "sim/gameflow.h"
#include "sim/lighting.h"
#include "sim/robots.h"
#include "sim/weapons.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace dvk {

namespace {

// The target panel: what the last thing the player shot is, and how it is
// doing. Left alone when nothing has been hit recently, which is what keeps the
// panel off screen rather than showing a stale robot.
void fillTarget(const SimState& state, const sim::RobotConfig& robots, gl::HudData& hud) {
    const int slot = state.combat.player.lastTargetRobot;
    if (state.combat.player.targetHudTime <= 0.0f || slot < 0 || slot >= state.robots.count) {
        return;
    }
    const sim::Robot& target = state.robots.robots[static_cast<size_t>(slot)];
    if (!target.alive || target.type < 0 ||
        static_cast<size_t>(target.type) >= robots.types.size()) {
        return;
    }
    const sim::RobotDef& def = robots.types[static_cast<size_t>(target.type)];
    hud.hasTarget = true;
    hud.targetDisabled = target.mode == sim::RobotMode::Disabled;
    hud.targetName = def.name.c_str();
    hud.targetStatus = hud.targetDisabled
                           ? "engines disabled"
                           : sim::kRobotModeNames[static_cast<size_t>(target.mode)];
    hud.targetHealth01 = def.health > 0.0f ? target.health / def.health : 0.0f;
    hud.targetEnergy01 = def.energy > 0.0f ? target.energy / def.energy : 1.0f;
    hud.targetFade = std::min(1.0f, state.combat.player.targetHudTime / 0.5f);
}

// The lead marker: the point the reticle has to cover for a shot from the
// selected gun to meet the target robot, if both hold their velocities. Called
// after fillTarget, and only ever marks the robot that panel is showing.
//
// A gun that inherits the ship's velocity sees the robot moving at the
// difference of the two; a missile that does not sees the robot's own. With
// r the robot relative to the ship, u that velocity and s the shot's speed,
// the shot meets the robot at the first t > 0 with |r + u t| = s t, and the
// point to aim at is the robot advanced by u t.
//
// Measured from the ship's centre rather than from the wingtip the next bolt
// leaves, which alternates; and in the chase camera the guns converge on what
// the eye ray hits behind the marker rather than on the marker itself. Both
// are small at the ranges the guns are used at.
void fillLead(const HudBuildContext& ctx, gl::HudData& hud) {
    if (!ctx.leadMarker || !ctx.aimVisible || !hud.hasTarget) return;
    const SimState& state = ctx.state;
    const int weaponId = state.combat.player.primaryWeapon;
    if (weaponId < 0 || weaponId >= sim::WeaponCount) return;
    // The traced weapons arrive the instant they are fired: the robot is the
    // mark, and the reticle already says so.
    if (sim::weaponFire(weaponId) != sim::WeaponFire::Projectile) return;
    const sim::WeaponDef& def = ctx.weapons.weapons[static_cast<size_t>(weaponId)];
    const float s = def.speed;
    if (s <= 0.0f) return;

    // Drawn over the interpolated robot, so the marker moves with the hull on
    // screen rather than a step ahead of it.
    const int slot = state.combat.player.lastTargetRobot;
    const RenderState::RobotView* view = nullptr;
    for (int i = 0; i < ctx.render.robotCount; ++i) {
        if (ctx.render.robots[static_cast<size_t>(i)].slot == slot) {
            view = &ctx.render.robots[static_cast<size_t>(i)];
            break;
        }
    }
    if (!view) return;

    glm::vec3 u = state.robots.robots[static_cast<size_t>(slot)].body.motion();
    if (sim::weaponInheritsVelocity(weaponId)) u -= state.ship.body.velocity;
    const glm::vec3 r = view->position - ctx.render.shipPos;

    // (u.u - s^2) t^2 + 2 (r.u) t + r.r = 0
    const float a = glm::dot(u, u) - s * s;
    const float b = 2.0f * glm::dot(r, u);
    const float c = glm::dot(r, r);
    float t = -1.0f;
    if (std::abs(a) < 1e-4f) {
        // The robot is pulling away exactly as fast as the shot flies: one
        // root, and only if it is closing at all.
        if (b < 0.0f) t = -c / b;
    } else {
        const float disc = b * b - 4.0f * a * c;
        if (disc < 0.0f) return;   // it is outrunning the shot
        const float root = std::sqrt(disc);
        const float t0 = (-b - root) / (2.0f * a);
        const float t1 = (-b + root) / (2.0f * a);
        const float lo = std::min(t0, t1);
        const float hi = std::max(t0, t1);
        t = lo > 0.0f ? lo : hi;
    }
    // A shot that expires on the way has nothing to lead.
    if (!(t > 0.0f) || t > def.lifetime) return;

    hud.hasLead = true;
    hud.leadPoint = view->position + u * t;
}

void fillMusic(const HudBuildContext& ctx, HudScratch& scratch, gl::HudData& hud) {
    // Read off the crossfade rather than off the director's target: for the two
    // seconds between them the target has already changed and both tracks are
    // still audible, which is exactly what the middle state of the readout is
    // for.
    const MusicReadout readout =
        musicReadout(ctx.audio.musicMix(), ctx.audio.musicPlaying(),
                     ctx.audio.musicTrackName(0).c_str(), ctx.audio.musicTrackName(1).c_str());
    formatInputBinding(ctx.bindings[PlayerAction::NextTrack], scratch.musicNextKey,
                       sizeof(scratch.musicNextKey));
    hud.musicShow = readout.show;
    hud.musicTransitioning = readout.transitioning;
    hud.musicTrack = readout.text;
    hud.musicNextKey = scratch.musicNextKey;
    hud.musicTime = ctx.musicTime;
}

} // namespace

gl::HudData buildHud(const HudBuildContext& ctx, HudScratch& scratch) {
    const SimState& state = ctx.state;
    const sim::PlayerState& player = state.combat.player;

    gl::HudData hud;
    hud.show = ctx.show;
    hud.shields = player.shields;
    hud.energy = player.energy;
    hud.flares = player.flares;
    fillWeaponSlots(player, ctx.weapons, ctx.bindings, ctx.simStep, hud);
    for (int k = 0; k < 3; ++k) hud.keys[k] = state.game.keys[static_cast<size_t>(k)];
    hud.countdown =
        state.game.phase == sim::GamePhase::Countdown ? state.game.countdown : -1.0f;
    hud.phase = static_cast<int>(state.game.phase);
    hud.message = state.game.endReason;
    hud.elapsed = state.game.elapsed;
    hud.robotsDestroyed = state.game.robotsDestroyed;
    hud.robotsTotal = state.game.robotsAtStart;

    fillTarget(state, ctx.robots, hud);

    hud.commActive = ctx.comm.visible;
    hud.commFrame = ctx.comm.frame;
    hud.commAlpha = ctx.comm.alpha;
    hud.commClip = ctx.commClip;
    hud.commSpeaker = "incoming transmission";
    hud.commText = "";
    if (ctx.commMessage) {
        hud.commSpeaker = ctx.commMessage->speaker.c_str();
        hud.commText = ctx.commMessage->text.c_str();
    }

    fillMusic(ctx, scratch, hud);

    hud.damageFlash = player.hurtTimer / sim::kHurtFlashTime;
    // Only the scene draws a firing line; the automap has its own camera.
    hud.aimPoint = ctx.aimPoint;
    hud.hasAimPoint = ctx.aimVisible;
    hud.aimOnBreakable = ctx.aimOnBreakable;
    fillLead(ctx, hud);
    hud.shipDisabled = state.ship.disabled;
    hud.shipDisabledTime = state.game.elapsed;
    hud.energyDepleted = sim::playerEnergyDepleted(player);

    // Every robot missile that is coming for the ship, and the nearest of
    // them: that is the one the player has to deal with first, whether by
    // shooting it or by getting out of its way.
    float nearest = std::numeric_limits<float>::max();
    for (const sim::Projectile& p : state.combat.projectiles.items) {
        if (!p.alive || p.owner < 0 || !p.targetPlayer) continue;
        ++hud.incomingMissiles;
        const float d = glm::length(p.position - ctx.render.shipPos);
        if (d < nearest) {
            nearest = d;
            hud.nearestMissile = p.position;
        }
    }
    hud.nearestMissileDistance = hud.incomingMissiles > 0 ? nearest : 0.0f;
    hud.missileMarker = hud.incomingMissiles > 0 && ctx.aimVisible;

    // Both the bar and the word come from `seen` rather than from the
    // illumination query, even though the latter is the better-behaved number.
    // They have to agree: a bar sitting under its own threshold tick beside the
    // word "exposed" reads as a bug, and the two queries genuinely can disagree
    // - one samples the hull and the other a point. The one worth showing is
    // the one the robots act on.
    hud.illumination = ctx.seen.illumination;
    hud.darkThreshold = sim::kDarkThreshold;
    hud.seen = ctx.seen.visible;
    // The headlight is what gave them away only when nothing else had: exactly
    // the case where `visible` would have been false without it.
    hud.seenByHeadlight = ctx.seen.headlight && ctx.seen.illumination < sim::kDarkThreshold;

    return hud;
}

} // namespace dvk

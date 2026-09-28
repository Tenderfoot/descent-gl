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
    hud.aimPoint = ctx.render.shipPos + ctx.shipForward * ctx.aimDistance;
    hud.hasAimPoint = ctx.aimVisible;
    hud.aimOnBreakable = ctx.aimOnBreakable;
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

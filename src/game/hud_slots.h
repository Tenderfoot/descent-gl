#pragma once

#include "game/input_actions.h"
#include "render/renderer.h"

namespace dvk {

namespace sim {
struct PlayerState;
struct WeaponConfig;
}

// Fills the HUD's weapon strip from the player's state and their current
// bindings.
//
// This lives in the game layer because it is the only one that can write it.
// The row's order is the order the keys are in, which `src/game/input_actions.h`
// owns, and the missiles' order is the one their key cycles, which
// `src/sim/weapons.h` owns; neither is the WeaponId enum's order, and the
// renderer is not allowed to know any of the three.
//
// `step` is the simulation's fixed step, needed because a continuous weapon
// spends energy per second and its cell has to say whether there is enough for
// one step of it.
//
// Allocates nothing: every string it writes is either static or goes into a
// buffer the slot already owns.
void fillWeaponSlots(const sim::PlayerState& player, const sim::WeaponConfig& cfg,
                     const InputBindings& bindings, float step, gl::HudData& hud);

}

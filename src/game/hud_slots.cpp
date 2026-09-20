#include "game/hud_slots.h"

#include "sim/combat.h"
#include "sim/weapons.h"

namespace dvk {
namespace {

void fillSlot(gl::HudWeaponSlot& slot, sim::WeaponId weapon, PlayerAction key,
              const sim::PlayerState& player, const sim::WeaponConfig& cfg,
              const InputBindings& bindings, float step, bool selected) {
    formatInputBinding(bindings[key], slot.key, sizeof(slot.key));

    // Every gun is carried from the first frame, so this is the answer for now
    // and the whole of what a change to that would have to rewrite.
    slot.available = true;

    // Named whether or not it is carried: the strip sizes the cell from the
    // name, so a mount that lost its name would collapse and take the rest of
    // the row sideways with it. `available` is what decides whether the name is
    // drawn.
    slot.name = sim::kWeaponShortNames[static_cast<size_t>(weapon)];
    const int* ammo = sim::weaponAmmoPool(player, weapon);
    slot.ammo = ammo ? *ammo : -1;
    slot.level = weapon == sim::WeaponLaser ? player.laserLevel : 0;
    slot.selected = selected;
    slot.usable = slot.available && sim::weaponReady(player, cfg, weapon, step);
}

}

void fillWeaponSlots(const sim::PlayerState& player, const sim::WeaponConfig& cfg,
                     const InputBindings& bindings, float step, gl::HudData& hud) {
    // The row is as long as the two lists that decide what goes in it. Asserted
    // rather than assumed: a fifth primary would otherwise be a gun with a key
    // and no cell to appear in, which is the kind of thing that is only noticed
    // in a screenshot.
    static_assert(kWeaponSelectActions.size() ==
                      static_cast<size_t>(gl::HudData::kPrimarySlots),
                  "the strip has one primary cell per weapon-select key");
    static_assert(sim::kSecondaryWeapons.size() ==
                      static_cast<size_t>(gl::HudData::kSecondarySlots),
                  "the strip has one missile cell per weapon in the cycle");

    for (size_t i = 0; i < kWeaponSelectActions.size(); ++i) {
        const WeaponSelectAction& select = kWeaponSelectActions[i];
        fillSlot(hud.primaries[i], select.weapon, select.action, player, cfg, bindings, step,
                 player.primaryWeapon == select.weapon);
    }

    // Both missiles report the key that cycles them, because both answer to it.
    // The strip says it once over the pair rather than on each cell; two cells
    // labelled with the same key read as a mistake.
    for (size_t i = 0; i < sim::kSecondaryWeapons.size(); ++i) {
        const sim::WeaponId weapon = sim::kSecondaryWeapons[i];
        fillSlot(hud.secondaries[i], weapon, PlayerAction::CycleSecondary, player, cfg, bindings,
                 step, player.secondaryWeapon == weapon);
    }

    formatInputBinding(bindings[PlayerAction::Flare], hud.flareKey, sizeof(hud.flareKey));
}

}

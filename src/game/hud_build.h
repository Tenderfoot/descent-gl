#pragma once

#include "game/comm_player.h"
#include "game/input_actions.h"
#include "render/renderer.h"

#include <glm/vec3.hpp>

namespace dvk {

struct RenderState;
struct ScriptMessage;
struct SimState;

namespace audio {
class Audio;
}

namespace sim {
struct PlayerVisibility;
struct RobotConfig;
struct WeaponConfig;
}

// Storage the finished HudData points into.
//
// `HudData` carries `const char*`, deliberately, so that naming a thing costs
// no allocation in the draw path. That only works if the characters outlive the
// frame, and the key a rebindable action answers to is the one string here with
// nowhere else to live: it is formatted fresh every frame because the player
// can rebind it in the settings menu without ever leaving the level. So the
// caller owns this and passes the same one each frame.
struct HudScratch {
    char musicNextKey[32] = {};
};

// Everything the HUD reads. Gathered here rather than in the renderer for the
// reason HudData exists at all: the renderer draws what it is handed and never
// reaches into simulation state.
struct HudBuildContext {
    const SimState& state;
    const RenderState& render;
    const sim::WeaponConfig& weapons;
    const sim::RobotConfig& robots;
    const InputBindings& bindings;
    // The simulation's fixed step. A continuous weapon spends energy per
    // second, so its cell needs to know how long one step is to say whether
    // there is enough for another.
    float simStep;

    // Whether the HUD is drawn at all. False under --no-ui, and on the run
    // summary, which is the menu's screen rather than the HUD's.
    bool show;

    glm::vec3 shipForward;
    float aimDistance;
    // The firing line ends on an attachment a selected weapon knocks loose.
    bool aimOnBreakable;
    // False on the automap, which has its own camera and no firing line on
    // screen to mark.
    bool aimVisible;

    const sim::PlayerVisibility& seen;

    CommFrame comm;
    int commClip;
    // Null when nothing scripted is on screen, which includes --comm's pinned
    // frame: it has pictures but no words and no speaker.
    const ScriptMessage* commMessage;

    const audio::Audio& audio;
    // The marquee's own clock, in seconds. Real time rather than simulation
    // time: the transports keep playing through a pause, so the scroll does
    // too.
    float musicTime;
};

gl::HudData buildHud(const HudBuildContext& ctx, HudScratch& scratch);

} // namespace dvk

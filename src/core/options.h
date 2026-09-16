#pragma once

#include <string>

namespace dvk {

// Command-line options. These exist so the app can be verified without a
// person at the keyboard:
//   --frames N          exit after N rendered frames
//   --screenshot PATH   write the final frame to PATH (PNG) before exiting
//   --resize-test       drive a scripted sequence of window resizes, an
//                       iconify and a restore, exercising the framebuffer
//                       callback and render-target rebuild
//   --level-select      open the main menu on the level list, so a screenshot can
//                       show it without navigating there.
//   --level-switch-test drive the level list to a second level, load it, play it and
//                       report whether the switch took. Needs a window.
//   --automap           start with the automap open (Tab at runtime), so a
//                       screenshot can show it without flying the level first.
//   --validate-assets   run every shipped model and level through the M2 and M3
//                       validators, print one line each, and exit. Needs no
//                       window and no GL.
//   --reveal            draw the automap as if every segment had been visited.
//                       A rendering check for the map itself, in the same family
//                       as --wireframe-volumes and --stencil-view: it changes
//                       nothing in the simulation and the game still only
//                       records the rooms actually flown through.
//   --chase             start in the chase camera (F1 at runtime), so a
//                       screenshot can show the ship without a keypress
//   --flight-test       run the flight model headlessly and report the
//                       response times the tuning is supposed to hit, then
//                       exit. Needs no window and no GL.
//   --world-test        load the level, validate it, and report portal
//                       visibility and light reach from every segment, then
//                       exit. Needs no window and no GL.
//   --segment N         start in segment N's centre instead of at the
//                       player start, so a screenshot can be taken anywhere
//                       in the mine without flying there
//   --collision-test    fly the ship into every wall from every direction at
//                       max speed and check it never escapes, cross-check the
//                       ray and sweep queries, and walk every portal both
//                       ways, then exit. Needs no window and no GL.
//   --wireframe-volumes start with shadow volumes drawn as wireframe (also an
//                       ImGui toggle), so the extrusion can be screenshotted
//                       rather than inferred from whether shadows look right
//   --stencil-view      start in the stencil visualiser (also an ImGui
//                       toggle): tints whatever the stencil marks as shadowed
//   --weapons-test      fire every weapon at walls and at targets and check
//                       what happened, then exit. Needs no window and no GL.
//   --robots-test       drive the robot state machine through idle, alert,
//                       chase, attack, retreat and death, and check they never
//                       leave the mine. Needs no window and no GL.
//   --headlight         start with the headlight on. It is off at the start
//                       from now on, so a screenshot can no longer show it
//                       without a keypress; this is the same gap --chase and
//                       --automap fill.
//   --light-test        drive the CPU illumination query: the falloff against
//                       the shader's own formula, the spot cone, whether
//                       occlusion blocks anything, and whether the level has
//                       both lit and dark places to stand. Needs no window and
//                       no GL.
//   --loop-test         play the level's win and lose conditions through:
//                       keys against doors, a shut door against light, the
//                       reactor, the countdown, the exit and every pickup.
//                       Needs no window and no GL.
//   --script-test       load the level's Lua declarations, enter and re-enter
//                       every trigger, run its callback, and prove trigger
//                       evaluation allocates nothing. Needs no window or GL.
//   --comm-test         decode and validate every frame of the HUD comm clip,
//                       check deterministic playback timing, then exit.
//   --comm              keep the comm panel visible for screenshot checks.
//   --target-hud        show a disabled robot in the target panel for a
//                       deterministic screenshot check.
//   --level PATH        load a level other than the default mine01
//   --no-ui             hide both the HUD and the debug panels, so a
//                       screenshot shows only the game
//   --debug-ui          start with the ImGui debug panels open (F3 at
//                       runtime); they are off by default from M7
//   --autofire W        hold the trigger for weapon W (0 laser, 1 vulcan,
//                       2 concussion, 3 homing, 4 railgun, 5 lightning,
//                       6 flares), so a screenshot can show bolts and beams in
//                       flight and the lights they carry
//   --debug-lines       start with wall normals, contact points and the nose
//                       raycast drawn (all also ImGui toggles)
//   --size W H          window size. Mostly for software-rendering
//                       verification, where the full-size frame is slower
//                       than the loop's 0.25 s frame clamp and the reported
//                       step rate is bounded by the clamp rather than by the
//                       simulation.
struct Options {
    int frames = -1;
    std::string screenshot;
    std::string level;
    bool resizeTest = false;
    bool chase = false;
    bool automap = false;
    bool reveal = false;
    bool validateAssets = false;
    bool flightTest = false;
    bool worldTest = false;
    bool collisionTest = false;
    bool weaponsTest = false;
    bool robotsTest = false;
    bool loopTest = false;
    bool scriptTest = false;
    bool commTest = false;
    bool comm = false;
    bool targetHud = false;
    bool lightTest = false;
    bool headlight = false;
    bool soundTest = false;
    bool noSound = false;
    float volume = 0.8f;
    int startSegment = -1;
    bool wireframeVolumes = false;
    bool stencilView = false;
    bool debugLines = false;
    int autofire = -1;
    bool noUi = false;
    bool debugUi = false;
    bool startMenu = false;
    bool levelSelect = false;
    bool levelSwitchTest = false;
    bool settingsTest = false;
    bool settingsMenu = false;
    bool controlsMenu = false;
    bool progressTest = false;
    std::string progressPath;
    std::string settingsPath;
    int width = 1600;
    int height = 900;
    bool volumeOverride = false;
    bool sizeOverride = false;

    static Options parse(int argc, char** argv);
};

} // namespace dvk

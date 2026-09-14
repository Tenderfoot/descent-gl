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
    bool resizeTest = false;
    bool chase = false;
    bool flightTest = false;
    bool worldTest = false;
    bool collisionTest = false;
    int startSegment = -1;
    bool wireframeVolumes = false;
    bool stencilView = false;
    bool debugLines = false;
    int width = 1600;
    int height = 900;

    static Options parse(int argc, char** argv);
};

} // namespace dvk

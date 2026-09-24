#include "render/debug_ui.h"

#include "audio/audio.h"
#include "core/input.h"
#include "core/options.h"
#include "game/level.h"
#include "game/state.h"
#include "game/visibility.h"
#include "render/renderer.h"
#include "sim/collision.h"
#include "sim/flight.h"
#include "sim/lighting.h"
#include "sim/robots.h"
#include "sim/weapons.h"

#include <glm/gtc/constants.hpp>
#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace dvk::gl {

void drawDebugUi(DebugUiContext& ctx) {
    const double frameMs = ctx.frameMs;
    const long long totalSteps = ctx.totalSteps;
    const double kSimHz = ctx.simHz;
    const float alpha = ctx.alpha;
    const glm::ivec2& fb = ctx.framebuffer;
    Renderer& renderer = ctx.renderer;
    RenderState& rs = ctx.renderState;
    Level& level = ctx.level;
    PortalWalk& walk = ctx.walk;
    bool& freezeVisible = ctx.freezeVisible;
    sim::FlightConfig& flight = ctx.flight;
    const sim::FlightConfig& flightDefaults = ctx.flightDefaults;
    const CameraMode camera = ctx.camera;
    const Input& input = ctx.input;
    SimState& cur = ctx.current;
    const sim::ContactLog& contacts = ctx.contacts;
    const sim::BodySet& bodies = ctx.bodies;
    const sim::BodyResolveStats& bodyStats = ctx.bodyStats;
    const sim::RayHit& noseRay = ctx.noseRay;
    const int frame = ctx.frame;
    const int kAllocWarmupFrames = ctx.allocWarmupFrames;
    const uint64_t worstSimAllocs = ctx.worstSimAllocs;
    const int simAllocFrames = ctx.simAllocFrames;
    const uint64_t worstDrawAllocs = ctx.worstDrawAllocs;
    const int drawAllocFrames = ctx.drawAllocFrames;
    const sim::Illumination& playerLight = ctx.playerLight;
    const sim::PlayerVisibility& playerSeen = ctx.playerSeen;
    sim::WeaponConfig& weapons = ctx.weapons;
    const sim::RobotConfig& robotConfig = ctx.robotConfig;
    audio::Audio& audio = ctx.audio;
    const Options& opts = ctx.options;
    LightFlood& flood = ctx.flood;
    const std::string& levelFile = ctx.levelFile;
    int& lightEditorSelection = ctx.lightEditorSelection;
    const char* kFlightConfigPath = ctx.flightConfigPath;
    const char* kWeaponConfigPath = ctx.weaponConfigPath;

        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(330, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("descent-vk")) {
            ImGui::Text("%.2f ms  (%.0f fps)", frameMs, frameMs > 0.0 ? 1000.0 / frameMs : 0.0);
            ImGui::Text("sim steps: %lld  (%.0f Hz fixed)", totalSteps, kSimHz);
            ImGui::Text("interp alpha: %.3f", alpha);
            ImGui::Text("framebuffer: %d x %d @ %dx MSAA", fb.x, fb.y,
                        static_cast<int>(dvk::gl::Renderer::kSamples));
            ImGui::Separator();
            const dvk::gl::Renderer::Stats& st = renderer.stats();
            ImGui::Text("segment %d of %d", rs.shipSegment, level.segmentCount());
            ImGui::Text("visible %d  (visited %d, portals %d, depth %d)",
                        static_cast<int>(walk.visible().size()), walk.segmentsVisited(),
                        walk.portalsCrossed(), walk.deepest());
            ImGui::Text("lights %d of %d reach view  (%d segment draws)", st.lightsDrawn,
                        st.lightsConsidered, st.lightSegmentDraws);
            if (ImGui::Checkbox("freeze visible set", &freezeVisible)) {
                // Nothing to do: the walk simply stops being rebuilt.
            }
            ImGui::SameLine();
            ImGui::Checkbox("light gizmos", &renderer.drawLightGizmos);
            ImGui::Separator();
            ImGui::Text("pos %.1f %.1f %.1f", rs.shipPos.x, rs.shipPos.y, rs.shipPos.z);
            ImGui::Text("speed %6.2f / %.0f units/s", rs.speed, flight.maxSpeed);
            ImGui::Text("turn  %6.2f / %.1f rad/s", rs.angularSpeed, flight.maxAngularSpeed);
            ImGui::Text("camera: %s  (F1 toggles)",
                        camera == dvk::CameraMode::Chase ? "chase" : "first person");
            ImGui::Text("mouse captured: %s  (Escape toggles)",
                        input.mouseCaptured() ? "yes" : "no");
            ImGui::Separator();
            const dvk::sim::PlayerState& pl = cur.combat.player;
            ImGui::Text("shields %5.1f   energy %5.1f", pl.shields, pl.energy);
            ImGui::Text("primary: %s",
                        dvk::sim::kWeaponNames[static_cast<size_t>(pl.primaryWeapon)]);
            if (pl.primaryWeapon == dvk::sim::WeaponLaser) {
                ImGui::SameLine();
                ImGui::Text("level %d", pl.laserLevel);
            }
            ImGui::Text("secondary: %s",
                        dvk::sim::kWeaponNames[static_cast<size_t>(pl.secondaryWeapon)]);
            ImGui::Text("vulcan %d   rail %d   static %d", pl.vulcanAmmo, pl.railgunAmmo,
                        pl.lightningAmmo);
            ImGui::Text("concussion %d   homing %d   flares %d", pl.concussionAmmo, pl.homingAmmo,
                        pl.flares);
            ImGui::Text("%d bolts, %d flares burning, %d effects, %d deaths",
                        cur.combat.projectiles.count(), cur.combat.flares.count(),
                        cur.combat.effects.count(), pl.deaths);
            ImGui::Separator();
            ImGui::Text("contacts %d in %d iterations", contacts.count, contacts.iterations);
            ImGui::Text("bodies %d: %d body, %d object, %d wall, %d iter", bodies.count,
                        bodyStats.bodyContacts, bodyStats.objectContacts, bodyStats.wallContacts,
                        bodyStats.iterations);
            if (noseRay.hit) {
                ImGui::Text("nose ray: %.2f units, segment %d, triangle %d", noseRay.distance,
                            noseRay.segment, noseRay.triangle);
            } else {
                ImGui::TextDisabled("nose ray: no hit");
            }
            ImGui::Checkbox("wall normals", &renderer.drawWallNormals);
            ImGui::SameLine();
            ImGui::Checkbox("contacts", &renderer.drawContacts);
            ImGui::SameLine();
            ImGui::Checkbox("nose ray", &renderer.drawNoseRay);
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(12, 210), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(390, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("gpu timings")) {
            const dvk::gl::GpuTimers& gt = renderer.timers();
            const dvk::gl::Renderer::Stats& st = renderer.stats();
            if (!gt.valid()) {
                ImGui::TextUnformatted("waiting for the first results...");
                ImGui::TextUnformatted("(read back two frames late, so nothing stalls)");
            } else {
                const float total = gt.pass(dvk::gl::GpuTimers::Prepass) +
                                    gt.pass(dvk::gl::GpuTimers::Lights) +
                                    gt.pass(dvk::gl::GpuTimers::Emissive) +
                                    gt.pass(dvk::gl::GpuTimers::Unlit);
                ImGui::Text("depth prepass %6.3f ms", gt.pass(dvk::gl::GpuTimers::Prepass));
                ImGui::Text("all lights    %6.3f ms   (%d drawn)",
                            gt.pass(dvk::gl::GpuTimers::Lights), gt.lightCount());
                ImGui::Text("  stencil     %6.3f ms   (first %d slots)", gt.stencilTotal(),
                            gt.timedLightCount());
                ImGui::Text("  lighting    %6.3f ms   (first %d slots)", gt.lightingTotal(),
                            gt.timedLightCount());
                ImGui::Text("emissive      %6.3f ms", gt.pass(dvk::gl::GpuTimers::Emissive));
                ImGui::Text("unlit         %6.3f ms", gt.pass(dvk::gl::GpuTimers::Unlit));
                ImGui::Separator();
                ImGui::Text("gpu total     %6.3f ms", total);
                if (gt.worstLight() >= 0) {
                    ImGui::Text("most expensive light: %d at %.3f ms", gt.worstLight(),
                                gt.worstLightMs());
                }
                ImGui::Separator();
                ImGui::BeginChild("per light gpu", ImVec2(0, 150), true);
                for (int li = 0; li < gt.timedLightCount(); ++li) {
                    ImGui::Text("%2d  stencil %6.3f   light %6.3f", li, gt.stencil(li),
                                gt.lighting(li));
                }
                ImGui::EndChild();
            }
            ImGui::Separator();
            ImGui::Text("shadow casters %d   volume tris %d", st.shadowCasters,
                        st.volumeTriangles);
            // Reported, not enforced: truncating a volume would reopen the
            // open-volume bug the shadow-closure work fixed.
            ImGui::Text("heaviest volume: light %d, %d triangles", st.worstVolumeLight,
                        st.worstVolumeTriangles);
            ImGui::Separator();
            const int measured = std::max(0, frame - kAllocWarmupFrames);
            ImGui::Text("heap allocations after a %d-frame warm-up:", kAllocWarmupFrames);
            ImGui::Text("  sim step:  %llu on %d of %d frames",
                        static_cast<unsigned long long>(worstSimAllocs), simAllocFrames, measured);
            ImGui::Text("  draw path: %llu on %d of %d frames",
                        static_cast<unsigned long long>(worstDrawAllocs), drawAllocFrames,
                        measured);
        }
        ImGui::End();

        if (ImGui::Begin("flight tuning")) {
            ImGui::TextDisabled("linear");
            ImGui::SliderFloat("forward thrust", &flight.forwardThrust, 0.0f, 200.0f, "%.1f");
            ImGui::SliderFloat("strafe thrust", &flight.strafeThrust, 0.0f, 200.0f, "%.1f");
            ImGui::SliderFloat("lift thrust", &flight.liftThrust, 0.0f, 200.0f, "%.1f");
            ImGui::SliderFloat("linear damping", &flight.linearDamping, 0.1f, 12.0f, "%.2f /s");
            ImGui::SliderFloat("max speed", &flight.maxSpeed, 1.0f, 80.0f, "%.1f");
            ImGui::SliderFloat("ship radius", &flight.shipRadius, 0.2f, 6.0f, "%.2f");
            ImGui::SliderFloat("wall scrape", &flight.wallScrape, 0.0f, 12.0f, "%.2f /s");
            // The two numbers the defaults were actually tuned against: with
            // exponential damping a decay is visually finished at about three
            // time constants, and terminal speed is thrust / damping.
            ImGui::TextDisabled("  tau %.2fs, settles ~%.2fs, terminal %.1f u/s",
                                1.0f / flight.linearDamping, 3.0f / flight.linearDamping,
                                flight.forwardThrust / flight.linearDamping);

            ImGui::Separator();
            ImGui::TextDisabled("angular");
            ImGui::SliderFloat("mouse sensitivity", &flight.mouseSensitivity, 0.0f, 0.12f, "%.4f");
            ImGui::SliderFloat("roll torque", &flight.rollTorque, 0.0f, 60.0f, "%.1f");
            ImGui::SliderFloat("pitch torque", &flight.pitchTorque, 0.0f, 60.0f, "%.1f");
            ImGui::SliderFloat("yaw torque", &flight.yawTorque, 0.0f, 60.0f, "%.1f");
            ImGui::SliderFloat("angular damping", &flight.angularDamping, 0.5f, 25.0f, "%.2f /s");
            ImGui::SliderFloat("max angular speed", &flight.maxAngularSpeed, 0.5f, 12.0f, "%.2f");
            ImGui::Checkbox("invert pitch", &flight.invertPitch);
            // A mouse impulse rotates the ship by impulse / damping in total,
            // so this is the swipe length that produces a 180 degree turn.
            const float halfTurnPixels =
                flight.mouseSensitivity > 0.0f
                    ? glm::pi<float>() * flight.angularDamping / flight.mouseSensitivity
                    : 0.0f;
            ImGui::TextDisabled("  180 deg turn = %.0f px of mouse travel", halfTurnPixels);

            ImGui::Separator();
            if (ImGui::Button("Save")) dvk::sim::saveFlightConfig(kFlightConfigPath, flight);
            ImGui::SameLine();
            if (ImGui::Button("Reset to defaults")) flight = flightDefaults;
            ImGui::SameLine();
            if (ImGui::Button("Reload")) flight = dvk::sim::loadFlightConfig(kFlightConfigPath);
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(420, 12), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(430, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("shadows")) {
            const dvk::gl::Renderer::Stats& st = renderer.stats();
            ImGui::Checkbox("shadows", &renderer.shadowsEnabled);
            ImGui::SameLine();
            ImGui::Checkbox("volumes as wireframe", &renderer.drawVolumeWireframe);
            ImGui::Checkbox("stencil visualiser", &renderer.showStencil);
            ImGui::SameLine();
            ImGui::Checkbox("headlight (H)", &cur.combat.player.headlightOn);

            ImGui::Text("%d casters, %d silhouette edges, %d volume triangles",
                        st.shadowCasters, st.silhouetteEdges, st.volumeTriangles);
            ImGui::Text("shadow budget: %d of %d used, %d lights lit without casting",
                        st.shadowLights, st.shadowBudget, st.budgetDenied);
            ImGui::SliderInt("budget", &weapons.shadowBudget, 1, 32);
            ImGui::Text("%d billboards", st.billboards);

            ImGui::Separator();
            // What the simulation believes about the light on the player, next
            // to what the renderer did, because the two answer the same
            // question by different means and the panel is where they can be
            // caught disagreeing. The query traces walls and doors only, so a
            // robot standing in the beam darkens the picture but not this
            // number.
            ImGui::Text("light on the player: %.3f  (%s)", playerLight.value,
                        playerLight.dark() ? "DARK" : "lit");
            ImGui::Text("  %.0f%% of the hull lit, brightest light %.3f, threshold %.2f",
                        playerLight.litFraction() * 100.0f, playerLight.brightest,
                        static_cast<double>(dvk::sim::kDarkThreshold));
            ImGui::Text("  %d lights reach, %d unoccluded, %d shadow rays",
                        playerLight.lightsReaching, playerLight.lightsVisible,
                        playerLight.raysCast);

            // The rule the robots actually act on, which is not the same number:
            // it is a point query rather than the hull, and a player carrying a
            // headlight is visible whatever is falling on them. Shown next to
            // the illumination above precisely because the two can disagree.
            // The frame already asked this for the HUD; this is that answer, so
            // the panel cannot report something the gauge disagrees with.
            const dvk::sim::PlayerVisibility& seen = playerSeen;
            int hunting = 0;
            for (int i = 0; i < cur.robots.count; ++i) {
                const dvk::sim::Robot& r = cur.robots.robots[static_cast<size_t>(i)];
                if (r.alive && r.awareness > 0.0f) ++hunting;
            }
            ImGui::Text("robots can see the player: %s%s", seen.visible ? "YES" : "no",
                        seen.headlight ? "  (headlight)" : "");
            ImGui::Text("  %d robot(s) already on to them and still hunting", hunting);

            ImGui::Separator();
            ImGui::TextDisabled("per light: edges / volume triangles / segments");
            ImGui::BeginChild("per light", ImVec2(0, 150), true);
            const auto& ls = renderer.lightStats();
            for (size_t i = 0; i < ls.size(); ++i) {
                const bool isStatic = i < level.lights.size();
                const char* label = isStatic ? "light" : (i == level.lights.size() ? "headlight"
                                                                                  : "dynamic");
                ImGui::PushID(static_cast<int>(i));
                if (isStatic) {
                    ImGui::Checkbox("##casts", &level.lights[i].castsShadows);
                } else {
                    ImGui::TextDisabled("  ");
                }
                ImGui::SameLine();
                ImGui::Text("%-9s %4d edges  %5d tris  %2d segs", label, ls[i].silhouetteEdges,
                            ls[i].volumeTriangles, ls[i].segments);
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(860, 12), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(510, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("robots")) {
            ImGui::Text("%d of %d alive   %d debris   %d drops", cur.robots.aliveCount(),
                        cur.robots.count,
                        static_cast<int>(std::count_if(
                            cur.robots.debris.begin(), cur.robots.debris.end(),
                            [](const dvk::sim::Debris& d) { return d.alive; })),
                        static_cast<int>(std::count_if(
                            cur.robots.drops.begin(), cur.robots.drops.end(),
                            [](const dvk::sim::Drop& d) { return d.alive; })));
            ImGui::Text("path field built for player segment %d", cur.robots.pathSegment);
            ImGui::Separator();
            ImGui::BeginChild("robot list", ImVec2(0, 220), true);
            for (int i = 0; i < cur.robots.count; ++i) {
                const dvk::sim::Robot& r = cur.robots.robots[static_cast<size_t>(i)];
                if (!r.alive) {
                    ImGui::TextDisabled("%2d  destroyed", i);
                    continue;
                }
                const dvk::sim::RobotDef& def =
                    robotConfig.types[static_cast<size_t>(r.type)];
                const int16_t distance =
                    r.body.segment >= 0 && r.body.segment < cur.robots.pathValidCount
                        ? cur.robots.distanceToPlayer[static_cast<size_t>(r.body.segment)]
                        : -1;
                ImGui::Text("%2d %-7s %-7s <- %-7s  %4.0f/%-4.0f hp  seg %2d  %2d away  "
                            "aware %.2f",
                            i, def.name.c_str(),
                            dvk::sim::kRobotModeNames[static_cast<size_t>(r.mode)],
                            dvk::sim::kRobotModeNames[static_cast<size_t>(r.previousMode)],
                            r.health, def.health, r.body.segment, distance, r.awareness);
            }
            ImGui::EndChild();
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(860, 300), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(430, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("sound")) {
            const dvk::audio::Mixer& mixer = audio.mixer();
            ImGui::Text("device %s   %d clip(s)   %d/%d voices",
                        opts.noSound ? "disabled"
                                     : (audio.deviceOpen() ? "open" : "none (silent)"),
                        static_cast<int>(audio.bank().clips.size()), mixer.activeVoices(),
                        dvk::audio::kMaxVoices);
            ImGui::Text("%u started, %u stolen, %u event(s), %u dropped",
                        static_cast<unsigned>(mixer.started()),
                        static_cast<unsigned>(mixer.stolen()), audio.totalPlayed(),
                        audio.droppedEvents());

            float volume = audio.masterGain();
            if (ImGui::SliderFloat("volume", &volume, 0.0f, 1.0f, "%.2f")) {
                audio.setMasterGain(volume);
            }
            bool isMuted = audio.muted();
            if (ImGui::Checkbox("mute (M)", &isMuted)) audio.setMuted(isMuted);

            // Editable, because the only way to tell whether a mine sounds
            // right is to fly it while changing these. They are not saved:
            // settling on a number means editing the default in mixer.h.
            ImGui::SeparatorText("attenuation");
            ImGui::SliderFloat("reference", &audio.mixer().attenuation.reference, 1.0f, 60.0f,
                               "%.0f units");
            ImGui::SliderFloat("maximum", &audio.mixer().attenuation.maximum, 40.0f, 500.0f,
                               "%.0f units");
            ImGui::SliderFloat("rolloff", &audio.mixer().attenuation.rolloff, 0.1f, 4.0f, "%.2f");

            // Every cue, including the ones that have never fired: a cue
            // sitting at zero for a whole level is the interesting case, and a
            // list of only what has played would be the one list that cannot
            // show it.
            ImGui::SeparatorText("cues played");
            ImGui::BeginChild("cue list", ImVec2(0, 180), true);
            const auto& played = audio.played();
            for (size_t i = 0; i < played.size(); ++i) {
                if (played[i] == 0) {
                    ImGui::TextDisabled("%-18s      -", dvk::sim::kCueNames[i]);
                } else {
                    ImGui::Text("%-18s %6u", dvk::sim::kCueNames[i], played[i]);
                }
            }
            ImGui::EndChild();
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(420, 300), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(430, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("weapons")) {
            ImGui::TextDisabled(
                "1-6 select, left mouse fires, right mouse launches, F throws a flare");
            for (int w = 0; w < dvk::sim::WeaponCount; ++w) {
                dvk::sim::WeaponDef& def = weapons.weapons[static_cast<size_t>(w)];
                ImGui::PushID(w);
                if (ImGui::CollapsingHeader(dvk::sim::kWeaponNames[static_cast<size_t>(w)])) {
                    ImGui::SliderFloat("damage", &def.damage, 0.0f, 80.0f, "%.1f");
                    ImGui::SliderFloat("speed", &def.speed, 5.0f, 400.0f, "%.0f u/s");
                    ImGui::SliderFloat("fire interval", &def.fireInterval, 0.02f, 2.0f, "%.3f s");
                    ImGui::SliderFloat("energy cost", &def.energyCost, 0.0f, 8.0f, "%.2f");
                    ImGui::SliderFloat("spread", &def.spread, 0.0f, 0.15f, "%.4f rad");
                    ImGui::ColorEdit3("colour", &def.color.r);
                    ImGui::SliderFloat("light radius", &def.lightRadius, 0.0f, 60.0f, "%.1f");
                    ImGui::SliderFloat("light intensity", &def.lightIntensity, 0.0f, 8.0f, "%.2f");
                    ImGui::SliderFloat("impact impulse", &def.impactImpulse, 0.0f, 40.0f, "%.1f");
                    if (def.splashRadius > 0.0f || w >= dvk::sim::WeaponConcussion) {
                        ImGui::SliderFloat("splash radius", &def.splashRadius, 0.0f, 30.0f, "%.1f");
                        ImGui::SliderFloat("splash damage", &def.splashDamage, 0.0f, 60.0f, "%.1f");
                        ImGui::SliderFloat("splash impulse", &def.splashImpulse, 0.0f, 150.0f,
                                           "%.1f");
                    }
                    if (dvk::sim::weaponFire(w) != dvk::sim::WeaponFire::Projectile) {
                        ImGui::SliderFloat("range", &def.range, 5.0f, 400.0f, "%.0f u");
                        ImGui::SliderFloat("beam width", &def.beamWidth, 0.02f, 0.6f, "%.3f");
                        ImGui::SliderFloat("beam time", &def.beamTime, 0.02f, 0.5f, "%.3f s");
                    }
                    if (w == dvk::sim::WeaponHoming) {
                        ImGui::SliderFloat("turn rate", &def.turnRate, 0.0f, 8.0f, "%.2f rad/s");
                        ImGui::SliderFloat("seek cone", &def.seekCone, 0.0f, 1.6f, "%.2f rad");
                        ImGui::SliderFloat("seek range", &def.seekRange, 0.0f, 300.0f, "%.0f");
                    }
                }
                ImGui::PopID();
            }
            ImGui::Separator();
            ImGui::SliderInt("laser level", &cur.combat.player.laserLevel, 1,
                             dvk::sim::kLaserLevels);
            ImGui::SliderFloat("flare radius", &weapons.flareRadius, 2.0f, 80.0f, "%.1f");
            ImGui::SliderFloat("flare lifetime", &weapons.flareLifetime, 2.0f, 90.0f, "%.0f s");
            ImGui::SliderFloat("headlight drain", &weapons.headlightDrain, 0.0f, 10.0f, "%.2f /s");
            ImGui::SliderFloat("player mass", &weapons.playerMass, 0.2f, 6.0f, "%.2f");
            if (ImGui::Button("Save weapons")) {
                dvk::sim::saveWeaponConfig(kWeaponConfigPath, weapons);
            }
            ImGui::SameLine();
            if (ImGui::Button("Refill")) {
                // From a fresh PlayerState rather than from numbers written out
                // here, so a change to what the ship starts with cannot leave
                // this button handing out last month's loadout.
                const dvk::sim::PlayerState full;
                cur.combat.player.shields = full.shields;
                cur.combat.player.energy = full.energy;
                cur.combat.player.vulcanAmmo = full.vulcanAmmo;
                cur.combat.player.concussionAmmo = full.concussionAmmo;
                cur.combat.player.homingAmmo = full.homingAmmo;
                cur.combat.player.railgunAmmo = full.railgunAmmo;
                cur.combat.player.lightningAmmo = full.lightningAmmo;
                cur.combat.player.flares = full.flares;
            }
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(12, 620), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(390, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("lights")) {
            if (ImGui::Button("Add here")) {
                dvk::Light added;
                added.position = rs.shipPos;
                added.segment = rs.shipSegment;
                added.radius = 30.0f;
                dvk::rebuildLightSegments(level, added, flood, &cur.game.doorField);
                level.lights.push_back(added);
                lightEditorSelection = static_cast<int>(level.lights.size()) - 1;
            }
            ImGui::SameLine();
            if (ImGui::Button("Save level")) dvk::saveLevel(levelFile, level);
            ImGui::SameLine();
            ImGui::TextDisabled("%d lights", static_cast<int>(level.lights.size()));

            ImGui::BeginChild("light list", ImVec2(0, 110), true);
            for (int i = 0; i < static_cast<int>(level.lights.size()); ++i) {
                const dvk::Light& l = level.lights[static_cast<size_t>(i)];
                char label[96];
                std::snprintf(label, sizeof(label), "%2d  seg %-3d  r %-5.1f  %s  (%d segs)", i,
                              l.segment, l.radius,
                              l.type == dvk::LightType::Spot ? "spot " : "point",
                              static_cast<int>(l.segments.size()));
                if (ImGui::Selectable(label, lightEditorSelection == i)) lightEditorSelection = i;
            }
            ImGui::EndChild();

            if (lightEditorSelection >= 0 &&
                lightEditorSelection < static_cast<int>(level.lights.size())) {
                dvk::Light& l = level.lights[static_cast<size_t>(lightEditorSelection)];
                // Position and radius both change which segments the light can
                // reach, so the set is rebuilt whenever either moves. Leaving it
                // stale is how a light ends up lighting a room through a wall.
                bool reach = false;
                reach |= ImGui::DragFloat3("position", &l.position.x, 0.25f);
                reach |= ImGui::SliderFloat("radius", &l.radius, 1.0f, 120.0f, "%.1f");
                ImGui::ColorEdit3("colour", &l.color.r);
                ImGui::SliderFloat("intensity", &l.intensity, 0.0f, 12.0f, "%.2f");

                int type = l.type == dvk::LightType::Spot ? 1 : 0;
                if (ImGui::Combo("type", &type, "point\0spot\0")) {
                    l.type = type == 1 ? dvk::LightType::Spot : dvk::LightType::Point;
                }
                if (l.type == dvk::LightType::Spot) {
                    ImGui::DragFloat3("direction", &l.direction.x, 0.02f, -1.0f, 1.0f);
                    ImGui::SliderFloat("cone", &l.coneDegrees, 5.0f, 170.0f, "%.0f deg");
                }

                if (reach) {
                    l.segment = dvk::findSegment(level, l.position, l.segment);
                    dvk::rebuildLightSegments(level, l, flood, &cur.game.doorField);
                }
                if (ImGui::Button("Remove")) {
                    level.lights.erase(level.lights.begin() + lightEditorSelection);
                    lightEditorSelection = -1;
                }
            } else {
                ImGui::TextDisabled("select a light to edit it");
            }
        }
        ImGui::End();
}

} // namespace dvk::gl

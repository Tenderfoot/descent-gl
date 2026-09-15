#pragma once

#include "game/level.h"
#include "sim/occluders.h"

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace dvk::sim {

struct SolidDoors;
struct CombatState;
struct WeaponConfig;
struct PropState;

// How much light arrives at a point in the mine.
//
// The renderer answers this on the GPU every frame, but only for surfaces, and
// only as pixels nobody can read back cheaply. This is the same question asked
// on the CPU, about a point in the air, in a form the simulation and the
// headless tests can both use: no GL, no window, no allocation.
//
// It exists because a mine with no ambient term makes darkness a place rather
// than a mood. "Is the player standing somewhere lit" is a real property of the
// world here, and the only honest way to answer it is to reproduce the shader's
// falloff and then ask whether anything is in the way.
//
// Two deliberate differences from what ends up on screen:
//
//  1. As of M8 the mine is no longer the only occluder - pass an OccluderSet
//     and robots, props, the ship and the level's objects block light here too,
//     the way they always have on screen. Without one the query falls back to
//     walls and door faces alone, which is what every caller did before.
//  2. The shadow budget does not apply. A light past the renderer's cap still
//     lights but stops casting, so on screen it shines through walls. Here it
//     never does. That makes this query the stricter of the two, and the one
//     worth building a mechanic on: it does not change with frame rate.

// The shader's windowed inverse square, in units of the light's own radius.
//
// MUST stay identical to the falloff in shaders/wall.frag. Two copies of this
// formula already existed - the shader and the brightness report in
// --world-test - and a third would have been the point where they drifted, so
// this is the one both the CPU paths call.
float lightFalloff(float distance, float radius);

// One light's contribution at a point, ignoring occlusion: falloff, intensity
// and the spot cone. Zero at or beyond the radius, and outside a spot's outer
// angle. No N dot L - the point being asked about is a position in the air, not
// a surface, so there is no normal to weigh it against.
float lightContribution(const Light& light, const glm::vec3& point);

// Below this a point is dark enough that the player cannot make out the walls
// without a light of their own. Picked by eye against rendered frames; it is
// the same threshold --world-test grades the level's dark stretches with.
inline constexpr float kDarkThreshold = 0.25f;

// How the point is sampled.
struct IlluminationQuery {
    glm::vec3 point{0.0f};
    int segment = -1;
    // 0 asks about the point alone. Anything positive samples the body sphere
    // as well - the centre plus six points one radius out along each axis - so
    // the answer does not flicker as a shadow edge crosses a single point. Use
    // Body::radius to ask about the thing rather than about its centre.
    float bodyRadius = 0.0f;
    // False skips the shadow rays and reports what would arrive if the mine
    // were transparent. What --world-test's brightness report measures, and
    // useful for telling "no light reaches here" apart from "a wall is in the
    // way".
    bool shadows = true;
    // WHOSE light this is being measured. Left out of the occluder set, because
    // a body cannot shadow itself: the samples start at its own centre, so
    // including its proxy puts every shadow ray inside solid geometry and the
    // answer is always dark. Default means "a point in the air, belonging to
    // nobody" and excludes nothing.
    OccluderId owner{};
};

struct Illumination {
    // Total light at the point, averaged over the samples. Comparable to
    // kDarkThreshold and to --world-test's brightness numbers.
    float value = 0.0f;
    // The brightest single light's contribution, on the same scale.
    float brightest = 0.0f;
    int lightsReaching = 0;   // passed the segment set and the radius
    int lightsVisible = 0;    // and were not occluded at any sample
    int raysCast = 0;         // shadow rays traced, for the cost readout
    int samples = 0;          // body samples taken, 1 for a point query
    // Bit s is set if sample s received light from any list. Kept as a mask
    // rather than a running fraction so that adding a second list unions with
    // the first exactly, instead of the two having to be reconciled after the
    // fact.
    uint8_t litMask = 0;

    // Fraction of the body that any light reaches, 0 to 1. The interesting
    // middle is a body straddling a shadow edge; a point query only ever
    // returns 0 or 1.
    float litFraction() const {
        if (samples <= 0) return 0.0f;
        int lit = 0;
        for (int s = 0; s < samples; ++s) lit += (litMask >> s) & 1;
        return static_cast<float>(lit) / static_cast<float>(samples);
    }

    bool dark() const { return value < kDarkThreshold; }
};

// Adds one list of lights to `out`.
//
// Called once per list rather than taking them all at once, because the level's
// lights and the frame's dynamic ones (the headlight, every bolt in flight,
// every burning flare) live in different places and neither is a copy of the
// other. Start from a default-constructed Illumination, add each list, read the
// result. Allocates nothing.
void addIllumination(Illumination& out, const IlluminationQuery& query, const Level& level,
                     const std::vector<Light>& lights, const SolidDoors* doors = nullptr,
                     const OccluderSet* occluders = nullptr);

// The single-list convenience, for callers that have only one.
Illumination illuminationAt(const IlluminationQuery& query, const Level& level,
                            const std::vector<Light>& lights, const SolidDoors* doors = nullptr,
                            const OccluderSet* occluders = nullptr);

// Adds a light the simulation owns directly - a bolt in flight, a burning
// flare, the flash off a gun - which carries no precomputed segment set to cull
// with. Falloff plus one shadow ray is the whole answer; there are only ever a
// handful of these near any one point, so skipping the cull costs nothing.
// `emitter` is the thing carrying the light, left out of the occluder set for
// the same reason `query.owner` is: a floating light inside its own housing
// would otherwise put itself out.
void addLooseLight(Illumination& out, const IlluminationQuery& query, const Level& level,
                   const glm::vec3& position, float radius, float intensity,
                   const SolidDoors* doors = nullptr, const OccluderSet* occluders = nullptr,
                   const OccluderId& emitter = OccluderId{});

// The same, for a loose light with a cone. A swinging lamp is a spot, and the
// cone is the whole point of it: standing outside the circle it throws is
// standing in the dark, and treating it as a point light reports the opposite
// in exactly the place the player is relying on.
void addLooseSpot(Illumination& out, const IlluminationQuery& query, const Level& level,
                  const glm::vec3& position, const glm::vec3& direction, float coneDegrees,
                  float radius, float intensity, const SolidDoors* doors = nullptr,
                  const OccluderSet* occluders = nullptr,
                  const OccluderId& emitter = OccluderId{});

// Whether the player can be seen at all.
//
// Separate from Illumination because being seen is not only a question of how
// much light falls on you. A player flying with the headlight on is carrying a
// lamp, and the fact that a forward-facing spot barely illuminates the hull it
// is bolted to does not make its owner hard to spot.
struct PlayerVisibility {
    float illumination = 0.0f;   // everything reaching the player, own fire included
    bool headlight = false;      // carrying their own light
    bool visible = false;

    // Why they are hidden, for the debug readout and the tests.
    bool hiddenByDarkness() const { return !visible; }
};

// Gathers the level's lights, every bolt, flare and flash the simulation owns,
// the floating and swinging lights, and the headlight, and decides whether the
// player is visible.
//
// EVERY LIGHT THE SIMULATION OWNS HAS TO BE IN HERE. This is the number the
// robots act on and the number the HUD's detection meter shows, and a light
// that reaches the screen without reaching this function is a room the player
// can see is lit while the game insists they are hidden in it. See
// kSimLightSources below, and the case per source in --light-test.
//
// Deliberately a POINT query rather than a body one. It runs every simulation
// step, seven-sample body coverage would cost seven times as much for an answer
// the AI does not use, and a body straddling a shadow edge would flicker; a
// point at the ship's centre both costs less and reads more decisively. Callers
// wanting coverage can ask illuminationAt directly.
PlayerVisibility playerVisibility(const Level& level, const CombatState& combat,
                                  const WeaponConfig& weapons, const glm::vec3& position,
                                  int segment, const SolidDoors* doors = nullptr,
                                  const PropState* props = nullptr,
                                  const OccluderSet* occluders = nullptr);

// The kinds of light the simulation owns, as distinct from the level's authored
// ones: a bolt in flight, a burning flare, an explosion flash, a floating light
// and a swinging lamp.
//
// A number rather than an enum because nothing dispatches on it. It exists so
// that adding a sixth kind breaks --light-test, which walks one case per kind
// and checks the count against this. Three times now a new emitter has been
// added without the thing that consumes light being told, so the obligation is
// written down where the next one will trip over it.
inline constexpr int kSimLightSources = 5;

// The sample points a query uses, exposed so the debug overlay can draw the
// same ones the answer came from. Returns how many of `out` were filled.
inline constexpr int kIlluminationSamples = 7;
int illuminationSamplePoints(const IlluminationQuery& query,
                             std::array<glm::vec3, kIlluminationSamples>& out);

} // namespace dvk::sim

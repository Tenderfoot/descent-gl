#pragma once

#include "sim/proxy.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dvk {

// A mine is a graph of deformed cubes. This is the first of the project's three
// permanent bets and every later system leans on it: rendering walks it for
// visibility, collision tests six sides instead of a world of triangles,
// pathfinding is BFS over it, and a light is culled to the set of segments it
// can possibly reach. Do not replace it with a soup of triangles and a
// spatial index; that trade gives up all four at once.

// Sides are always in this order. The renderer, the validator and the level
// files all depend on it.
enum Side : int { SideLeft = 0, SideTop, SideRight, SideBottom, SideBack, SideFront, SideCount };

// Which of the segment's 8 vertices make up each side, wound counter-clockwise
// seen from INSIDE the segment. The mine is a space you are inside, so a wall's
// front face and its normal both point inward.
//
// Cube corner numbering, before any deformation:
//   0 (-x,-y,-z)  1 (+x,-y,-z)  2 (+x,+y,-z)  3 (-x,+y,-z)
//   4 (-x,-y,+z)  5 (+x,-y,+z)  6 (+x,+y,+z)  7 (-x,+y,+z)
inline constexpr std::array<std::array<int, 4>, SideCount> kSideVertices{{
    {{0, 3, 7, 4}},   // left   (-x)
    {{3, 2, 6, 7}},   // top    (+y)
    {{1, 5, 6, 2}},   // right  (+x)
    {{0, 4, 5, 1}},   // bottom (-y)
    {{0, 1, 2, 3}},   // back   (-z)
    {{4, 7, 6, 5}},   // front  (+z)
}};

inline constexpr std::array<int, SideCount> kOppositeSide{
    SideRight, SideBottom, SideLeft, SideTop, SideFront, SideBack};

inline constexpr std::array<const char*, SideCount> kSideNames{"left",   "top",  "right",
                                                              "bottom", "back", "front"};

// One face of a segment: either a wall to be drawn, or a portal into a
// neighbour.
enum class KeyColor : int { None = -1, Blue = 0, Yellow, Red, Count };

inline constexpr std::array<const char*, 3> kKeyNames{"blue", "yellow", "red"};

struct SideData {
    int neighbour = -1;  // >= 0 makes this a portal into that segment
    // >= 0 makes this portal a door: index into Level::doors. A door is a
    // portal in the graph - so the two segments still share their four vertices
    // and the mine's wall surface stays closed - with a slab that fills the
    // opening while it is shut.
    int door = -1;
    int texture = 0;     // layer in the wall texture array, walls only
    // How brightly this side emits, on top of whatever light reaches it. 0,
    // the default, is an ordinary wall that is only ever as bright as the
    // lights in the room.
    //
    // This is emissive, not ambient. The project's bet is that a surface no
    // light reaches stays black, and this keeps it: a lava floor is a specific
    // wall that emits, not a floor lifted off black everywhere. It does not
    // light anything else either - the renderer has no area lights and shadow
    // volumes have no meaning for one - so a glowing pool that should also lay
    // light on the room around it is a side with `emissive` AND a Light in the
    // segment, which is the same arrangement as a lamp with a fixture.
    float emissive = 0.0f;
    std::array<glm::vec2, 4> uv{};

    // Chosen at load time, never authored: which diagonal splits the quad.
    // false = corners 0-2, true = corners 1-3. See splitSides().
    bool altDiagonal = false;

    // Plane through the side, normal pointing into the segment. Built from the
    // two triangles, so it is meaningful even when the quad is not planar.
    glm::vec3 normal{0.0f};
    float d = 0.0f;  // dot(normal, x) + d = 0

    glm::vec3 center{0.0f};

    bool isPortal() const { return neighbour >= 0; }
};

struct Segment {
    std::array<int, 8> verts{};
    std::array<SideData, SideCount> sides{};
    glm::vec3 center{0.0f};
};

enum class LightType { Point, Spot };

struct Light {
    glm::vec3 position{0.0f};
    float radius = 30.0f;
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
    LightType type = LightType::Point;
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    float coneDegrees = 45.0f;

    // A visible source for the light: the half-extent of an emissive billboard
    // drawn at `position`, or 0 for nothing, which is the default. Authored and
    // written back to the file, unlike castsShadows below.
    //
    // This is the swinging lamp's housing without the cord or the swing. A
    // spotlight whose apex is empty air reads as light from nowhere, and the
    // reason a lamp does not is that it draws the thing the light comes out of.
    float fixtureSize = 0.0f;

    int segment = 0;
    // Debug toggle, not a level property: lets one light's shadows be turned
    // off to see what it is contributing. Not written back to the file.
    bool castsShadows = true;

    // Every segment this light can possibly reach, flood-filled from `segment`
    // and stopped at any portal whose nearest point is farther away than
    // `radius`. A light is only ever considered against segments in here, which
    // is what makes a mine full of lights cost nothing in the rooms you are not
    // standing in. Rebuilt whenever the light moves or its radius changes.
    std::vector<int> segments;
};

struct PlayerStart {
    int segment = 0;
    glm::vec3 position{0.0f};
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
};

// A wall triangle after the quad split. Rendering, collision and (from M3)
// shadow volumes all use these and never the quad they came from.
struct WallTriangle {
    std::array<glm::vec3, 3> p;
    // The same three corners as indices into Level::vertices. Shadow volumes
    // need the welded topology - two segments meeting at a portal share these
    // indices, which is what makes the union of all wall triangles a closed
    // surface rather than a pile of coincident faces.
    std::array<int, 3> vi{};
    glm::vec3 normal{0.0f};  // inward
    float d = 0.0f;
    int segment = 0;
    int side = 0;
};

// A door fills one portal. Opens when a body comes close, closes again after a
// delay, and refuses to open at all while either of its locks holds: without
// the right key, or, for an emergency hatch, while the reactor is still up.
struct Door {
    int segment = 0;
    int side = 0;
    int neighbour = 0;
    KeyColor key = KeyColor::None;
    // An emergency hatch: sealed until the reactor is gone, however many keys
    // the player is carrying.
    //
    // A separate field and not a fourth KeyColor. KeyColor indexes the three
    // keys the player can be holding, in GameState, in RenderState and in the
    // signage; a fourth enumerator would run off the end of all three and
    // would be lying about what it is besides. The two locks are orthogonal
    // and a door may carry both, in which case it wants the key AND the
    // reactor.
    //
    // A hatch in a level with no reactor never opens, which is the honest
    // answer: there is nothing that could ever unseal it.
    bool requiresReactor = false;
    float openDistance = 11.0f;
    float openTime = 0.9f;      // seconds to travel from shut to open
    float closeDelay = 2.5f;    // seconds held open after nothing is near
    // The four corners of the opening, cached from the side so the slab and
    // its collision do not have to look them up every step.
    std::array<glm::vec3, 4> corners{};
    glm::vec3 center{0.0f};
    glm::vec3 normal{0.0f};
};

// The three keys are last on purpose, and anything new goes in front of them:
// the automap, the render size and the loop test all ask "is this a key" as
// `kind >= PickupKind::KeyBlue`, and a kind appended after KeyRed would quietly
// become a key to all three. Saved levels name their kind rather than number
// it, so inserting here does not invalidate a level file.
enum class PickupKind : int {
    Shield = 0, Energy, LaserUpgrade, Concussion, Homing, Flares,
    RailgunAmmo, LightningAmmo, VulcanAmmo,
    KeyBlue, KeyYellow, KeyRed, Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(PickupKind::Count)> kPickupNames{
    "shield", "energy", "laser", "concussion", "homing", "flares",
    "railgun", "lightning", "vulcan",
    "key_blue", "key_yellow", "key_red"};

struct Pickup {
    PickupKind kind = PickupKind::Shield;
    glm::vec3 position{0.0f};
    int segment = -1;
};

// Where the level is escaped from, once the reactor is gone.
struct ExitTrigger {
    bool present = false;
    glm::vec3 position{0.0f};
    float radius = 6.0f;
    int segment = -1;
};

// A static thing standing in the mine. M3 places these purely as shadow
// casters; M6 turns the same entries into robots.
struct LevelObject {
    std::string type;   // names a mesh in assets/models/
    glm::vec3 position{0.0f};
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    float scale = 1.0f;
    int segment = -1;
    // Zero means indestructible scenery. Anything positive is a target, and
    // becomes a robot's starting health in M6.
    float health = 0.0f;
};

// Authored props that move once the level is running. The simulation state for
// them lives in sim::PropState; these are only what the file says.
struct FloatingLightDef {
    glm::vec3 position{0.0f};
    int segment = -1;
    float radius = 1.1f;
    glm::vec3 color{1.0f, 0.86f, 0.62f};
    float lightRadius = 26.0f;
    float lightIntensity = 2.6f;
    float restitution = 0.72f;
    float mass = 0.45f;
    float drift = 1.4f;
};

struct SwingingLightDef {
    glm::vec3 anchor{0.0f};     // where the cord meets the ceiling
    int segment = -1;
    float cordLength = 7.0f;
    float amplitude = 0.55f;    // radians either side of straight down
    float rate = 1.5f;          // rad/s through the swing
    float phase = 0.0f;
    glm::vec3 swingAxis{1.0f, 0.0f, 0.0f};
    glm::vec3 color{1.0f, 0.95f, 0.82f};
    float lightRadius = 34.0f;
    float lightIntensity = 3.2f;
    float coneDegrees = 60.0f;
};

struct Level {
    std::string name;
    // Resolved at load against the level file's directory. Empty means this
    // level has no script.
    std::string scriptPath;
    std::vector<glm::vec3> vertices;
    std::vector<Segment> segments;
    std::vector<Light> lights;
    std::vector<LevelObject> objects;
    // The collision shape of each object, parallel to `objects` and resolved at
    // load from the model each one names. A sphere per object was the old
    // answer and it was wrong by a factor of four on a column: see
    // `sim::ProxyVolume`. Built by `resolveObjectProxies` in game/proxies.cpp,
    // because deriving it needs the OBJ and `sim` may not read one.
    std::vector<sim::PlacedProxy> objectProxies;
    // The convex hulls those proxies point into, one per distinct model that
    // has one, with the flat arrays that back them. Sized once and never grown
    // afterwards, because a `PlacedProxy` holds raw pointers into them and a
    // reallocation would leave every proxy in the level pointing at freed
    // memory. `resolveObjectProxies` builds them in two passes for that reason.
    std::vector<glm::vec4> hullPlanes;
    std::vector<glm::vec3> hullVertices;
    std::vector<int> hullFaceFirst;
    std::vector<int> hullFaceVertex;
    std::vector<Door> doors;
    std::vector<Pickup> pickups;
    std::vector<FloatingLightDef> floatingLights;
    std::vector<SwingingLightDef> swingingLights;
    ExitTrigger exitTrigger;
    PlayerStart start;

    // Wall triangles grouped by segment, with the range each segment owns, so
    // "draw only these segments" is a handful of draw calls into one buffer.
    std::vector<WallTriangle> walls;
    std::vector<int> wallFirst;  // per segment, index of its first wall triangle
    std::vector<int> wallCount;  // per segment

    int segmentCount() const { return static_cast<int>(segments.size()); }

    // The four corners of a side, in winding order.
    std::array<glm::vec3, 4> sideCorners(int segment, int side) const;
};

// Loads and validates. Any structural problem is fatal with a message naming
// the segment and side, because a level file that is wrong here produces
// symptoms (light leaks, a ship falling out of the world) that look like
// renderer or physics bugs.
Level loadLevel(const std::string& path);

// Writes the level back out, preserving the authored structure and picking up
// whatever the ImGui light editor changed.
bool saveLevel(const std::string& path, const Level& level);

// Scratch space for the flood fill, so a light whose set is rebuilt every frame
// (the headlight) does not allocate in the frame loop.
struct LightFlood {
    std::vector<uint32_t> mark;
    std::vector<int> queue;
    uint32_t stamp = 0;
};

namespace sim {
struct DoorField;
}

// True if a point is inside the mine's sealed surface, by ray parity against
// every wall triangle.
//
// A different question from findSegment(), and the one containment checks want.
// findSegment() asks which hexahedron holds the point and answers -1 on a point
// that sits exactly on a shared face or in a corner where two segments meet -
// which is inside the mine, just not unambiguously inside any one segment.
// Using findSegment() to mean "did it escape" reports leaks that are not there.
//
// Linear in the wall count and meant for tests, not for the step.
bool insideMine(const Level& level, const glm::vec3& point);

// Recomputes one light's segment set. Call after moving it or changing radius;
// a stale set is how a light ends up lighting a room through a wall.
//
// `doors`, when given, stops the flood at any door that is shut, so a closed
// door blocks light as well as movement.
void rebuildLightSegments(const Level& level, Light& light, LightFlood& scratch,
                          const sim::DoorField* doors = nullptr);

// Convenience for load time and the editor, where one more allocation does not
// matter.
void rebuildLightSegments(Level& level, Light& light);

// Every level light at once, against the current door state. Called at start
// and again whenever a door becomes see-through or shut: a level light's set
// is otherwise computed once at load with no doors at all, and a light beside
// a shut door then lights the far side through the doorway.
void rebuildAllLightSegments(Level& level, LightFlood& scratch, const sim::DoorField* doors);

// Which segment contains p, or -1. `hint` is tried first and its neighbours
// next, so the common case of "still where it was last step" is a few plane
// tests rather than a search.
int findSegment(const Level& level, const glm::vec3& p, int hint = -1);

// Follows p across portal planes starting from `current`, returning the
// segment it now belongs to. Used every sim step to track the ship.
int trackSegment(const Level& level, const glm::vec3& p, int current);

} // namespace dvk

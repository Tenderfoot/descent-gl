#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

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
// delay, and refuses to open at all without the right key.
struct Door {
    int segment = 0;
    int side = 0;
    int neighbour = 0;
    KeyColor key = KeyColor::None;
    float openDistance = 11.0f;
    float openTime = 0.9f;      // seconds to travel from shut to open
    float closeDelay = 2.5f;    // seconds held open after nothing is near
    // The four corners of the opening, cached from the side so the slab and
    // its collision do not have to look them up every step.
    std::array<glm::vec3, 4> corners{};
    glm::vec3 center{0.0f};
    glm::vec3 normal{0.0f};
};

enum class PickupKind : int {
    Shield = 0, Energy, LaserUpgrade, Concussion, Homing, Flares,
    KeyBlue, KeyYellow, KeyRed, Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(PickupKind::Count)> kPickupNames{
    "shield", "energy", "laser", "concussion", "homing", "flares",
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

struct Level {
    std::string name;
    std::vector<glm::vec3> vertices;
    std::vector<Segment> segments;
    std::vector<Light> lights;
    std::vector<LevelObject> objects;
    std::vector<Door> doors;
    std::vector<Pickup> pickups;
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

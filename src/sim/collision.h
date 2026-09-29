#pragma once

#include "sim/body.h"

#include <array>
#include <glm/vec3.hpp>

namespace dvk {
struct Level;
}

namespace dvk::sim {

// Collision against the mine, and the ray and sweep queries every later
// milestone consumes.
//
// A ship that clips through walls poisons everything after it: projectiles that
// tunnel, robots that leave the mine, doors that do not block. So the resolve
// loop below is deliberately iterated and deliberately handles the edge and
// vertex cases of sphere-versus-triangle, which are the ones a plane-only test
// gets wrong and the ones that let a body fall out of the world.
//
// Nothing here allocates.

struct Contact {
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f};   // out of the wall, towards the body
    int segment = -1;
    int triangle = -1;
    float depth = 0.0f;
};

// Optional record of what the last resolve touched, for the debug overlay.
struct ContactLog {
    static constexpr int kCapacity = 32;
    std::array<Contact, kCapacity> contacts{};
    int count = 0;
    int iterations = 0;

    void clear() {
        count = 0;
        iterations = 0;
    }
    void add(const Contact& c) {
        if (count < kCapacity) contacts[static_cast<size_t>(count++)] = c;
    }
};

// How many doors a level may have. Here rather than with the game flow because
// this is the file that owns door geometry, and the two things that have to
// agree on the number - the slab set below and the strike set in combat.h -
// both reach it from here.
inline constexpr int kMaxDoors = 32;

// A door counts as open to a body a little before its slabs are fully buried,
// so flying through one does not feel like squeezing past it.
//
// This is the PATHING and body-to-body answer - "is this doorway a way through"
// - and not the collision geometry, which is the slabs themselves and is exact
// at every openness. A ship small enough to fit can already be through the gap
// before this trips; what trips here is the robots' route planning and whether
// two bodies either side of the doorway are allowed to shove each other.
inline constexpr float kPassableAt = 0.25f;

// The triangles of every door slab that is still in the way, in world space.
//
// A door is a portal in the segment graph, so it contributes nothing to the
// static wall list. What lives here is the two SLABS at their current positions
// - not the whole opening - because that is what a door actually is: two halves
// that slide apart, leaving a gap in the middle that grows. Collide against the
// opening instead and a door is all-or-nothing, so it either stops a ship that
// can plainly see its way through the gap or lets one through the part of the
// doorway still covered by slab. Both of those were true before this was
// per-slab.
//
// A slab is opaque at every openness - it is a steel plate - so there is no
// per-face see-through flag any more. What a ray needs to know instead is
// whether the door is fully SHUT, because only then can the portal behind it
// not be crossed at all; `openness` carries that and the passability threshold
// besides. Faces have to work from both sides.
struct SolidDoors {
    struct Face {
        std::array<glm::vec3, 3> p{};
        glm::vec3 normal{0.0f};
        int segmentA = -1;
        int segmentB = -1;
        // Which Level::doors entry this slab belongs to, so that something
        // hitting it - a shot, most of all - knows which door to open.
        int door = -1;
        float openness = 0.0f;
    };
    // Four triangles per door that is not yet fully open: two slabs, two
    // triangles each. Sized so every door a level may have can be mid-travel at
    // once, which is the only bound that cannot be argued with - the old cap
    // held half as many because a door left the set once it was merely passable,
    // and a slab stays now until it is buried.
    static constexpr int kCapacity = kMaxDoors * 4;
    std::array<Face, kCapacity> faces{};
    int count = 0;

    void clear() { count = 0; }

    // How many of the faces belong to a door a body still cannot route through.
    //
    // `count` on its own no longer answers that: a slab stays in the set until
    // its door is fully open, so the total does not change on the step a door
    // becomes passable. Anything caching a decision that turns on passability -
    // the robots' path field, above all - has to watch this number instead, or
    // it keeps steering around a door that has since opened.
    int shutCount() const {
        int n = 0;
        for (int i = 0; i < count; ++i) {
            if (faces[static_cast<size_t>(i)].openness < kPassableAt) ++n;
        }
        return n;
    }

    bool touches(int segment) const {
        for (int i = 0; i < count; ++i) {
            if (faces[static_cast<size_t>(i)].segmentA == segment ||
                faces[static_cast<size_t>(i)].segmentB == segment) {
                return true;
            }
        }
        return false;
    }
};

// True if a door that still blocks bodies lies between two neighbouring
// segments. `doors` may be null, in which case nothing is blocked.
bool doorShut(const SolidDoors* doors, int segment, int neighbour);

// True while the door is fully closed, and so is the one state in which the
// portal behind it cannot be crossed by anything at all. Once the slabs have
// started to part, sight and shots go through the gap and are stopped by the
// slabs themselves rather than by the doorway.
bool doorSealed(const SolidDoors* doors, int segment, int neighbour);

struct CollisionConfig {
    // How many times the push-out is repeated per step. Corners need more than
    // one pass: resolving against one wall can push the body into another.
    int iterations = 4;
    // Left between the sphere and the wall after a push-out, so the next step
    // does not start in contact and chatter.
    float skin = 0.01f;
    // Exponential rate applied while in contact, so grinding along a wall
    // bleeds speed instead of preserving it perfectly. Anything meant to bounce
    // wants this at zero, or the bounce is bled away as fast as it is made.
    float scrapeDamping = 2.5f;   // 1/s
    // How much of the inward velocity comes back out. Zero is the sliding
    // response every ship and robot has always had: the inward part is removed
    // and the tangential part survives. Above zero reflects, which is what a
    // loose object rattling around a room needs.
    float restitution = 0.0f;
};

// Pushes the body out of any wall it is inside, removes the inward part of its
// velocity so it slides, and updates body.segment. Returns the number of
// contacts resolved. `log` may be null.
int resolveCollision(const Level& level, Body& body, const CollisionConfig& cfg, float dt,
                     ContactLog* log, const SolidDoors* doors = nullptr);

// --- bodies against each other ------------------------------------------

// Everything in the mine that can be stood on by something else, for one step.
//
// A body-to-body resolve cannot be done a body at a time: it needs both masses
// at once, and - more importantly - it has to be INTERLEAVED with the wall
// resolve rather than sequenced before it. Pushing two ships apart and then
// pushing each out of the walls is not enough, because the wall correction can
// put them back inside each other; and doing it the other way round pushes one
// of them through a wall, which is the failure this file exists to prevent.
// So the loop below alternates, bounded, and always ends on the mine.
struct BodyEntry {
    Body* body = nullptr;
    // How much it comes off another body. Zero is the sliding response the ship
    // and the robots have against walls; a floating light wants its own number.
    float restitution = 0.0f;
};

// Sized for the ship, every robot and every floating light at once, with room
// to spare. Deliberately not `std::vector`: this is rebuilt every step and the
// sim step does not allocate.
inline constexpr int kMaxCollisionBodies = 48;

struct BodySet {
    std::array<BodyEntry, kMaxCollisionBodies> entries{};
    int count = 0;

    void clear() { count = 0; }
    bool add(Body& body, float restitution = 0.0f) {
        if (count >= kMaxCollisionBodies) return false;
        entries[static_cast<size_t>(count)].body = &body;
        entries[static_cast<size_t>(count)].restitution = restitution;
        ++count;
        return true;
    }
};

// Which of the level's objects are solid this step.
//
// `Level::objects` is one list with two owners. An entry whose type names a
// robot is spawned as a robot and stepped by stepRobots; its proxy must NOT be
// solid, or every robot's spawn point becomes an invisible box that ships bounce
// off for no visible reason. That exact bug already happened once to gunfire -
// see `initObjects` - and this is the same trap one layer down.
//
// A raw pointer and a count rather than the ObjectHealth it comes from, so that
// collision does not have to know what combat is. It doubles as the destroyed
// check: shoot a crate and you can fly through where it stood.
struct SolidObjects {
    const bool* alive = nullptr;
    int count = 0;

    bool solid(size_t index) const {
        return alive && index < static_cast<size_t>(count) && alive[index];
    }
};

struct BodyResolveStats {
    int iterations = 0;
    int bodyContacts = 0;     // body against body
    int objectContacts = 0;   // body against a level object's proxy
    int wallContacts = 0;     // corrections the mine had to make afterwards
};

// Separates everything in `set` from everything else, and from the mine.
//
// Order within one iteration is fixed - pairs by slot index, then the level's
// objects, then the walls - because sequential resolution is order dependent
// and this project's fixed timestep is meant to be reproducible. Two runs of
// the same setup must land on the same numbers.
//
// The WALL PASS IS LAST on every iteration, so whatever the bodies did to each
// other, the mine has the final say on where they ended up. A body outside the
// mine is worse than two bodies overlapping: it tunnels projectiles, it walks
// robots through rock, and it makes doors stop blocking.
//
// Allocates nothing.
BodyResolveStats resolveBodies(const Level& level, BodySet& set, const CollisionConfig& cfg,
                               float dt, const SolidDoors* doors = nullptr,
                               const SolidObjects* objects = nullptr);

// --- queries ------------------------------------------------------------

struct RayHit {
    bool hit = false;
    float distance = 0.0f;
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f};
    int segment = -1;
    int triangle = -1;
    // Which Level::doors entry was hit, or -1 for the mine itself. What lets a
    // shot open the door it just struck, rather than dying against an anonymous
    // surface the way it used to.
    int door = -1;
    // The segment the walk was in when it stopped - set on a miss too, unlike
    // `segment`. A miss only says nothing was hit before maxDistance, and a
    // ray that slipped out of the mine through a crack between two triangles
    // misses as well; this is what tells the two apart. A ray that really
    // reached its end stopped in or beside the segment that end is in.
    int endSegment = -1;
};

// Walks the segment graph: tests the current segment's wall triangles, and on
// leaving through a portal continues into the neighbour. Stops at the first
// wall hit or at maxDistance.
RayHit raycast(const Level& level, const glm::vec3& origin, const glm::vec3& direction,
               float maxDistance, int startSegment, const SolidDoors* doors = nullptr);

// The same path, for a sphere of the given radius. This is what fast
// projectiles move along in M5, so that a bolt crossing several segments in one
// step cannot pass through a wall.
RayHit sweepSphere(const Level& level, const glm::vec3& origin, const glm::vec3& direction,
                   float radius, float maxDistance, int startSegment,
                   const SolidDoors* doors = nullptr);

} // namespace dvk::sim

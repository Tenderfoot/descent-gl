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

// The triangles of every door that is currently shut, in world space.
//
// A door is a portal in the segment graph, so it contributes nothing to the
// static wall list. While it is closed it has to stop bodies, rays and bolts
// anyway, and it has to do so from both sides. Rebuilt once per step and handed
// to everything that asks a geometric question, so collision, line of sight and
// projectiles cannot disagree about whether a door is shut.
struct SolidDoors {
    struct Face {
        std::array<glm::vec3, 3> p{};
        glm::vec3 normal{0.0f};
        int segmentA = -1;
        int segmentB = -1;
    };
    static constexpr int kCapacity = 64;
    std::array<Face, kCapacity> faces{};
    int count = 0;

    void clear() { count = 0; }
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

struct CollisionConfig {
    // How many times the push-out is repeated per step. Corners need more than
    // one pass: resolving against one wall can push the body into another.
    int iterations = 4;
    // Left between the sphere and the wall after a push-out, so the next step
    // does not start in contact and chatter.
    float skin = 0.01f;
    // Exponential rate applied while in contact, so grinding along a wall
    // bleeds speed instead of preserving it perfectly.
    float scrapeDamping = 2.5f;   // 1/s
};

// Pushes the body out of any wall it is inside, removes the inward part of its
// velocity so it slides, and updates body.segment. Returns the number of
// contacts resolved. `log` may be null.
int resolveCollision(const Level& level, Body& body, const CollisionConfig& cfg, float dt,
                     ContactLog* log, const SolidDoors* doors = nullptr);

// --- queries ------------------------------------------------------------

struct RayHit {
    bool hit = false;
    float distance = 0.0f;
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f};
    int segment = -1;
    int triangle = -1;
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

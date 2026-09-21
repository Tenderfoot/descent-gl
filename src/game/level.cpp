#include "game/level.h"

#include "core/log.h"
#include "game/proxies.h"
#include "sim/gameflow.h"
#include "sim/geometry.h"

#include <nlohmann/json.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace dvk {

namespace {

// One texture tile per this many world units, for sides with no authored UVs.
// A 20-unit segment face therefore carries between three and four tiles.
constexpr float kWorldUnitsPerTile = 5.5f;

glm::vec3 readVec3(const nlohmann::json& j) {
    if (!j.is_array() || j.size() != 3) throw std::runtime_error("expected [x, y, z]");
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

// Corner order for each of the two candidate triangulations of a quad.
constexpr int kSplitA[2][3] = {{0, 1, 2}, {0, 2, 3}};   // diagonal 0-2
constexpr int kSplitB[2][3] = {{0, 1, 3}, {1, 2, 3}};   // diagonal 1-3

glm::vec3 faceNormal(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 n = glm::cross(b - a, c - a);
    const float len = glm::length(n);
    return len > 0.0f ? n / len : glm::vec3(0.0f);
}

// Picks the diagonal that leaves the smaller crease between the two triangles.
//
// Sides are not guaranteed planar - a deformed cube is the whole point of the
// format - and the two diagonals of a warped quad are not equivalent. The one
// with the larger dot product between its triangle normals is the flatter fold,
// which is both what the eye expects and what keeps a wall from developing a
// crease a sphere can catch on in M4.
bool preferAltDiagonal(const std::array<glm::vec3, 4>& c) {
    const float flatA = glm::dot(faceNormal(c[kSplitA[0][0]], c[kSplitA[0][1]], c[kSplitA[0][2]]),
                                 faceNormal(c[kSplitA[1][0]], c[kSplitA[1][1]], c[kSplitA[1][2]]));
    const float flatB = glm::dot(faceNormal(c[kSplitB[0][0]], c[kSplitB[0][1]], c[kSplitB[0][2]]),
                                 faceNormal(c[kSplitB[1][0]], c[kSplitB[1][1]], c[kSplitB[1][2]]));
    return flatB > flatA;
}

} // namespace

std::array<glm::vec3, 4> Level::sideCorners(int segment, int side) const {
    const Segment& s = segments[static_cast<size_t>(segment)];
    std::array<glm::vec3, 4> out{};
    for (int i = 0; i < 4; ++i) {
        out[static_cast<size_t>(i)] =
            vertices[static_cast<size_t>(s.verts[static_cast<size_t>(
                kSideVertices[static_cast<size_t>(side)][static_cast<size_t>(i)])])];
    }
    return out;
}

namespace {

void splitSides(Level& level) {
    level.wallFirst.assign(level.segments.size(), 0);
    level.wallCount.assign(level.segments.size(), 0);

    for (size_t si = 0; si < level.segments.size(); ++si) {
        Segment& seg = level.segments[si];
        const auto segIndex = static_cast<int>(si);

        seg.center = glm::vec3(0.0f);
        for (int v : seg.verts) seg.center += level.vertices[static_cast<size_t>(v)];
        seg.center /= 8.0f;

        level.wallFirst[si] = static_cast<int>(level.walls.size());

        for (int side = 0; side < SideCount; ++side) {
            SideData& sd = seg.sides[static_cast<size_t>(side)];
            const std::array<glm::vec3, 4> c = level.sideCorners(segIndex, side);

            sd.altDiagonal = preferAltDiagonal(c);
            sd.center = (c[0] + c[1] + c[2] + c[3]) * 0.25f;

            // Unauthored UVs get a planar mapping at a fixed world size per
            // tile, so texel density is the same on a corridor wall and on the
            // room's raised ceiling without anyone having to type coordinates.
            if (sd.uv[1] == glm::vec2(0.0f) && sd.uv[2] == glm::vec2(0.0f)) {
                const float u = glm::length(c[1] - c[0]) / kWorldUnitsPerTile;
                const float v = glm::length(c[3] - c[0]) / kWorldUnitsPerTile;
                const float u2 = glm::length(c[2] - c[3]) / kWorldUnitsPerTile;
                const float v2 = glm::length(c[2] - c[1]) / kWorldUnitsPerTile;
                sd.uv = {glm::vec2(0.0f, 0.0f), glm::vec2(u, 0.0f), glm::vec2(u2, v2),
                         glm::vec2(0.0f, v)};
            }

            const int(*split)[3] = sd.altDiagonal ? kSplitB : kSplitA;

            const glm::vec3 n0 = faceNormal(c[split[0][0]], c[split[0][1]], c[split[0][2]]);
            const glm::vec3 n1 = faceNormal(c[split[1][0]], c[split[1][1]], c[split[1][2]]);
            sd.normal = glm::normalize(n0 + n1);
            sd.d = -glm::dot(sd.normal, sd.center);

            if (sd.isPortal()) continue;

            for (int t = 0; t < 2; ++t) {
                WallTriangle w;
                for (int k = 0; k < 3; ++k) {
                    const int corner = split[t][k];
                    w.p[static_cast<size_t>(k)] = c[static_cast<size_t>(corner)];
                    w.vi[static_cast<size_t>(k)] = seg.verts[static_cast<size_t>(
                        kSideVertices[static_cast<size_t>(side)][static_cast<size_t>(corner)])];
                }
                w.normal = faceNormal(w.p[0], w.p[1], w.p[2]);
                w.d = -glm::dot(w.normal, w.p[0]);
                w.segment = segIndex;
                w.side = side;
                level.walls.push_back(w);
            }
        }

        level.wallCount[si] = static_cast<int>(level.walls.size()) - level.wallFirst[si];
    }
}

// Every structural rule the format promises. A level that breaks one of these
// produces symptoms elsewhere that look like renderer or physics bugs, so it is
// worth being loud here instead.
void validate(const Level& level, const std::string& path) {
    const auto n = static_cast<int>(level.segments.size());

    for (int si = 0; si < n; ++si) {
        const Segment& seg = level.segments[static_cast<size_t>(si)];

        for (int v : seg.verts) {
            if (v < 0 || v >= static_cast<int>(level.vertices.size())) {
                log::fatal("{}: segment {} references vertex {}, which does not exist", path, si, v);
            }
        }

        for (int side = 0; side < SideCount; ++side) {
            const SideData& sd = seg.sides[static_cast<size_t>(side)];
            if (!sd.isPortal()) continue;

            if (sd.neighbour >= n) {
                log::fatal("{}: segment {} side {} is a portal into segment {}, which does not exist",
                           path, si, kSideNames[static_cast<size_t>(side)], sd.neighbour);
            }
            if (sd.neighbour == si) {
                log::fatal("{}: segment {} side {} is a portal into itself", path, si,
                           kSideNames[static_cast<size_t>(side)]);
            }

            // Reciprocity: the neighbour must name this segment back, through a
            // side built from the same four vertices. Checking only that the
            // neighbour points back would accept two segments joined through
            // different faces, which walks and renders fine right up until a
            // light leaks through the seam.
            const Segment& other = level.segments[static_cast<size_t>(sd.neighbour)];
            int matching = -1;
            for (int os = 0; os < SideCount; ++os) {
                if (other.sides[static_cast<size_t>(os)].neighbour == si) {
                    matching = os;
                    break;
                }
            }
            if (matching < 0) {
                log::fatal("{}: segment {} side {} opens into segment {}, which does not open back",
                           path, si, kSideNames[static_cast<size_t>(side)], sd.neighbour);
            }

            std::array<int, 4> mine{}, theirs{};
            for (int i = 0; i < 4; ++i) {
                mine[static_cast<size_t>(i)] = seg.verts[static_cast<size_t>(
                    kSideVertices[static_cast<size_t>(side)][static_cast<size_t>(i)])];
                theirs[static_cast<size_t>(i)] = other.verts[static_cast<size_t>(
                    kSideVertices[static_cast<size_t>(matching)][static_cast<size_t>(i)])];
            }
            std::sort(mine.begin(), mine.end());
            std::sort(theirs.begin(), theirs.end());
            if (mine != theirs) {
                log::fatal(
                    "{}: portal between segments {} ({}) and {} ({}) does not share its four "
                    "vertices: [{} {} {} {}] vs [{} {} {} {}]",
                    path, si, kSideNames[static_cast<size_t>(side)], sd.neighbour,
                    kSideNames[static_cast<size_t>(matching)], mine[0], mine[1], mine[2], mine[3],
                    theirs[0], theirs[1], theirs[2], theirs[3]);
            }
        }
    }

    if (level.start.segment < 0 || level.start.segment >= n) {
        log::fatal("{}: player start is in segment {}, which does not exist", path,
                   level.start.segment);
    }
}

} // namespace

bool insideMine(const Level& level, const glm::vec3& point) {
    // Deliberately irrational-looking directions: a ray down an axis lies in
    // the plane of a grid-aligned wall far too often.
    static const std::array<glm::vec3, 3> kProbes{{
        glm::normalize(glm::vec3(0.5773f, 0.3313f, 0.7454f)),
        glm::normalize(glm::vec3(-0.8112f, 0.4231f, 0.4037f)),
        glm::normalize(glm::vec3(0.2671f, -0.8923f, 0.3629f)),
    }};

    int votes = 0;
    for (const glm::vec3& dir : kProbes) {
        int crossings = 0;
        for (const WallTriangle& w : level.walls) {
            if (sim::rayTriangleDistance(point, dir, w.p[0], w.p[1], w.p[2]) > 1e-4f) ++crossings;
        }
        if (crossings % 2 == 1) ++votes;
    }
    return votes >= 2;
}

void rebuildLightSegments(const Level& level, Light& light, LightFlood& scratch,
                          const sim::DoorField* doors) {
    light.segments.clear();
    if (level.segments.empty()) return;

    if (light.segment < 0 || light.segment >= level.segmentCount()) {
        // A light outside the mine lights nothing. Better to say so than to
        // silently drop it and leave a room dark for no visible reason.
        log::warn("light at {:.1f} {:.1f} {:.1f} is not inside any segment", light.position.x,
                  light.position.y, light.position.z);
        return;
    }

    // Stamped marks rather than a cleared array: the headlight rebuilds its set
    // every frame and this runs in the draw path.
    scratch.mark.resize(level.segments.size(), 0);
    ++scratch.stamp;
    scratch.queue.clear();

    scratch.queue.push_back(light.segment);
    scratch.mark[static_cast<size_t>(light.segment)] = scratch.stamp;

    for (size_t head = 0; head < scratch.queue.size(); ++head) {
        const int si = scratch.queue[head];
        light.segments.push_back(si);

        const Segment& seg = level.segments[static_cast<size_t>(si)];
        for (int side = 0; side < SideCount; ++side) {
            const SideData& sd = seg.sides[static_cast<size_t>(side)];
            if (!sd.isPortal()) continue;
            if (doors && doors->isOpaque(si, side)) continue;
            if (scratch.mark[static_cast<size_t>(sd.neighbour)] == scratch.stamp) continue;

            // Stop at a portal the light cannot reach. Measured to the nearest
            // point of the portal itself rather than to the neighbour's centre:
            // a long segment whose centre is out of range can still have a
            // doorway well inside it.
            const std::array<glm::vec3, 4> c = level.sideCorners(si, side);
            const float nearest =
                std::min(sim::distanceToTriangle(light.position, c[0], c[1], c[2]),
                         sim::distanceToTriangle(light.position, c[0], c[2], c[3]));
            if (nearest > light.radius) continue;

            scratch.mark[static_cast<size_t>(sd.neighbour)] = scratch.stamp;
            scratch.queue.push_back(sd.neighbour);
        }
    }

    // Sorted so the renderer can binary-search membership.
    std::sort(light.segments.begin(), light.segments.end());
}

void rebuildAllLightSegments(Level& level, LightFlood& scratch, const sim::DoorField* doors) {
    for (Light& light : level.lights) {
        if (light.segment < 0 || light.segment >= level.segmentCount()) {
            light.segment = findSegment(level, light.position);
        }
        rebuildLightSegments(static_cast<const Level&>(level), light, scratch, doors);
    }
}

void rebuildLightSegments(Level& level, Light& light) {
    if (light.segment < 0 || light.segment >= level.segmentCount()) {
        light.segment = findSegment(level, light.position);
    }
    LightFlood scratch;
    rebuildLightSegments(static_cast<const Level&>(level), light, scratch);
}

namespace {

bool insideSegment(const Level& level, int si, const glm::vec3& p, float slack) {
    const Segment& seg = level.segments[static_cast<size_t>(si)];
    for (const SideData& sd : seg.sides) {
        if (glm::dot(sd.normal, p) + sd.d < -slack) return false;
    }
    return true;
}

} // namespace

int findSegment(const Level& level, const glm::vec3& p, int hint) {
    const int n = level.segmentCount();
    if (hint >= 0 && hint < n && insideSegment(level, hint, p, 0.01f)) return hint;

    if (hint >= 0 && hint < n) {
        for (const SideData& sd : level.segments[static_cast<size_t>(hint)].sides) {
            if (sd.isPortal() && insideSegment(level, sd.neighbour, p, 0.01f)) return sd.neighbour;
        }
    }

    for (int si = 0; si < n; ++si) {
        if (insideSegment(level, si, p, 0.01f)) return si;
    }
    return -1;
}

int trackSegment(const Level& level, const glm::vec3& p, int current) {
    const int n = level.segmentCount();
    if (current < 0 || current >= n) return findSegment(level, p, -1);

    // A fast step can cross more than one portal, so follow the chain rather
    // than moving a single segment. The cap stops a degenerate level from
    // spinning here forever.
    for (int step = 0; step < 8; ++step) {
        const Segment& seg = level.segments[static_cast<size_t>(current)];

        int crossed = -1;
        float worst = -0.001f;
        for (int side = 0; side < SideCount; ++side) {
            const SideData& sd = seg.sides[static_cast<size_t>(side)];
            const float signedDistance = glm::dot(sd.normal, p) + sd.d;
            if (signedDistance < worst && sd.isPortal()) {
                worst = signedDistance;
                crossed = sd.neighbour;
            }
        }
        if (crossed < 0) return current;
        current = crossed;
    }
    return current;
}

Level loadLevel(const std::string& path) {
    std::ifstream in(path);
    if (!in) log::fatal("cannot open level '{}'", path);

    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        log::fatal("level '{}' is not valid JSON: {}", path, e.what());
    }

    Level level;
    try {
        level.name = j.value("name", path);
        if (j.contains("script")) {
            const std::filesystem::path authored = j.at("script").get<std::string>();
            if (authored.empty() || authored.is_absolute()) {
                log::fatal("{}: script must be a non-empty relative path", path);
            }
            const std::filesystem::path levelDir =
                std::filesystem::absolute(std::filesystem::path(path)).parent_path().lexically_normal();
            const std::filesystem::path resolved = (levelDir / authored).lexically_normal();
            const std::filesystem::path relative = resolved.lexically_relative(levelDir);
            if (relative.empty() || *relative.begin() == "..") {
                log::fatal("{}: script '{}' escapes the level directory", path, authored.string());
            }
            level.scriptPath = resolved.string();
        }

        for (const auto& v : j.at("vertices")) level.vertices.push_back(readVec3(v));

        for (const auto& sj : j.at("segments")) {
            Segment seg;
            const auto& verts = sj.at("verts");
            if (verts.size() != 8) {
                log::fatal("{}: segment {} has {} vertices, expected 8", path,
                           level.segments.size(), verts.size());
            }
            for (size_t i = 0; i < 8; ++i) seg.verts[i] = verts[i].get<int>();

            const auto& sides = sj.at("sides");
            if (sides.size() != SideCount) {
                log::fatal("{}: segment {} has {} sides, expected {} (left, top, right, bottom, "
                           "back, front)",
                           path, level.segments.size(), sides.size(),
                           static_cast<int>(SideCount));
            }
            for (size_t i = 0; i < SideCount; ++i) {
                SideData& sd = seg.sides[i];
                sd.neighbour = sides[i].value("portal", -1);
                sd.door = sides[i].value("door", -1);
                sd.texture = sides[i].value("texture", 0);
                sd.emissive = sides[i].value("emissive", 0.0f);
                if (sd.emissive < 0.0f) {
                    log::fatal("{}: segment {} side {} has a negative emissive ({})", path,
                               level.segments.size(), i, sd.emissive);
                }
                if (auto it = sides[i].find("uv"); it != sides[i].end()) {
                    for (size_t k = 0; k < 4 && k < it->size(); ++k) {
                        sd.uv[k] = glm::vec2((*it)[k][0].get<float>(), (*it)[k][1].get<float>());
                    }
                } else {
                    // Left to the loader, which scales the tile to the side's
                    // real size once the vertices are known. A flat 0..1 per
                    // side would stretch one 256-pixel tile across a 20-unit
                    // wall, and a level authored on a grid has sides of
                    // several different sizes.
                    sd.uv = {};
                }
            }
            level.segments.push_back(seg);
        }

        if (auto it = j.find("lights"); it != j.end()) {
            for (const auto& lj : *it) {
                Light light;
                light.position = readVec3(lj.at("position"));
                light.radius = lj.value("radius", 30.0f);
                light.color = lj.contains("color") ? readVec3(lj.at("color")) : glm::vec3(1.0f);
                light.intensity = lj.value("intensity", 1.0f);
                light.type = lj.value("type", std::string("point")) == "spot" ? LightType::Spot
                                                                             : LightType::Point;
                if (lj.contains("direction")) light.direction = glm::normalize(readVec3(lj.at("direction")));
                light.coneDegrees = lj.value("cone", 45.0f);
                light.fixtureSize = lj.value("fixtureSize", 0.0f);
                light.segment = lj.value("segment", -1);
                level.lights.push_back(light);
            }
        }

        if (auto it = j.find("doors"); it != j.end()) {
            for (const auto& dj : *it) {
                Door door;
                door.segment = dj.at("segment").get<int>();
                door.side = dj.at("side").get<int>();
                const std::string key = dj.value("key", std::string("none"));
                for (int k = 0; k < 3; ++k) {
                    if (key == kKeyNames[static_cast<size_t>(k)]) door.key = static_cast<KeyColor>(k);
                }
                door.requiresReactor = dj.value("hatch", false);
                door.openDistance = dj.value("openDistance", door.openDistance);
                door.openTime = dj.value("openTime", door.openTime);
                door.closeDelay = dj.value("closeDelay", door.closeDelay);
                level.doors.push_back(door);
            }
        }

        if (auto it = j.find("pickups"); it != j.end()) {
            for (const auto& pj : *it) {
                Pickup pickup;
                const std::string kind = pj.at("kind").get<std::string>();
                for (size_t k = 0; k < kPickupNames.size(); ++k) {
                    if (kind == kPickupNames[k]) pickup.kind = static_cast<PickupKind>(k);
                }
                pickup.position = readVec3(pj.at("position"));
                pickup.segment = pj.value("segment", -1);
                level.pickups.push_back(pickup);
            }
        }

        if (auto it = j.find("exit"); it != j.end()) {
            level.exitTrigger.present = true;
            level.exitTrigger.position = readVec3(it->at("position"));
            level.exitTrigger.radius = it->value("radius", 6.0f);
            level.exitTrigger.segment = it->value("segment", -1);
        }

        if (auto it = j.find("floatingLights"); it != j.end()) {
            for (const auto& fj : *it) {
                FloatingLightDef def;
                def.position = readVec3(fj.at("position"));
                def.segment = fj.value("segment", -1);
                def.radius = fj.value("radius", def.radius);
                if (fj.contains("color")) def.color = readVec3(fj.at("color"));
                def.lightRadius = fj.value("lightRadius", def.lightRadius);
                def.lightIntensity = fj.value("lightIntensity", def.lightIntensity);
                def.restitution = fj.value("restitution", def.restitution);
                def.mass = fj.value("mass", def.mass);
                def.drift = fj.value("drift", def.drift);
                level.floatingLights.push_back(def);
            }
        }

        if (auto it = j.find("swingingLights"); it != j.end()) {
            for (const auto& sj : *it) {
                SwingingLightDef def;
                def.anchor = readVec3(sj.at("anchor"));
                def.segment = sj.value("segment", -1);
                def.cordLength = sj.value("cordLength", def.cordLength);
                def.amplitude = sj.value("amplitude", def.amplitude);
                def.rate = sj.value("rate", def.rate);
                def.phase = sj.value("phase", def.phase);
                if (sj.contains("swingAxis")) def.swingAxis = readVec3(sj.at("swingAxis"));
                if (sj.contains("color")) def.color = readVec3(sj.at("color"));
                def.lightRadius = sj.value("lightRadius", def.lightRadius);
                def.lightIntensity = sj.value("lightIntensity", def.lightIntensity);
                def.coneDegrees = sj.value("coneDegrees", def.coneDegrees);
                level.swingingLights.push_back(def);
            }
        }

        if (auto it = j.find("objects"); it != j.end()) {
            for (const auto& oj : *it) {
                LevelObject obj;
                obj.type = oj.at("type").get<std::string>();
                obj.position = readVec3(oj.at("position"));
                obj.scale = oj.value("scale", 1.0f);
                obj.segment = oj.value("segment", -1);
                obj.health = oj.value("health", 0.0f);
                if (auto q = oj.find("orientation"); q != oj.end() && q->size() == 4) {
                    obj.orientation = glm::quat((*q)[0].get<float>(), (*q)[1].get<float>(),
                                                (*q)[2].get<float>(), (*q)[3].get<float>());
                }
                level.objects.push_back(obj);
            }
        }

        const auto& start = j.at("start");
        level.start.segment = start.value("segment", 0);
        level.start.position = readVec3(start.at("position"));
        if (auto it = start.find("orientation"); it != start.end() && it->size() == 4) {
            level.start.orientation = glm::quat((*it)[0].get<float>(), (*it)[1].get<float>(),
                                                (*it)[2].get<float>(), (*it)[3].get<float>());
        }
    } catch (const nlohmann::json::exception& e) {
        log::fatal("level '{}' is malformed: {}", path, e.what());
    } catch (const std::exception& e) {
        log::fatal("level '{}': {}", path, e.what());
    }

    validate(level, path);
    splitSides(level);

    // Doors are resolved after the split, because they need the side's corners
    // and its averaged normal. A door on a side that is not a portal is a level
    // error: there would be nothing on the other side to open into.
    for (size_t di = 0; di < level.doors.size(); ++di) {
        Door& door = level.doors[di];
        if (door.segment < 0 || door.segment >= level.segmentCount()) {
            log::fatal("{}: door {} is in segment {}, which does not exist", path, di, door.segment);
        }
        if (door.side < 0 || door.side >= SideCount) {
            log::fatal("{}: door {} is on side {}, which is not one of the six", path, di,
                       door.side);
        }
        SideData& sd = level.segments[static_cast<size_t>(door.segment)]
                           .sides[static_cast<size_t>(door.side)];
        if (!sd.isPortal()) {
            log::fatal("{}: door {} is on segment {} side {}, which is a solid wall. A door has "
                       "to be a portal - there must be somewhere for it to open into.",
                       path, di, door.segment, kSideNames[static_cast<size_t>(door.side)]);
        }
        sd.door = static_cast<int>(di);
        door.neighbour = sd.neighbour;
        door.corners = level.sideCorners(door.segment, door.side);
        door.center = sd.center;
        door.normal = sd.normal;

        // The neighbour's matching side points at the same door, so either
        // segment can ask "is the way out of here shut?" without searching.
        Segment& other = level.segments[static_cast<size_t>(door.neighbour)];
        for (int os = 0; os < SideCount; ++os) {
            if (other.sides[static_cast<size_t>(os)].neighbour == door.segment) {
                other.sides[static_cast<size_t>(os)].door = static_cast<int>(di);
                break;
            }
        }
    }

    for (Pickup& pickup : level.pickups) {
        if (pickup.segment < 0 || pickup.segment >= level.segmentCount()) {
            pickup.segment = findSegment(level, pickup.position, pickup.segment);
        }
        if (pickup.segment < 0) {
            log::fatal("{}: pickup '{}' at {:.1f} {:.1f} {:.1f} is not inside any segment", path,
                       kPickupNames[static_cast<size_t>(pickup.kind)], pickup.position.x,
                       pickup.position.y, pickup.position.z);
        }
    }

    if (level.exitTrigger.present &&
        (level.exitTrigger.segment < 0 || level.exitTrigger.segment >= level.segmentCount())) {
        level.exitTrigger.segment = findSegment(level, level.exitTrigger.position);
    }

    // Both prop kinds are validated here rather than at spawn, so a level with
    // a lamp hung through the ceiling or a ball inside a wall fails at load -
    // where the message can name the file - instead of behaving oddly later.
    for (FloatingLightDef& def : level.floatingLights) {
        if (def.segment < 0 || def.segment >= level.segmentCount()) {
            def.segment = findSegment(level, def.position, def.segment);
        }
        if (def.segment < 0) {
            log::fatal("{}: floating light at {:.1f} {:.1f} {:.1f} is not inside any segment",
                       path, def.position.x, def.position.y, def.position.z);
        }
    }

    for (SwingingLightDef& def : level.swingingLights) {
        if (def.segment < 0 || def.segment >= level.segmentCount()) {
            def.segment = findSegment(level, def.anchor, def.segment);
        }
        if (def.segment < 0) {
            log::fatal("{}: swinging light anchored at {:.1f} {:.1f} {:.1f} is not inside any "
                       "segment", path, def.anchor.x, def.anchor.y, def.anchor.z);
        }
        // A cord longer than the room drops the lamp through the floor, where
        // it lights the segment below through solid rock.
        const glm::vec3 lowest = def.anchor + glm::vec3(0.0f, -def.cordLength, 0.0f);
        if (findSegment(level, lowest, def.segment) < 0) {
            log::fatal("{}: swinging light at {:.1f} {:.1f} {:.1f} has a {:.1f}u cord that "
                       "reaches outside the mine", path, def.anchor.x, def.anchor.y,
                       def.anchor.z, def.cordLength);
        }
    }

    for (Light& light : level.lights) rebuildLightSegments(level, light);

    for (LevelObject& obj : level.objects) {
        if (obj.segment < 0 || obj.segment >= level.segmentCount()) {
            obj.segment = findSegment(level, obj.position, obj.segment);
        }
        if (obj.segment < 0) {
            log::fatal("{}: object '{}' at {:.1f} {:.1f} {:.1f} is not inside any segment", path,
                       obj.type, obj.position.x, obj.position.y, obj.position.z);
        }
    }

    // After the objects have their segments, because a proxy is only useful
    // once the thing it belongs to is known to be somewhere real.
    resolveObjectProxies(level);

    int portals = 0;
    int altDiagonals = 0;
    for (const Segment& seg : level.segments) {
        for (const SideData& sd : seg.sides) {
            if (sd.isPortal()) ++portals;
            if (sd.altDiagonal) ++altDiagonals;
        }
    }
    log::info("level '{}': {} segments, {} portals, {} wall triangles, {} lights, {} objects, "
              "{} doors, {} pickups",
              path, level.segments.size(), portals / 2, level.walls.size(), level.lights.size(),
              level.objects.size(), level.doors.size(), level.pickups.size());
    log::info("  {} of {} sides took the alternate diagonal at the minimum-crease split",
              altDiagonals, level.segments.size() * SideCount);
    return level;
}

bool saveLevel(const std::string& path, const Level& level) {
    nlohmann::json j;
    j["name"] = level.name;
    if (!level.scriptPath.empty()) {
        const std::filesystem::path levelDir =
            std::filesystem::absolute(std::filesystem::path(path)).parent_path().lexically_normal();
        const std::filesystem::path relative =
            std::filesystem::path(level.scriptPath).lexically_normal().lexically_relative(levelDir);
        if (!relative.empty() && *relative.begin() != "..") {
            j["script"] = relative.generic_string();
        } else {
            log::error("refusing to save script path '{}' outside '{}'", level.scriptPath,
                       levelDir.string());
            return false;
        }
    }

    for (const glm::vec3& v : level.vertices) j["vertices"].push_back({v.x, v.y, v.z});

    for (const Segment& seg : level.segments) {
        nlohmann::json sj;
        sj["verts"] = seg.verts;
        for (const SideData& sd : seg.sides) {
            nlohmann::json side;
            if (sd.isPortal()) {
                side["portal"] = sd.neighbour;
                if (sd.door >= 0) side["door"] = sd.door;
            } else {
                side["texture"] = sd.texture;
                // Omitted at the default, so a level that emits nowhere reads
                // exactly as it did before sides could emit at all.
                if (sd.emissive > 0.0f) side["emissive"] = sd.emissive;
            }
            sj["sides"].push_back(side);
        }
        j["segments"].push_back(sj);
    }

    for (const Light& light : level.lights) {
        nlohmann::json lj;
        lj["position"] = {light.position.x, light.position.y, light.position.z};
        lj["radius"] = light.radius;
        lj["color"] = {light.color.r, light.color.g, light.color.b};
        lj["intensity"] = light.intensity;
        lj["type"] = light.type == LightType::Spot ? "spot" : "point";
        if (light.type == LightType::Spot) {
            lj["direction"] = {light.direction.x, light.direction.y, light.direction.z};
            lj["cone"] = light.coneDegrees;
        }
        // Omitted when there is no fixture, which is almost every light: a
        // level full of "fixtureSize": 0.0 would be noise in every diff.
        if (light.fixtureSize > 0.0f) lj["fixtureSize"] = light.fixtureSize;
        j["lights"].push_back(lj);
    }

    for (const LevelObject& obj : level.objects) {
        nlohmann::json oj;
        oj["type"] = obj.type;
        oj["position"] = {obj.position.x, obj.position.y, obj.position.z};
        oj["orientation"] = {obj.orientation.w, obj.orientation.x, obj.orientation.y,
                             obj.orientation.z};
        if (obj.scale != 1.0f) oj["scale"] = obj.scale;
        if (obj.health > 0.0f) oj["health"] = obj.health;
        oj["segment"] = obj.segment;
        j["objects"].push_back(oj);
    }

    for (const Door& door : level.doors) {
        nlohmann::json dj;
        dj["segment"] = door.segment;
        dj["side"] = door.side;
        if (door.requiresReactor) dj["hatch"] = true;
        dj["key"] = door.key == KeyColor::None
                        ? std::string("none")
                        : std::string(kKeyNames[static_cast<size_t>(door.key)]);
        dj["openDistance"] = door.openDistance;
        dj["openTime"] = door.openTime;
        dj["closeDelay"] = door.closeDelay;
        j["doors"].push_back(dj);
    }
    for (const Pickup& pickup : level.pickups) {
        j["pickups"].push_back(
            {{"kind", kPickupNames[static_cast<size_t>(pickup.kind)]},
             {"position", {pickup.position.x, pickup.position.y, pickup.position.z}},
             {"segment", pickup.segment}});
    }
    for (const FloatingLightDef& def : level.floatingLights) {
        j["floatingLights"].push_back(
            {{"position", {def.position.x, def.position.y, def.position.z}},
             {"segment", def.segment},
             {"radius", def.radius},
             {"color", {def.color.r, def.color.g, def.color.b}},
             {"lightRadius", def.lightRadius},
             {"lightIntensity", def.lightIntensity},
             {"restitution", def.restitution},
             {"mass", def.mass},
             {"drift", def.drift}});
    }
    for (const SwingingLightDef& def : level.swingingLights) {
        j["swingingLights"].push_back(
            {{"anchor", {def.anchor.x, def.anchor.y, def.anchor.z}},
             {"segment", def.segment},
             {"cordLength", def.cordLength},
             {"amplitude", def.amplitude},
             {"rate", def.rate},
             {"phase", def.phase},
             {"swingAxis", {def.swingAxis.x, def.swingAxis.y, def.swingAxis.z}},
             {"color", {def.color.r, def.color.g, def.color.b}},
             {"lightRadius", def.lightRadius},
             {"lightIntensity", def.lightIntensity},
             {"coneDegrees", def.coneDegrees}});
    }
    if (level.exitTrigger.present) {
        j["exit"] = {{"position",
                      {level.exitTrigger.position.x, level.exitTrigger.position.y,
                       level.exitTrigger.position.z}},
                     {"radius", level.exitTrigger.radius},
                     {"segment", level.exitTrigger.segment}};
    }

    j["start"]["segment"] = level.start.segment;
    j["start"]["position"] = {level.start.position.x, level.start.position.y,
                              level.start.position.z};
    j["start"]["orientation"] = {level.start.orientation.w, level.start.orientation.x,
                                 level.start.orientation.y, level.start.orientation.z};

    std::ofstream out(path);
    if (!out) {
        log::error("cannot write level '{}'", path);
        return false;
    }
    out << j.dump(2) << '\n';
    if (!out) {
        log::error("failed writing level '{}'", path);
        return false;
    }
    log::info("level saved to '{}'", path);
    return true;
}

} // namespace dvk

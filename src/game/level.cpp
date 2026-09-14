#include "game/level.h"

#include "core/log.h"
#include "sim/geometry.h"

#include <nlohmann/json.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <fstream>
#include <queue>

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
                    w.p[static_cast<size_t>(k)] = c[static_cast<size_t>(split[t][k])];
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

void rebuildLightSegments(Level& level, Light& light) {
    light.segments.clear();
    if (level.segments.empty()) return;

    if (light.segment < 0 || light.segment >= level.segmentCount()) {
        light.segment = findSegment(level, light.position);
        if (light.segment < 0) {
            // A light outside the mine lights nothing. Better to say so than to
            // silently drop it and leave a room dark for no visible reason.
            log::warn("light at {:.1f} {:.1f} {:.1f} is not inside any segment", light.position.x,
                      light.position.y, light.position.z);
            return;
        }
    }

    std::vector<bool> seen(level.segments.size(), false);
    std::queue<int> queue;
    queue.push(light.segment);
    seen[static_cast<size_t>(light.segment)] = true;

    while (!queue.empty()) {
        const int si = queue.front();
        queue.pop();
        light.segments.push_back(si);

        const Segment& seg = level.segments[static_cast<size_t>(si)];
        for (int side = 0; side < SideCount; ++side) {
            const SideData& sd = seg.sides[static_cast<size_t>(side)];
            if (!sd.isPortal() || seen[static_cast<size_t>(sd.neighbour)]) continue;

            // Stop at a portal the light cannot reach. Measured to the nearest
            // point of the portal itself rather than to the neighbour's centre:
            // a long segment whose centre is out of range can still have a
            // doorway well inside it.
            const std::array<glm::vec3, 4> c = level.sideCorners(si, side);
            const float nearest =
                std::min(sim::distanceToTriangle(light.position, c[0], c[1], c[2]),
                         sim::distanceToTriangle(light.position, c[0], c[2], c[3]));
            if (nearest > light.radius) continue;

            seen[static_cast<size_t>(sd.neighbour)] = true;
            queue.push(sd.neighbour);
        }
    }

    std::sort(light.segments.begin(), light.segments.end());
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
                sd.texture = sides[i].value("texture", 0);
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
                light.segment = lj.value("segment", -1);
                level.lights.push_back(light);
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

    for (Light& light : level.lights) rebuildLightSegments(level, light);

    int portals = 0;
    int altDiagonals = 0;
    for (const Segment& seg : level.segments) {
        for (const SideData& sd : seg.sides) {
            if (sd.isPortal()) ++portals;
            if (sd.altDiagonal) ++altDiagonals;
        }
    }
    log::info("level '{}': {} segments, {} portals, {} wall triangles, {} lights", path,
              level.segments.size(), portals / 2, level.walls.size(), level.lights.size());
    log::info("  {} of {} sides took the alternate diagonal at the minimum-crease split",
              altDiagonals, level.segments.size() * SideCount);
    return level;
}

bool saveLevel(const std::string& path, const Level& level) {
    nlohmann::json j;
    j["name"] = level.name;

    for (const glm::vec3& v : level.vertices) j["vertices"].push_back({v.x, v.y, v.z});

    for (const Segment& seg : level.segments) {
        nlohmann::json sj;
        sj["verts"] = seg.verts;
        for (const SideData& sd : seg.sides) {
            nlohmann::json side;
            if (sd.isPortal()) {
                side["portal"] = sd.neighbour;
            } else {
                side["texture"] = sd.texture;
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
        j["lights"].push_back(lj);
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

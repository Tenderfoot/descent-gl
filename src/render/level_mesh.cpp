#include "render/level_mesh.h"

#include "core/log.h"
#include "game/level.h"

#include <glm/geometric.hpp>

namespace dvk::gl {

namespace {

// UVs are authored per side corner, but a side is split into two triangles by
// one of two diagonals. These map a triangle's corners back to the side's
// corner slots so the authored UVs follow whichever split was chosen.
constexpr int kSplitA[2][3] = {{0, 1, 2}, {0, 2, 3}};
constexpr int kSplitB[2][3] = {{0, 1, 3}, {1, 2, 3}};

} // namespace

LevelMesh::LevelMesh(const Level& level) {
    std::vector<Vertex> vertices;
    vertices.reserve(level.walls.size() * 3);

    first_.resize(level.segments.size());
    count_.resize(level.segments.size());
    emissiveFirst_.resize(level.segments.size());
    emissiveCount_.resize(level.segments.size());

    const auto emit = [&](size_t si, int side) {
        const SideData& sd = level.segments[si].sides[static_cast<size_t>(side)];
        const std::array<glm::vec3, 4> c = level.sideCorners(static_cast<int>(si), side);
        const int(*split)[3] = sd.altDiagonal ? kSplitB : kSplitA;

        for (int t = 0; t < 2; ++t) {
            const glm::vec3 a = c[static_cast<size_t>(split[t][0])];
            const glm::vec3 b = c[static_cast<size_t>(split[t][1])];
            const glm::vec3 d = c[static_cast<size_t>(split[t][2])];
            const glm::vec3 raw = glm::cross(b - a, d - a);
            const float len = glm::length(raw);
            const glm::vec3 n = len > 0.0f ? raw / len : sd.normal;

            for (int k = 0; k < 3; ++k) {
                const int corner = split[t][k];
                vertices.push_back({c[static_cast<size_t>(corner)], n,
                                    sd.uv[static_cast<size_t>(corner)],
                                    static_cast<float>(sd.texture), sd.emissive});
            }
        }
    };

    for (size_t si = 0; si < level.segments.size(); ++si) {
        const Segment& seg = level.segments[si];
        first_[si] = static_cast<GLint>(vertices.size());

        // Two passes over the sides, dark ones then emitting ones, so that a
        // segment's emitting sides are a contiguous tail of its range. See
        // drawSegmentEmissive().
        for (int side = 0; side < SideCount; ++side) {
            const SideData& sd = seg.sides[static_cast<size_t>(side)];
            if (sd.isPortal() || sd.emissive > 0.0f) continue;
            emit(si, side);
        }

        emissiveFirst_[si] = static_cast<GLint>(vertices.size());
        for (int side = 0; side < SideCount; ++side) {
            const SideData& sd = seg.sides[static_cast<size_t>(side)];
            if (sd.isPortal() || sd.emissive <= 0.0f) continue;
            emit(si, side);
        }
        emissiveCount_[si] = static_cast<GLsizei>(vertices.size()) - emissiveFirst_[si];
        emissiveTotal_ += emissiveCount_[si];

        count_[si] = static_cast<GLsizei>(vertices.size()) - first_[si];
    }

    total_ = static_cast<int>(vertices.size());
    if (vertices.empty()) log::fatal("level has no wall triangles at all");

    // Grates after every segment's walls, so the segment ranges above are
    // untouched. Each is two quads back to back, one facing into each segment,
    // so back-face culling stays on and whichever face the camera is looking
    // at carries the normal that faces it. The UVs come from the grate's linear
    // mapping rather than from the side, so a point on the drawn bars and the
    // same point found by the shadow in wall.frag read the same texel.
    grateFirst_.reserve(level.grates.size());
    for (const Grate& grate : level.grates) {
        grateFirst_.push_back(static_cast<GLint>(vertices.size()));
        const SideData& sd = level.segments[static_cast<size_t>(grate.segment)]
                                 .sides[static_cast<size_t>(grate.side)];
        const int(*split)[3] = sd.altDiagonal ? kSplitB : kSplitA;
        const auto uvAt = [&](const glm::vec3& p) {
            return glm::vec2(glm::dot(glm::vec3(grate.uAxis), p) + grate.uAxis.w,
                             glm::dot(glm::vec3(grate.vAxis), p) + grate.vAxis.w);
        };

        for (int face = 0; face < 2; ++face) {
            for (int t = 0; t < 2; ++t) {
                glm::vec3 p[3];
                for (int k = 0; k < 3; ++k) {
                    // The second face runs each triangle's corners backwards,
                    // which flips both its winding and its normal.
                    const int corner = split[t][face == 0 ? k : 2 - k];
                    p[k] = grate.corners[static_cast<size_t>(corner)];
                }
                const glm::vec3 raw = glm::cross(p[1] - p[0], p[2] - p[0]);
                const float len = glm::length(raw);
                const glm::vec3 n = len > 0.0f ? raw / len
                                               : (face == 0 ? grate.normal : -grate.normal);
                for (int k = 0; k < 3; ++k) {
                    vertices.push_back({p[k], n, uvAt(p[k]), static_cast<float>(grate.texture),
                                        0.0f});
                }
            }
        }
    }

    glCreateVertexArrays(1, &vao_);
    glCreateBuffers(1, &vbo_);
    glNamedBufferStorage(vbo_, static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)),
                         vertices.data(), 0);
    glVertexArrayVertexBuffer(vao_, 0, vbo_, 0, sizeof(Vertex));

    glEnableVertexArrayAttrib(vao_, 0);
    glVertexArrayAttribFormat(vao_, 0, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, pos));
    glVertexArrayAttribBinding(vao_, 0, 0);

    glEnableVertexArrayAttrib(vao_, 1);
    glVertexArrayAttribFormat(vao_, 1, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, normal));
    glVertexArrayAttribBinding(vao_, 1, 0);

    glEnableVertexArrayAttrib(vao_, 2);
    glVertexArrayAttribFormat(vao_, 2, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, uv));
    glVertexArrayAttribBinding(vao_, 2, 0);

    glEnableVertexArrayAttrib(vao_, 3);
    glVertexArrayAttribFormat(vao_, 3, 1, GL_FLOAT, GL_FALSE, offsetof(Vertex, layer));
    glVertexArrayAttribBinding(vao_, 3, 0);

    // Read only by the emissive pass. The lit and depth shaders leave it
    // undeclared, which costs them nothing: an enabled attribute a program does
    // not name is simply not fetched.
    glEnableVertexArrayAttrib(vao_, 4);
    glVertexArrayAttribFormat(vao_, 4, 1, GL_FLOAT, GL_FALSE, offsetof(Vertex, emissive));
    glVertexArrayAttribBinding(vao_, 4, 0);

    log::info("level mesh: {} triangles in {} segment ranges, {} emitting, {} grates", total_ / 3,
              first_.size(), emissiveTotal_ / 3, grateFirst_.size());
}

LevelMesh::~LevelMesh() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void LevelMesh::drawSegment(int segment) const {
    const auto i = static_cast<size_t>(segment);
    if (i >= first_.size() || count_[i] == 0) return;
    glDrawArrays(GL_TRIANGLES, first_[i], count_[i]);
}

void LevelMesh::drawGrate(int grate) const {
    const auto i = static_cast<size_t>(grate);
    if (i >= grateFirst_.size()) return;
    glDrawArrays(GL_TRIANGLES, grateFirst_[i], kGrateVertices);
}

void LevelMesh::drawSegmentEmissive(int segment) const {
    const auto i = static_cast<size_t>(segment);
    if (i >= emissiveFirst_.size() || emissiveCount_[i] == 0) return;
    glDrawArrays(GL_TRIANGLES, emissiveFirst_[i], emissiveCount_[i]);
}

} // namespace dvk::gl

#include "sim/lighting.h"

#include "sim/collision.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace dvk::sim {

namespace {

// How much closer than the light a wall hit has to be before it counts as
// occluding. A lamp authored flush against its own ceiling puts the wall
// triangle at almost exactly the light's distance, and without a margin every
// such light would shadow itself and the mine would go black.
constexpr float kShadowSkin = 0.05f;

// smoothstep, matching the GLSL of the same name so the spot cone falls off on
// the CPU exactly as it does on screen.
float smoothstep(float edge0, float edge1, float x) {
    if (edge0 == edge1) return x < edge0 ? 0.0f : 1.0f;
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// The six axis offsets that, with the centre, make up a body sample.
constexpr std::array<glm::vec3, 6> kAxisOffsets{{
    {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
}};

} // namespace

float lightFalloff(float distance, float radius) {
    if (radius <= 0.0f || distance >= radius) return 0.0f;
    const float t = distance / radius;
    const float window = std::clamp(1.0f - t * t * t * t, 0.0f, 1.0f);
    return window * window / (1.0f + 12.0f * t * t);
}

float lightContribution(const Light& light, const glm::vec3& point) {
    if (light.intensity <= 0.0f) return 0.0f;
    const glm::vec3 toLight = light.position - point;
    const float distance = glm::length(toLight);
    float value = lightFalloff(distance, light.radius);
    if (value <= 0.0f) return 0.0f;

    if (light.type == LightType::Spot) {
        // The direction the light sees this point in, against the cone axis.
        const glm::vec3 away = toLight / std::max(distance, 1e-4f) * -1.0f;
        const float cosAngle = glm::dot(away, glm::normalize(light.direction));
        const float outer = glm::radians(light.coneDegrees * 0.5f);
        value *= smoothstep(std::cos(outer), std::cos(outer * 0.75f), cosAngle);
        if (value <= 0.0f) return 0.0f;
    }
    return value * light.intensity;
}

int illuminationSamplePoints(const IlluminationQuery& query,
                             std::array<glm::vec3, kIlluminationSamples>& out) {
    out[0] = query.point;
    if (query.bodyRadius <= 0.0f) return 1;
    for (size_t i = 0; i < kAxisOffsets.size(); ++i) {
        out[i + 1] = query.point + kAxisOffsets[i] * query.bodyRadius;
    }
    return kIlluminationSamples;
}

void addIllumination(Illumination& out, const IlluminationQuery& query, const Level& level,
                     const std::vector<Light>& lights, const SolidDoors* doors) {
    if (query.segment < 0 || query.segment >= level.segmentCount()) return;

    std::array<glm::vec3, kIlluminationSamples> samples{};
    const int sampleCount = illuminationSamplePoints(query, samples);
    out.samples = sampleCount;

    // This list's own per-sample totals. `out` accumulates across lists, so the
    // running value is added at the end and the lit samples are OR-ed into the
    // mask, which is what makes two lists union rather than overwrite.
    std::array<float, kIlluminationSamples> perSample{};

    for (const Light& light : lights) {
        // The segment set is an exact bound, not a guess: the falloff window
        // reaches zero AT the radius and the set is flood-filled to that same
        // radius through open portals only. A light whose set does not contain
        // this segment contributes exactly nothing, so this is the whole cull.
        if (std::find(light.segments.begin(), light.segments.end(), query.segment) ==
            light.segments.end()) {
            continue;
        }

        bool reaches = false;
        bool visible = false;
        for (int s = 0; s < sampleCount; ++s) {
            const glm::vec3& p = samples[static_cast<size_t>(s)];
            const float contribution = lightContribution(light, p);
            if (contribution <= 0.0f) continue;
            reaches = true;

            if (query.shadows) {
                const glm::vec3 toLight = light.position - p;
                const float distance = glm::length(toLight);
                if (distance > 1e-4f) {
                    ++out.raysCast;
                    const RayHit hit = raycast(level, p, toLight / distance, distance,
                                               query.segment, doors);
                    if (hit.hit && hit.distance < distance - kShadowSkin) continue;
                }
            }

            visible = true;
            perSample[static_cast<size_t>(s)] += contribution;
            out.litMask |= static_cast<uint8_t>(1u << s);
            out.brightest = std::max(out.brightest, contribution);
        }
        if (reaches) ++out.lightsReaching;
        if (visible) ++out.lightsVisible;
    }

    float total = 0.0f;
    for (int s = 0; s < sampleCount; ++s) total += perSample[static_cast<size_t>(s)];
    out.value += total / static_cast<float>(sampleCount);
}

Illumination illuminationAt(const IlluminationQuery& query, const Level& level,
                            const std::vector<Light>& lights, const SolidDoors* doors) {
    Illumination result;
    addIllumination(result, query, level, lights, doors);
    return result;
}

} // namespace dvk::sim

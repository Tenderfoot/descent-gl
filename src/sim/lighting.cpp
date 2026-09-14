#include "sim/lighting.h"

#include "sim/collision.h"
#include "sim/combat.h"

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

namespace {

// One light at every sample point: falloff, spot cone, then one shadow ray.
// Shared by the level's lights and by the loose ones the simulation owns, so
// the two can never drift into answering differently.
void accumulateLight(Illumination& out, std::array<float, kIlluminationSamples>& perSample,
                     const std::array<glm::vec3, kIlluminationSamples>& samples, int sampleCount,
                     const IlluminationQuery& query, const Level& level, const Light& light,
                     const SolidDoors* doors) {
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
                const RayHit hit =
                    raycast(level, p, toLight / distance, distance, query.segment, doors);
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

} // namespace

void addIllumination(Illumination& out, const IlluminationQuery& query, const Level& level,
                     const std::vector<Light>& lights, const SolidDoors* doors) {
    if (query.segment < 0 || query.segment >= level.segmentCount()) return;

    std::array<glm::vec3, kIlluminationSamples> samples{};
    const int sampleCount = illuminationSamplePoints(query, samples);
    out.samples = sampleCount;
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
        accumulateLight(out, perSample, samples, sampleCount, query, level, light, doors);
    }

    float total = 0.0f;
    for (int s = 0; s < sampleCount; ++s) total += perSample[static_cast<size_t>(s)];
    out.value += total / static_cast<float>(sampleCount);
}

void addLooseLight(Illumination& out, const IlluminationQuery& query, const Level& level,
                   const glm::vec3& position, float radius, float intensity,
                   const SolidDoors* doors) {
    if (query.segment < 0 || query.segment >= level.segmentCount()) return;
    if (radius <= 0.0f || intensity <= 0.0f) return;

    std::array<glm::vec3, kIlluminationSamples> samples{};
    const int sampleCount = illuminationSamplePoints(query, samples);
    out.samples = sampleCount;
    std::array<float, kIlluminationSamples> perSample{};

    // A stack Light with no segment set. Constructing one allocates nothing as
    // long as `segments` is left alone, which is why the cull is skipped rather
    // than faked: there is nothing to cull with, and the falloff plus one
    // shadow ray is the whole answer anyway.
    Light light;
    light.type = LightType::Point;
    light.position = position;
    light.radius = radius;
    light.intensity = intensity;
    accumulateLight(out, perSample, samples, sampleCount, query, level, light, doors);

    float total = 0.0f;
    for (int s = 0; s < sampleCount; ++s) total += perSample[static_cast<size_t>(s)];
    out.value += total / static_cast<float>(sampleCount);
}

PlayerVisibility playerVisibility(const Level& level, const CombatState& combat,
                                 const WeaponConfig& weapons, const glm::vec3& position,
                                 int segment, const SolidDoors* doors) {
    PlayerVisibility out;
    out.headlight = combat.player.headlightOn;

    IlluminationQuery query;
    query.point = position;
    query.segment = segment;

    Illumination lit;
    addIllumination(lit, query, level, level.lights, doors);

    // Everything the player's own fight throws off. The muzzle flash is the one
    // that matters most: firing from the dark lights you up for as long as the
    // flash lasts, so a shot taken from cover gives the position away without
    // any rule saying so.
    for (const Projectile& p : combat.projectiles.items) {
        if (!p.alive) continue;
        addLooseLight(lit, query, level, p.position, p.lightRadius, p.lightIntensity, doors);
    }
    for (const Flare& f : combat.flares.items) {
        if (!f.alive) continue;
        const float remaining = f.lifetime - f.age;
        const float fade = std::clamp(remaining / weapons.flareFade, 0.0f, 1.0f);
        addLooseLight(lit, query, level, f.position, weapons.flareRadius,
                      weapons.flareIntensity * fade, doors);
    }
    for (const Effect& e : combat.effects.items) {
        if (!e.alive || e.lightIntensity <= 0.0f) continue;
        const float k = e.lifetime > 0.0f ? e.age / e.lifetime : 1.0f;
        addLooseLight(lit, query, level, e.position, e.lightRadius,
                      e.lightIntensity * std::max(0.0f, 1.0f - k), doors);
    }

    out.illumination = lit.value;
    out.visible = out.headlight || lit.value >= kDarkThreshold;
    return out;
}

Illumination illuminationAt(const IlluminationQuery& query, const Level& level,
                            const std::vector<Light>& lights, const SolidDoors* doors) {
    Illumination result;
    addIllumination(result, query, level, lights, doors);
    return result;
}

} // namespace dvk::sim

#include "sim/flight.h"

#include "core/log.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace dvk::sim {

namespace {

// Reads one field if present, leaves the default alone if not. Keeps an older
// or hand-trimmed config file working after a constant is added.
template <class T>
void get(const nlohmann::json& j, const char* key, T& out) {
    if (auto it = j.find(key); it != j.end() && !it->is_null()) {
        out = it->get<T>();
    }
}

} // namespace

FlightConfig loadFlightConfig(const std::string& path) {
    FlightConfig cfg;

    std::ifstream in(path);
    if (!in) {
        log::info("flight config '{}' not found, using defaults", path);
        return cfg;
    }

    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        log::error("flight config '{}' is not valid JSON ({}), using defaults", path, e.what());
        return cfg;
    }

    get(j, "forwardThrust", cfg.forwardThrust);
    get(j, "strafeThrust", cfg.strafeThrust);
    get(j, "liftThrust", cfg.liftThrust);
    get(j, "linearDamping", cfg.linearDamping);
    get(j, "maxSpeed", cfg.maxSpeed);
    get(j, "mouseSensitivity", cfg.mouseSensitivity);
    get(j, "rollTorque", cfg.rollTorque);
    get(j, "pitchTorque", cfg.pitchTorque);
    get(j, "yawTorque", cfg.yawTorque);
    get(j, "angularDamping", cfg.angularDamping);
    get(j, "maxAngularSpeed", cfg.maxAngularSpeed);
    get(j, "invertPitch", cfg.invertPitch);
    get(j, "shipRadius", cfg.shipRadius);
    get(j, "wallScrape", cfg.wallScrape);

    log::info("flight config '{}' loaded", path);
    return cfg;
}

bool saveFlightConfig(const std::string& path, const FlightConfig& cfg) {
    const nlohmann::json j{
        {"forwardThrust", cfg.forwardThrust},
        {"strafeThrust", cfg.strafeThrust},
        {"liftThrust", cfg.liftThrust},
        {"linearDamping", cfg.linearDamping},
        {"maxSpeed", cfg.maxSpeed},
        {"mouseSensitivity", cfg.mouseSensitivity},
        {"rollTorque", cfg.rollTorque},
        {"pitchTorque", cfg.pitchTorque},
        {"yawTorque", cfg.yawTorque},
        {"angularDamping", cfg.angularDamping},
        {"maxAngularSpeed", cfg.maxAngularSpeed},
        {"invertPitch", cfg.invertPitch},
        {"shipRadius", cfg.shipRadius},
        {"wallScrape", cfg.wallScrape},
    };

    std::ofstream out(path);
    if (!out) {
        log::error("cannot write flight config '{}'", path);
        return false;
    }
    out << j.dump(2) << '\n';
    if (!out) {
        log::error("failed writing flight config '{}'", path);
        return false;
    }
    log::info("flight config saved to '{}'", path);
    return true;
}

} // namespace dvk::sim

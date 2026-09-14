#pragma once

#include "game/state.h"

namespace dvk {

struct Level;

namespace sim {
struct RobotConfig;
}

enum class AppMode {
    MainMenu,
    Playing,
    Paused,
    RunSummary,
};

class LevelSession {
public:
    void load(const Level& level, const sim::RobotConfig& robots, int startSegment = -1);
    void start();
    void restart();
    void pause();
    void resume();
    void finish();
    void leave();

    AppMode mode() const { return mode_; }
    bool active() const { return active_; }
    bool advancesSimulation() const { return mode_ == AppMode::Playing; }

    SimState& current() { return current_; }
    const SimState& current() const { return current_; }
    SimState& previous() { return previous_; }
    const SimState& previous() const { return previous_; }

private:
    AppMode mode_ = AppMode::MainMenu;
    bool active_ = false;
    SimState initial_;
    SimState previous_;
    SimState current_;
};

}

#include "game/session.h"

#include "game/level.h"

namespace dvk {

void LevelSession::load(const Level& level, const sim::RobotConfig& robots, int startSegment) {
    initial_ = SimState{};
    initial_.ship.body.position = level.start.position;
    initial_.ship.body.segment = level.start.segment;
    initial_.ship.orientation = level.start.orientation;
    if (startSegment >= 0 && startSegment < level.segmentCount()) {
        initial_.ship.body.segment = startSegment;
        initial_.ship.body.position = level.segments[static_cast<size_t>(startSegment)].center;
    }
    sim::initObjects(level, robots, initial_.combat.objects);
    sim::spawnRobots(level, robots, initial_.robots);
    sim::spawnProps(level, initial_.props);
    sim::initGame(level, initial_.robots, initial_.game);
    current_ = initial_;
    previous_ = initial_;
    active_ = false;
    mode_ = AppMode::MainMenu;
}

void LevelSession::start() {
    active_ = true;
    restart();
}

void LevelSession::restart() {
    if (!active_) return;
    current_ = initial_;
    previous_ = initial_;
    mode_ = AppMode::Playing;
}

void LevelSession::pause() {
    if (mode_ == AppMode::Playing) mode_ = AppMode::Paused;
}

void LevelSession::resume() {
    if (mode_ == AppMode::Paused) mode_ = AppMode::Playing;
}

void LevelSession::finish() {
    if (mode_ == AppMode::Playing) mode_ = AppMode::RunSummary;
}

void LevelSession::leave() {
    active_ = false;
    mode_ = AppMode::MainMenu;
}

}

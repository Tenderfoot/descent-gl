#include "game/level_load.h"

#include "core/log.h"
#include "game/comm_director.h"
#include "game/level.h"
#include "game/script.h"
#include "game/session.h"
#include "render/renderer.h"
#include "sim/collision.h"
#include "sim/robots.h"

namespace dvk {

void loadLevelScript(ScriptHost& script, const Level& level, const CommDirector& comm) {
    script.load(level);
    for (const ScriptMessage& message : script.messages()) {
        if (!comm.knows(message.clip)) {
            log::fatal("Lua message '{}' names unknown comm clip '{}'", message.id, message.clip);
        }
    }
}

void applySessionLoadout(LevelSession& session, const ScriptLoadout& loadout) {
    applyLoadout(loadout, session.initial().combat.player);
    applyLoadout(loadout, session.previous().combat.player);
    applyLoadout(loadout, session.current().combat.player);
}

void setSessionHeadlight(LevelSession& session, bool on) {
    session.initial().combat.player.headlightOn = on;
    session.previous().combat.player.headlightOn = on;
    session.current().combat.player.headlightOn = on;
}

void loadLevelFile(LoadedLevel& loaded, const std::string& path) {
    if (path == loaded.path) return;
    log::info("switching to level '{}'", path);
    loaded.level = loadLevel(path);   // fatal if the file is broken, as everywhere else
    loaded.path = path;

    loaded.renderer.setLevel(loaded.level);
    loaded.session.load(loaded.level, loaded.robots, loaded.startSegment);
    loadLevelScript(loaded.script, loaded.level, loaded.comm);
    applySessionLoadout(loaded.session, loaded.script.loadout());
    loaded.comm.leaveLevel();
    if (loaded.headlight) setSessionHeadlight(loaded.session, true);

    loaded.contacts.clear();
    const bool wasOpen = loaded.automap.open;
    loaded.automap = gl::AutomapData{};
    loaded.automap.open = wasOpen;
    loaded.automapZoomTarget = loaded.automap.zoom;
    loaded.lightDoorSignature = -1;
    loaded.lightEditorSelection = -1;
}

} // namespace dvk

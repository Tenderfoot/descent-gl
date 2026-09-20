#pragma once

#include "game/comm_player.h"

#include <string>
#include <string_view>

namespace dvk {

struct ScriptMessage;

namespace audio {
class Audio;
}

namespace gl {
class Renderer;
}

// A transmission: its clock, the clip its pictures come from, and the voice
// beside it.
//
// These three used to be three locals and four lambdas in main(), and they have
// to move together. Every frame index the player produces is relative to the
// active clip, so setting one without the other puts one message's words over
// another message's pictures.
//
// The renderer owns the clip library and the audio system owns the voice, so
// both must outlive the director. Neither is reached into for anything else:
// this asks the renderer what a clip is called and how long it runs, and tells
// the audio system to start or stop a track.
class CommDirector {
public:
    // `pinned` is --comm's screenshot clip, and is empty for an ordinary run.
    CommDirector(const gl::Renderer& renderer, audio::Audio& audio, std::string pinned);

    // Whether the library has a clip of that name. This answers rather than
    // decides: at load an unknown name is fatal, and that is the caller's call
    // to make because the caller knows whose name it is.
    bool knows(std::string_view name) const;

    // Entering a level shows nothing. A transmission is level content: the only
    // thing that raises one is a script calling comm(), so a level with no
    // script has none, and a level that wants one says when. This also runs on
    // restart, where it is what clears a transmission the previous attempt was
    // part way through.
    void enterLevel();

    // Leaving this mine for another one. Unlike enterLevel it pins nothing:
    // --comm's held frame belongs to the level the run started in.
    void leaveLevel();

    // A script callback asking for a message. The clip's own displaySeconds is
    // how long the pictures run; the message says how long it is on screen, and
    // the frames loop to fill it.
    //
    // The picture and the voice are started together here and never corrected
    // against each other afterwards; see audio.h and docs/ARCHITECTURE.md for
    // why that is enough.
    void play(int index, const ScriptMessage& message, double now);

    // What the panel should draw. `now` is the application clock, the same one
    // the main loop reads, and not the simulation's accumulated time.
    CommFrame frame(double now) const { return player_.at(now); }

    // Which clip the panel is drawing, and which message raised it. The message
    // is -1 when nothing scripted is on screen, which is also what --comm's
    // pinned frame reports: it is a screenshot and not a performance.
    int activeClip() const { return activeClip_; }
    int activeMessage() const { return activeMessage_; }

private:
    int selectClip(std::string_view name) const;
    CommClipTiming timing(int clip) const;

    const gl::Renderer* renderer_ = nullptr;
    audio::Audio* audio_ = nullptr;
    std::string pinned_;
    CommPlayer player_;
    int activeMessage_ = -1;
    int activeClip_ = 0;
};

} // namespace dvk

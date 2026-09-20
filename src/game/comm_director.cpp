#include "game/comm_director.h"

#include "audio/audio.h"
#include "game/script.h"
#include "render/renderer.h"

#include <algorithm>
#include <utility>

namespace dvk {

CommDirector::CommDirector(const gl::Renderer& renderer, audio::Audio& audio, std::string pinned)
    : renderer_(&renderer), audio_(&audio), pinned_(std::move(pinned)) {}

bool CommDirector::knows(std::string_view name) const {
    return renderer_->commClipIndex(name) >= 0;
}

int CommDirector::selectClip(std::string_view name) const {
    // Names are validated at load, so an unknown one here means the library
    // changed underneath us; clip 0 keeps the panel showing something rather
    // than indexing off the end of the array.
    return std::max(0, renderer_->commClipIndex(name));
}

CommClipTiming CommDirector::timing(int clip) const {
    const gl::CommClipInfo* info = renderer_->commClip(clip);
    if (!info) return CommClipTiming{};
    return CommClipTiming{info->frameCount, info->framesPerSecond, info->displaySeconds};
}

void CommDirector::enterLevel() {
    activeMessage_ = -1;
    activeClip_ = 0;
    if (!pinned_.empty()) {
        // --comm is a screenshot pin, not a performance: a fixed frame and no
        // voice, so the picture is the same every run. The only thing that puts
        // a transmission on screen without a script.
        activeClip_ = selectClip(pinned_);
        player_.configure(timing(activeClip_));
        player_.hold(1.0);
        audio_->stopComm();
        return;
    }
    player_.stop();
    audio_->stopComm();
}

void CommDirector::leaveLevel() {
    stop();
}

void CommDirector::stop() {
    activeMessage_ = -1;
    activeClip_ = 0;
    player_.stop();
    audio_->stopComm();
}

void CommDirector::play(int index, const ScriptMessage& message, double now) {
    activeMessage_ = index;
    activeClip_ = selectClip(message.clip);
    CommClipTiming clip = timing(activeClip_);
    if (message.seconds > 0.0f) clip.displaySeconds = message.seconds;
    player_.configure(clip);
    player_.start(now);
    audio_->playComm(audio_->bank().commClip(message.clip));
}

} // namespace dvk

#include "game/tutorial.h"

#include "audio/audio.h"
#include "game/script.h"
#include "render/renderer.h"

#include <algorithm>
#include <cmath>

namespace dvk {

namespace {

// Long enough that the pane arrives rather than appears, short enough that it
// is not something the player waits through. There is no matching fade out:
// what takes the pane away is a click, and a modal that dissolves after the
// button has been pressed reads as lag rather than as polish.
constexpr float kFadeInSeconds = 0.22f;

} // namespace

TutorialPane::TutorialPane(const gl::Renderer& renderer, audio::Audio& audio)
    : renderer_(&renderer), audio_(&audio) {}

bool TutorialPane::knows(std::string_view name) const {
    return renderer_->commClipIndex(name) >= 0;
}

void TutorialPane::show(const ScriptTutorial& tutorial, double now) {
    // Names are validated at load, so an unknown one here means the library
    // changed underneath us; clip 0 keeps the pane showing something rather
    // than indexing off the end of the array.
    clip_ = std::max(0, renderer_->commClipIndex(tutorial.clip));
    frameCount_ = 0;
    framesPerSecond_ = 0.0f;
    if (const gl::CommClipInfo* info = renderer_->commClip(clip_)) {
        frameCount_ = info->frameCount;
        framesPerSecond_ = info->framesPerSecond;
    }
    startSeconds_ = now;
    active_ = true;
    // The clip's own recording, played once. A tutorial's audio is whatever
    // sound effects the clip came with rather than a voice over it, so nothing
    // here ducks the music: see the comm director, which does.
    audio_->playComm(audio_->bank().commClip(tutorial.clip));
}

void TutorialPane::dismiss() {
    active_ = false;
    audio_->stopComm();
}

void TutorialPane::clear() {
    active_ = false;
    audio_->stopComm();
}

TutorialFrame TutorialPane::frame(double now) const {
    TutorialFrame out;
    if (!active_) return out;

    const double elapsed = std::max(0.0, now - startSeconds_);
    const auto seconds = static_cast<float>(elapsed);
    out.alpha = std::clamp(seconds / kFadeInSeconds, 0.0f, 1.0f);
    // A clip with no frames or no rate still gets a pane: the words are the
    // point and the picture is beside them, so a library that cannot supply
    // one is not a reason to refuse to say anything.
    if (frameCount_ <= 0 || framesPerSecond_ <= 0.0f) return out;
    out.frame = static_cast<int>(std::floor(seconds * framesPerSecond_)) % frameCount_;
    return out;
}

} // namespace dvk

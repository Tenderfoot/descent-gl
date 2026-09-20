#pragma once

#include "game/music.h"
#include "game/session.h"

namespace dvk {

struct SimState;

namespace audio {
class Audio;
}

// Drives the adaptive score for one rendered frame: what the simulation last
// reported, through the director, into the mixer's two transports.
//
// Adaptive music is presentation state, derived from the simulation rather than
// fed back into it, which is why this reads SimState and writes nothing back.
// Awareness already carries the useful hysteresis: it snaps to one on contact
// and takes about three seconds to decay after the player breaks line of sight,
// which is exactly as long as the soundtrack should go on believing there is a
// fight.
//
// `frameSeconds` is the real frame delta rather than the simulation's step: the
// crossfade and the marquee both run on the wall clock. It is frozen while
// paused - a pause is not the fight being over - and the director is reset
// outright when the soundtrack stops, because the next level starts its own
// fight and carrying a grace across the gap would open it mid-crossfade out of
// a mine nobody is in.
void updateSoundtrack(MusicDirector& director, audio::Audio& audio, const SimState& state,
                      AppMode mode, bool commDucked, float musicVolume, float frameSeconds,
                      float& marqueeTime);

} // namespace dvk

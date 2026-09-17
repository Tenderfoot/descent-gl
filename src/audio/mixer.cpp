#include "audio/mixer.h"

#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>

namespace dvk::audio {
namespace {

// How much of the way a voice's panning moves towards its target each block.
// The audio callback is handed 256-1024 frames at a time, so this is a few
// milliseconds of glide: enough to kill the zipper on a fast-moving bolt, short
// enough that nothing sounds like it is dragging behind where it is.
constexpr float kGainGlide = 0.35f;

// Equal-power panning. A sound moved from hard left to hard right keeps its
// apparent loudness, which a linear pan does not: at centre a linear pan is
// 0.5/0.5, which sums to 3 dB quieter than either extreme, and a bolt flying
// past the ship audibly dips as it crosses.
void equalPower(float pan, float& left, float& right) {
    const float angle = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * 0.25f *
                        3.14159265358979323846f;
    left = std::cos(angle);
    right = std::sin(angle);
}

} // namespace

void Mixer::render(float* out, int frames) {
    const int samples = frames * kChannels;
    for (int i = 0; i < samples; ++i) out[i] = 0.0f;

    drain();
    if (!clips_) return;

    const float master = masterGain_.load(std::memory_order_relaxed);

    for (Voice& v : voices_) {
        if (!v.active || !v.clip || v.clip->samples.empty()) continue;

        float targetL = 0.0f;
        float targetR = 0.0f;
        spatialise(v, targetL, targetR);
        // A new voice starts at its own gain rather than gliding up from
        // nothing; only a continuing one glides.
        if (v.gainL < 0.0f) {
            v.gainL = targetL;
            v.gainR = targetR;
        }

        const auto sourceFrames = static_cast<double>(v.clip->frames());
        const double step = static_cast<double>(v.pitch);

        for (int f = 0; f < frames; ++f) {
            if (v.cursor >= sourceFrames) {
                if (!v.loop) {
                    v.active = false;
                    if (v.ambient >= 0) {
                        ambientSlots_[static_cast<size_t>(v.ambient)] = -1;
                    }
                    break;
                }
                // Wrap by the source length rather than resetting to zero, so a
                // loop whose period does not divide the block size does not
                // gain a click at every wrap.
                v.cursor = std::fmod(v.cursor, sourceFrames);
            }

            // Linear interpolation between neighbouring source frames. The
            // pitch is rarely exactly 1, because every one-shot gets a small
            // random detune so a burst of vulcan fire does not sound like the
            // same 40 ms of audio stamped out twelve times.
            const auto i0 = static_cast<size_t>(v.cursor);
            const size_t i1 = i0 + 1 < v.clip->frames() ? i0 + 1 : i0;
            const auto t = static_cast<float>(v.cursor - static_cast<double>(i0));
            const auto interpolate = [&](int channel) {
                const size_t channels = static_cast<size_t>(v.clip->channels);
                const size_t a = i0 * channels + static_cast<size_t>(channel);
                const size_t b = i1 * channels + static_cast<size_t>(channel);
                return v.clip->samples[a] * (1.0f - t) + v.clip->samples[b] * t;
            };
            float sourceL = interpolate(0);
            float sourceR = v.clip->channels == 2 ? interpolate(1) : sourceL;
            if (v.positional && v.clip->channels == 2) {
                sourceL = sourceR = (sourceL + sourceR) * 0.5f;
            }

            // Glide per frame rather than per block: gliding per block would
            // still step, just less often.
            const float glide =
                v.gainGlideSeconds > 0.0f
                    ? std::min(1.0f, 4.6052f /
                                         (v.gainGlideSeconds * static_cast<float>(kSampleRate)))
                    : kGainGlide / static_cast<float>(frames);
            v.gainL += (targetL - v.gainL) * glide;
            v.gainR += (targetR - v.gainR) * glide;

            out[f * kChannels] += sourceL * v.gainL;
            out[f * kChannels + 1] += sourceR * v.gainR;
            v.cursor += step;
        }
    }

    // Soft clip rather than a hard clamp. With a dozen explosions in one room
    // the sum genuinely does pass 1.0, and a clamp turns that into buzzing
    // square edges; tanh bends it instead, and is bounded by +/-1 for every
    // finite input, which is what makes the "never exceeds full scale" check in
    // --sound-test hold by construction rather than by luck.
    for (int i = 0; i < samples; ++i) out[i] = std::tanh(out[i] * master);
}

void Mixer::drain() {
    Command command;
    while (commands_.pop(command)) {
        switch (command.kind) {
            case Command::Kind::SetListener:
                listener_ = command.listener;
                break;
            case Command::Kind::SetAmbient: {
                const auto channel = static_cast<size_t>(command.ambient);
                if (channel >= ambientSlots_.size()) break;
                const int slot = ambientSlots_[channel];
                // Stop: a null clip always retires the channel. Ordinary
                // ambients also stop at zero; music keeps its transport alive.
                if (command.clip < 0 || (command.gain <= 0.0f && !command.keepAliveAtZero)) {
                    if (slot >= 0) {
                        voices_[static_cast<size_t>(slot)].active = false;
                        ambientSlots_[channel] = -1;
                    }
                    break;
                }
                // Already running the same clip: update it in place. Restarting
                // every time the throttle moved would make the engine stutter.
                //
                // The clip has to match. A channel that carries a different
                // recording each time it is used - a transmission rather than
                // an engine - would otherwise keep playing whatever it started
                // with and silently ignore the new one, because nothing below
                // ever reassigns `clip`.
                if (!clips_ || static_cast<size_t>(command.clip) >= clips_->size()) break;
                const Clip* wanted = &(*clips_)[static_cast<size_t>(command.clip)];
                if (slot >= 0 && voices_[static_cast<size_t>(slot)].active) {
                    Voice& v = voices_[static_cast<size_t>(slot)];
                    if (v.clip == wanted) {
                        v.gain = command.gain;
                        v.pitch = command.pitch;
                        v.position = command.position;
                        v.positional = command.positional;
                        v.gainGlideSeconds = command.gainGlideSeconds;
                        break;
                    }
                    // A different recording, so the channel is being reused
                    // rather than adjusted. Retire the voice holding it before
                    // taking it again: start() would claim a free slot and
                    // leave this one playing in a slot the channel no longer
                    // points at - still flagged as a channel, so still never
                    // stolen, and with nothing able to stop it.
                    v.active = false;
                    v.ambient = -1;
                    ambientSlots_[channel] = -1;
                }
                start(command);
                break;
            }
            case Command::Kind::Play:
                start(command);
                break;
        }
    }
}

void Mixer::start(const Command& command) {
    if (!clips_) return;
    if (command.clip < 0 || static_cast<size_t>(command.clip) >= clips_->size()) return;
    const Clip& clip = (*clips_)[static_cast<size_t>(command.clip)];
    if (!clip.valid()) return;

    const int slot = claimSlot();
    // Only when every slot is a protected channel. Nothing to steal, so the
    // newest sound is the one that goes, exactly as a full command queue drops
    // its newest push.
    if (slot < 0) return;
    Voice& v = voices_[static_cast<size_t>(slot)];
    if (v.active) {
        ++stolen_;
        // A stolen slot might have been an ambient channel's. Releasing the
        // channel here is what stops it pointing at a voice that is now
        // something else entirely.
        if (v.ambient >= 0) ambientSlots_[static_cast<size_t>(v.ambient)] = -1;
    }

    v = Voice{};
    v.clip = &clip;
    v.cursor = 0.0;
    v.pitch = command.pitch > 0.0f ? command.pitch : 1.0f;
    v.gain = command.gain;
    v.gainGlideSeconds = command.gainGlideSeconds;
    v.position = command.position;
    v.positional = command.positional;
    v.active = true;

    if (command.kind == Command::Kind::SetAmbient) {
        // Protection and looping are separate questions. The engine and the
        // klaxon want both; a voice that has to survive a firefight but still
        // end by itself wants only the first, and forcing the loop here used to
        // deny it that.
        v.loop = command.loop;
        v.ambient = static_cast<int>(command.ambient);
        ambientSlots_[static_cast<size_t>(command.ambient)] = slot;
    }
    ++started_;
}

int Mixer::claimSlot() const {
    // -1 until the first eligible slot is examined. Seeding the search on slot
    // 0 instead was wrong twice over: when slot 0 held an ambient voice the
    // loop skipped it without ever seeding, every later comparison ran against
    // a gain of 0.0 that no real voice is quieter than, and the function
    // returned the one slot it promises never to touch.
    int quietest = -1;
    float quietestGain = 0.0f;
    for (int i = 0; i < kMaxVoices; ++i) {
        const Voice& v = voices_[static_cast<size_t>(i)];
        if (!v.active) return i;
        // An ambient channel is never the one stolen: losing the engine loop to
        // a burst of gunfire and never getting it back is worse than losing a
        // shot.
        if (v.ambient >= 0) continue;
        // gainL is -1 until the voice has been through a block, so a voice
        // started earlier in this same drain has no gain to compare yet.
        // Reading the sentinel as a level would make it the quietest thing in
        // the pool and therefore the first victim, which is backwards: it has
        // not been heard at all, so stealing it drops the sound entirely rather
        // than cutting one short. Ask what it is about to be mixed at instead.
        float left = v.gainL;
        float right = v.gainR;
        if (left < 0.0f) spatialise(v, left, right);
        const float current = std::max(left, right);
        if (quietest < 0 || current < quietestGain) {
            quietest = i;
            quietestGain = current;
        }
    }
    // Every slot is a protected channel. Dropping the new sound is the right
    // answer: the only alternative is cutting one of the channels this function
    // exists to keep. Unreachable while there are fewer ambients than voices,
    // which is why the caller only has to handle it and not recover from it.
    return quietest;
}

void Mixer::spatialise(const Voice& voice, float& left, float& right) const {
    if (!voice.positional) {
        // The player's own guns and their own hull: centred, full volume, no
        // distance. See the note on SoundEvent::atListener.
        left = voice.gain;
        right = voice.gain;
        return;
    }

    const glm::vec3 to = voice.position - listener_.position;
    const float distance = glm::length(to);

    float attenuated = voice.gain;
    if (distance > attenuation.reference) {
        if (distance >= attenuation.maximum) {
            left = 0.0f;
            right = 0.0f;
            return;
        }
        // Inverse-distance, then faded linearly to nothing at the maximum. The
        // inverse curve alone never reaches zero, so without the fade a sound
        // ten rooms away still costs a voice and still contributes a hiss.
        const float inverse =
            attenuation.reference /
            (attenuation.reference + attenuation.rolloff * (distance - attenuation.reference));
        const float fade = 1.0f - (distance - attenuation.reference) /
                                      (attenuation.maximum - attenuation.reference);
        attenuated = voice.gain * inverse * fade;
    }

    // Pan against the ship's own right vector, so rolling the ship rolls the
    // stereo image with it. In a 6DOF game that is not a detail: fly upside
    // down and a sound on your left has to move to your right, or the audio is
    // telling you something the view is contradicting.
    float pan = 0.0f;
    if (distance > 0.001f) {
        pan = glm::dot(to / distance, listener_.right);
        // Sounds very close to the camera collapse to centre. Without this, a
        // bolt spawned at a wingtip pans hard to that side for its first frame
        // and clicks across to centre as it flies away.
        const float closeness = std::min(1.0f, distance / attenuation.reference);
        pan *= closeness;
    }

    equalPower(pan, left, right);
    left *= attenuated;
    right *= attenuated;
}

int Mixer::activeVoices() const {
    int n = 0;
    for (const Voice& v : voices_) n += v.active ? 1 : 0;
    return n;
}

} // namespace dvk::audio

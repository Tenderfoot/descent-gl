#include "audio/sound_test.h"

#include "audio/audio.h"
#include "audio/mixer.h"
#include "core/alloc_count.h"
#include "core/log.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/quaternion.hpp>
#include <vector>

namespace dvk::audio {
namespace {

// A block the size of a typical driver callback, so the mixer is exercised the
// way a device would exercise it rather than one sample at a time.
constexpr int kBlock = 480;

struct Buffer {
    std::vector<float> samples;
    explicit Buffer(int frames) : samples(static_cast<size_t>(frames) * kChannels, 0.0f) {}
    float* data() { return samples.data(); }
};

float rms(const std::vector<float>& interleaved, int channel) {
    double total = 0.0;
    size_t n = 0;
    for (size_t i = static_cast<size_t>(channel); i < interleaved.size(); i += kChannels) {
        total += static_cast<double>(interleaved[i]) * interleaved[i];
        ++n;
    }
    return n ? static_cast<float>(std::sqrt(total / static_cast<double>(n))) : 0.0f;
}

float peak(const std::vector<float>& interleaved) {
    float high = 0.0f;
    for (float v : interleaved) high = std::max(high, std::abs(v));
    return high;
}

// Renders `blocks` blocks into one buffer, so a short clip can be measured over
// its whole length rather than over the first ten milliseconds of it.
Buffer renderBlocks(Mixer& mixer, int blocks) {
    Buffer out(kBlock * blocks);
    for (int b = 0; b < blocks; ++b) {
        mixer.render(out.data() + static_cast<size_t>(b) * kBlock * kChannels, kBlock);
    }
    return out;
}

// Plays one clip at `position` and measures the whole thing.
Buffer playAt(Mixer& mixer, int clip, const glm::vec3& position, int blocks) {
    Command c;
    c.kind = Command::Kind::Play;
    c.clip = clip;
    c.position = position;
    c.gain = 1.0f;
    c.positional = true;
    mixer.push(c);
    return renderBlocks(mixer, blocks);
}

void setListener(Mixer& mixer, const glm::vec3& position, const glm::vec3& right) {
    Command c;
    c.kind = Command::Kind::SetListener;
    c.listener.position = position;
    c.listener.right = right;
    c.listener.forward = glm::vec3(0.0f, 0.0f, -1.0f);
    mixer.push(c);
    // Commands are only read by render(), so the listener is not in place until
    // something has been rendered. One silent block does that.
    Buffer discard(kBlock);
    mixer.render(discard.data(), kBlock);
}

} // namespace

int runSoundTest(const std::string& soundDir) {
    bool ok = true;
    const auto fail = [&ok](const std::string& why) {
        log::error("{}", why);
        ok = false;
    };

    // --- the bank --------------------------------------------------------
    Audio audio;
    audio.init(soundDir, /*openDevice=*/false);
    const Bank& bank = audio.bank();

    size_t totalSamples = 0;
    float longest = 0.0f;
    for (const Clip& clip : bank.clips) {
        totalSamples += clip.samples.size();
        longest = std::max(longest, clip.seconds());
        for (float v : clip.samples) {
            if (!std::isfinite(v) || std::abs(v) > 1.0f) {
                fail(std::format("clip {} has a sample outside -1..1 or not finite", clip.name));
                break;
            }
        }
        if (clip.sampleRate != kSampleRate) {
            fail(std::format("clip {} is at {} Hz after load, not {}", clip.name, clip.sampleRate,
                             kSampleRate));
        }
    }
    log::info("sound test: {} clips, {:.2f} s total, longest {:.2f} s, {} KiB resident",
              bank.clips.size(),
              static_cast<double>(totalSamples) / static_cast<double>(kSampleRate), longest,
              (totalSamples * sizeof(float)) / 1024);

    int silentCues = 0;
    for (size_t i = 0; i < bank.cueClip.size(); ++i) {
        if (bank.cueClip[i] < 0) {
            log::error("  cue {} has no clip", sim::kCueNames[i]);
            ++silentCues;
        }
    }
    if (silentCues > 0) {
        fail(std::format("{} of {} cues would be silent", silentCues,
                         static_cast<int>(bank.cueClip.size())));
    }
    for (size_t i = 0; i < bank.ambientClip.size(); ++i) {
        if (bank.ambientClip[i] < 0) {
            fail(std::format("ambient channel {} has no clip", i));
        }
    }
    // Nothing below can say anything useful without clips to play.
    if (bank.clips.empty()) {
        log::info("sound test: FAIL (no clips loaded from {})", soundDir);
        return 1;
    }

    const int explosionClip = bank.cueClip[static_cast<size_t>(sim::Cue::Explosion)];
    const int laserClip = bank.cueClip[static_cast<size_t>(sim::Cue::LaserFire)];
    if (explosionClip < 0 || laserClip < 0) {
        log::info("sound test: FAIL (the clips the checks below need are missing)");
        return 1;
    }

    Mixer mixer;
    mixer.setClips(&bank.clips);
    mixer.setMasterGain(1.0f);
    setListener(mixer, glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f));

    // --- silence ---------------------------------------------------------
    // A mixer with nothing playing must write zeros, not leave the buffer
    // alone. A device hands back whatever was in that memory last time, and
    // "forgot to clear the buffer" sounds exactly like a hardware fault.
    {
        Buffer quiet = renderBlocks(mixer, 4);
        if (peak(quiet.samples) != 0.0f) {
            fail(std::format("an idle mixer wrote {:.6f} instead of silence", peak(quiet.samples)));
        }
        if (mixer.activeVoices() != 0) {
            fail("an idle mixer has active voices");
        }
    }

    // --- distance --------------------------------------------------------
    const Attenuation att = mixer.attenuation;
    const std::array<float, 6> distances{{0.0f, att.reference, 40.0f, 90.0f, 160.0f,
                                          att.maximum + 10.0f}};
    std::array<float, 6> levels{};
    for (size_t i = 0; i < distances.size(); ++i) {
        Buffer b = playAt(mixer, explosionClip, glm::vec3(0.0f, 0.0f, -distances[i]), 100);
        levels[i] = rms(b.samples, 0) + rms(b.samples, 1);
    }
    log::info("  attenuation (ref {:.0f}, max {:.0f}): {:.3f} at 0, {:.3f} at {:.0f}, {:.3f} at 40, "
              "{:.3f} at 90, {:.3f} at 160, {:.3f} beyond max",
              att.reference, att.maximum, levels[0], levels[1], att.reference, levels[2], levels[3],
              levels[4], levels[5]);
    for (size_t i = 1; i < levels.size(); ++i) {
        if (levels[i] > levels[i - 1]) {
            fail(std::format("a sound at {:.0f} units is louder than one at {:.0f}", distances[i],
                             distances[i - 1]));
        }
    }
    if (levels[0] <= 0.0f) fail("a sound at the listener is silent");
    if (levels[5] != 0.0f) {
        fail(std::format("a sound past the maximum distance still mixes at {:.6f}", levels[5]));
    }

    // --- the stereo image ------------------------------------------------
    {
        // Far enough out that the close-range collapse to centre has fully
        // released; see Mixer::spatialise.
        const glm::vec3 right(60.0f, 0.0f, 0.0f);
        Buffer r = playAt(mixer, laserClip, right, 60);
        Buffer l = playAt(mixer, laserClip, -right, 60);
        Buffer c = playAt(mixer, laserClip, glm::vec3(0.0f, 0.0f, -60.0f), 60);

        const float rl = rms(r.samples, 0), rr = rms(r.samples, 1);
        const float ll = rms(l.samples, 0), lr = rms(l.samples, 1);
        const float cl = rms(c.samples, 0), cr = rms(c.samples, 1);
        log::info("  stereo: right source L {:.4f} R {:.4f}, left source L {:.4f} R {:.4f}, "
                  "ahead L {:.4f} R {:.4f}",
                  rl, rr, ll, lr, cl, cr);

        if (rr <= rl * 1.5f) fail("a sound to starboard is not clearly louder on the right");
        if (ll <= lr * 1.5f) fail("a sound to port is not clearly louder on the left");
        if (cl <= 0.0f || std::abs(cl - cr) > cl * 0.05f) {
            fail("a sound dead ahead is not centred");
        }

        // Rolling the ship inverts the stereo image. This is the check that the
        // pan is taken against the ship's own basis and not against world X: in
        // a 6DOF game, flying upside down must move a sound from one ear to the
        // other, and a world-space pan would leave it where it was.
        setListener(mixer, glm::vec3(0.0f), glm::vec3(-1.0f, 0.0f, 0.0f));
        Buffer rolled = playAt(mixer, laserClip, right, 60);
        const float xl = rms(rolled.samples, 0), xr = rms(rolled.samples, 1);
        log::info("  rolled 180 deg: same source L {:.4f} R {:.4f} (was L {:.4f} R {:.4f})", xl, xr,
                  rl, rr);
        if (xl <= xr * 1.5f) fail("rolling the ship did not swap the stereo image");
        setListener(mixer, glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    }

    // --- the voice pool --------------------------------------------------
    {
        const uint64_t stolenBefore = mixer.stolen();
        for (int i = 0; i < kMaxVoices * 3; ++i) {
            Command c;
            c.kind = Command::Kind::Play;
            c.clip = explosionClip;
            c.position = glm::vec3(static_cast<float>(i), 0.0f, 0.0f);
            c.gain = 1.0f;
            mixer.push(c);
        }
        Buffer b = renderBlocks(mixer, 1);
        const int active = mixer.activeVoices();
        log::info("  voice pool: {} requests, {} voices active (cap {}), {} stolen, peak {:.3f}",
                  kMaxVoices * 3, active, kMaxVoices, mixer.stolen() - stolenBefore,
                  peak(b.samples));
        if (active > kMaxVoices) fail("the mixer exceeded its own voice cap");
        if (mixer.stolen() <= stolenBefore) fail("an over-full voice pool stole nothing");
        // The soft clip is what keeps a room full of explosions from turning
        // into square waves. It is bounded by construction, so this failing
        // means the limiter was removed, not that it was overwhelmed.
        if (peak(b.samples) > 1.0f) {
            fail(std::format("the bus reached {:.4f}, past full scale", peak(b.samples)));
        }
        // Let them ring out so the next check starts from a quiet mixer.
        renderBlocks(mixer, 400);
    }

    // --- ambient channels ------------------------------------------------
    {
        const int engineClip = bank.ambientClip[static_cast<size_t>(Ambient::Engine)];
        Command c;
        c.kind = Command::Kind::SetAmbient;
        c.ambient = static_cast<uint8_t>(Ambient::Engine);
        c.clip = engineClip;
        c.gain = 1.0f;
        c.pitch = 1.0f;
        c.positional = false;
        mixer.push(c);
        renderBlocks(mixer, 1);
        const int afterStart = mixer.activeVoices();

        // Asking again must update the running voice, not stack a second copy
        // of the engine on top of the first.
        c.gain = 0.5f;
        mixer.push(c);
        renderBlocks(mixer, 1);
        const int afterUpdate = mixer.activeVoices();

        // Render well past the end of the clip: a loop is still going, a
        // one-shot is not.
        const float clipLength = engineClip >= 0 ? bank.clips[static_cast<size_t>(engineClip)].seconds()
                                                 : 0.0f;
        const int blocksPastEnd =
            static_cast<int>((clipLength * static_cast<float>(kSampleRate)) / kBlock) + 20;
        Buffer looped = renderBlocks(mixer, blocksPastEnd);
        const int afterLoop = mixer.activeVoices();
        const float loopLevel = rms(looped.samples, 0);

        c.clip = -1;
        c.gain = 0.0f;
        mixer.push(c);
        renderBlocks(mixer, 1);
        const int afterStop = mixer.activeVoices();

        log::info("  ambient: {} voice after start, {} after re-setting, {} after {:.2f}s (past the "
                  "clip's {:.2f}s, level {:.4f}), {} after stop",
                  afterStart, afterUpdate, afterLoop,
                  static_cast<double>(blocksPastEnd) * kBlock / kSampleRate, clipLength, loopLevel,
                  afterStop);
        if (afterStart != 1) fail("starting one ambient channel did not start exactly one voice");
        if (afterUpdate != 1) fail("re-setting an ambient channel layered a second voice");
        if (afterLoop != 1) fail("an ambient channel stopped at the end of its clip instead of looping");
        if (loopLevel <= 0.0f) fail("a looping ambient channel rendered silence");
        if (afterStop != 0) fail("stopping an ambient channel left a voice running");
    }

    // --- the heap --------------------------------------------------------
    {
        // The same rule main.cpp instruments on the sim step, for the same
        // reason and with more teeth: this runs on a thread with a hard
        // deadline, and an allocation that takes the lock in the middle of a
        // callback is an audible click rather than a slow frame.
        Buffer scratch(kBlock);
        for (int i = 0; i < 8; ++i) mixer.render(scratch.data(), kBlock);   // warm up

        dvk::alloc::Scope allocations;
        for (int i = 0; i < 200; ++i) {
            if (i % 8 == 0) {
                Command c;
                c.kind = Command::Kind::Play;
                c.clip = laserClip;
                c.position = glm::vec3(0.0f, 0.0f, -20.0f);
                mixer.push(c);
            }
            mixer.render(scratch.data(), kBlock);
        }
        // Read the counter before logging anything. std::format allocates, so a
        // check written after the log line measures the log line - which is how
        // this check first reported "no allocations" and then failed on one.
        const uint64_t count = allocations.count();
        const uint64_t bytes = allocations.bytes();
        log::info("  heap: {} allocation(s) over 200 mixed blocks with voices starting",
                  count == 0 ? "no" : std::to_string(count));
        if (count != 0) {
            fail(std::format("the mix path allocated {} time(s) ({} bytes)", count, bytes));
        }
    }

    // --- the command queue -----------------------------------------------
    {
        // Overflowing the queue must drop requests, not corrupt it. Pushing
        // more than it holds without rendering is the only way to reach that
        // state, and it is worth knowing it is survivable.
        int accepted = 0;
        for (int i = 0; i < kCommandQueueSize * 2; ++i) {
            Command c;
            c.kind = Command::Kind::Play;
            c.clip = laserClip;
            if (mixer.push(c)) ++accepted;
        }
        Buffer b = renderBlocks(mixer, 2);
        log::info("  command queue: {} of {} pushes accepted, {} voices after, peak {:.3f}",
                  accepted, kCommandQueueSize * 2, mixer.activeVoices(), peak(b.samples));
        if (accepted >= kCommandQueueSize * 2) fail("the command queue never reported full");
        if (accepted < kCommandQueueSize - 1) fail("the command queue filled earlier than its size");
        if (peak(b.samples) > 1.0f) fail("the bus passed full scale after a queue overflow");
    }

    log::info("sound test: {}", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace dvk::audio

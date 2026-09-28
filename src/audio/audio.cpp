#include "audio/audio.h"

#include <filesystem>
#include <fstream>

#include "audio/device.h"
#include "core/log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat3x3.hpp>

namespace dvk::audio {
namespace {

using sim::Cue;

// Move `value` towards `target` by at most `step`, never overshooting.
//
// Linear rather than the exponential glide the mixer uses for its own
// smoothing: a fade with a stated length should be finished when that length
// is up, and a one-pole only ever gets close to its target. Two seconds of
// "nearly silent" is still two seconds of the wrong track.
float approach(float value, float target, float step) {
    if (value < target) return std::min(target, value + step);
    return std::max(target, value - step);
}

// The mix, as a table. Gains here are relative to each other, not absolute:
// the master gain sets the level, and these say that a laser is quieter than a
// missile and that a door is quieter than either.
//
// Several cues share a file on purpose. A robot dying and a crate being
// destroyed are both something coming apart, and giving them separate files
// would mean two nearly identical recordings to keep in step.
constexpr std::array<CueDef, static_cast<size_t>(Cue::Count)> kCues{{
    {"laser.wav", 0.55f, 0.06f},          // LaserFire
    {"vulcan.wav", 0.45f, 0.11f},         // VulcanFire
    {"concussion.wav", 0.85f, 0.04f},     // ConcussionFire
    {"homing.wav", 0.85f, 0.04f},         // HomingFire
    {"railgun.wav", 0.90f, 0.03f},        // RailgunFire
    {"lightning.wav", 0.55f, 0.10f},      // LightningFire
    {"flare.wav", 0.40f, 0.05f},          // FlareThrow
    {"empty.wav", 0.35f, 0.02f},          // WeaponEmpty
    {"select.wav", 0.30f, 0.00f},         // WeaponSelect
    {"impact.wav", 0.45f, 0.14f},         // WallImpact
    {"explosion.wav", 0.95f, 0.09f},      // Explosion
    {"robot_hit.wav", 0.50f, 0.12f},      // RobotHit
    {"robot_fire.wav", 0.55f, 0.09f},     // RobotFire
    {"robot_alert.wav", 0.50f, 0.07f},    // RobotAlert
    {"explosion.wav", 0.90f, 0.10f},      // RobotDeath
    {"explosion.wav", 0.80f, 0.08f},      // ObjectDestroyed
    {"player_hurt.wav", 0.70f, 0.05f},    // PlayerHurt
    {"player_death.wav", 1.00f, 0.00f},   // PlayerDeath
    {"door_open.wav", 0.55f, 0.03f},      // DoorOpen
    {"door_close.wav", 0.55f, 0.03f},     // DoorClose
    {"locked.wav", 0.55f, 0.02f},         // DoorLocked
    {"pickup.wav", 0.55f, 0.04f},         // Pickup
    {"key_pickup.wav", 0.70f, 0.00f},     // KeyPickup
    {"reactor.wav", 1.00f, 0.00f},        // ReactorDestroyed
    {"countdown.wav", 0.10f, 0.00f},      // CountdownWarn
    {"exit.wav", 0.90f, 0.00f},           // Exit
    {"special_windup.wav", 0.70f, 0.00f},   // SpecialWindup
    {"attachment_break.wav", 1.00f, 0.04f}, // AttachmentBreak
}};

constexpr std::array<const char*, static_cast<size_t>(Ambient::Count)> kAmbientFiles{{
    "engine.wav",   // Engine
    "alarm.wav",    // Alarm
    "",             // Comm - supplied per transmission, see Bank::loadComm
    "",             // MusicLull - supplied by Bank::loadMusic
    "",             // MusicCombat - supplied by Bank::loadMusic
}};

// What the playlists hold when music.json is missing or unreadable: the two
// tracks the game shipped with, one a side. A mine with no soundtrack would be
// a strange way to report a typo in a config file.
constexpr std::array<const char*, 2> kMusicFallback{{
    "Sub-Bass Drones.ogg",
    "Mechanical Devastation.ogg",
}};

// The playlist file, beside the tracks it lists.
constexpr const char* kMusicPlaylist = "music.json";

// The two keys in it, in the order the crossfade runs between them.
constexpr std::array<const char*, 2> kMusicSides{{"lull", "combat"}};

// One side of the playlist as it was authored, before any of it is decoded.
struct PlaylistEntry {
    std::string file;
    std::string name;   // empty to take the name from the filename
};

// Reads one side out of the parsed json. An entry is either a filename or an
// object with `file` and an optional `name`; anything else in the array is
// skipped with a warning rather than taken as a reason to abandon the level's
// music.
std::vector<PlaylistEntry> readPlaylistSide(const nlohmann::json& source, const char* key) {
    std::vector<PlaylistEntry> entries;
    const auto side = source.find(key);
    if (side == source.end() || !side->is_array()) return entries;

    for (const nlohmann::json& entry : *side) {
        if (entry.is_string()) {
            entries.push_back({entry.get<std::string>(), ""});
            continue;
        }
        if (entry.is_object()) {
            const auto file = entry.find("file");
            if (file != entry.end() && file->is_string()) {
                PlaylistEntry parsed;
                parsed.file = file->get<std::string>();
                const auto name = entry.find("name");
                if (name != entry.end() && name->is_string()) parsed.name = name->get<std::string>();
                entries.push_back(std::move(parsed));
                continue;
            }
        }
        log::warn("sound: a '{}' playlist entry is neither a filename nor a file object", key);
    }
    return entries;
}

// Both sides of the playlist, from music.json in `root`. Falls back to the
// shipped pair when the file is absent, will not parse, or names nothing at
// all, so the only way to end up with no soundtrack is to ask for one.
std::array<std::vector<PlaylistEntry>, 2> readPlaylist(const std::string& root) {
    std::array<std::vector<PlaylistEntry>, 2> sides;
    const std::filesystem::path path = std::filesystem::path(root) / kMusicPlaylist;

    std::ifstream file(path);
    if (file) {
        try {
            const nlohmann::json source = nlohmann::json::parse(file);
            for (size_t i = 0; i < kMusicSides.size(); ++i) {
                sides[i] = readPlaylistSide(source, kMusicSides[i]);
            }
        } catch (const nlohmann::json::exception& error) {
            log::warn("sound: '{}' would not parse: {}", path.string(), error.what());
        }
    }

    if (sides[0].empty() && sides[1].empty()) {
        for (size_t i = 0; i < kMusicFallback.size(); ++i) {
            sides[i].push_back({kMusicFallback[i], ""});
        }
        log::info("sound: no playlist at '{}', using the two shipped tracks", path.string());
    }
    return sides;
}

} // namespace

const std::array<CueDef, static_cast<size_t>(Cue::Count)>& cueTable() {
    return kCues;
}

int Bank::commClip(std::string_view name) const {
    for (size_t i = 0; i < commNames.size(); ++i) {
        if (commNames[i] == name) return commClips[i];
    }
    return -1;
}

void Bank::loadComm(const std::string& root) {
    commNames.clear();
    commClips.clear();
    std::error_code error;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(root, error)) {
        if (!entry.is_directory()) continue;
        const std::filesystem::path wav = entry.path() / "comm.wav";
        // A transmission with no voice track is the ordinary case, not a
        // missing asset: the panel plays silently and nothing warns.
        if (!std::filesystem::is_regular_file(wav)) continue;
        Clip clip = loadWav(wav.string());
        if (!clip.valid()) {
            ++missing;
            continue;
        }
        resample(clip, kSampleRate);
        clips.push_back(std::move(clip));
        commNames.push_back(entry.path().filename().string());
        commClips.push_back(static_cast<int>(clips.size() - 1));
    }
    if (error) {
        log::warn("sound: comm root '{}' would not be read: {}", root, error.message());
    }
    log::info("sound: {} transmission voice track(s) from '{}'", commNames.size(), root);
}

const std::vector<MusicTrack>& Bank::music(int side) const {
    static const std::vector<MusicTrack> none;
    return side == 0 || side == 1 ? musicTracks[static_cast<size_t>(side)] : none;
}

void Bank::loadMusic(const std::string& root) {
    for (std::vector<MusicTrack>& side : musicTracks) side.clear();

    const std::array<std::vector<PlaylistEntry>, 2> playlist = readPlaylist(root);
    for (size_t i = 0; i < playlist.size(); ++i) {
        for (const PlaylistEntry& entry : playlist[i]) {
            const std::filesystem::path path = std::filesystem::path(root) / entry.file;
            Clip clip = loadClip(path.string());
            if (!clip.valid()) {
                ++missing;
                continue;
            }
            resample(clip, kSampleRate);
            clips.push_back(std::move(clip));
            // A track only joins the playlist once its samples are in the
            // bank, so a name on screen is always a name of something
            // audible and an index into the list is always playable.
            MusicTrack track;
            track.clip = static_cast<int>(clips.size() - 1);
            track.name = entry.name.empty() ? path.stem().string() : entry.name;
            musicTracks[i].push_back(std::move(track));
        }
        // The channel's resting clip, for the generic ambient path. The
        // adaptive one names its clip in every command, so this only matters
        // to a caller that reaches for setAmbient directly.
        const Ambient channel = i == 0 ? Ambient::MusicLull : Ambient::MusicCombat;
        ambientClip[static_cast<size_t>(channel)] =
            musicTracks[i].empty() ? -1 : musicTracks[i].front().clip;
    }

    log::info("sound: {} lull and {} combat track(s) from '{}'", musicTracks[0].size(),
              musicTracks[1].size(), root);
}

void Bank::load(const std::string& dir) {
    clips.clear();
    cueClip.fill(-1);
    ambientClip.fill(-1);
    for (std::vector<MusicTrack>& side : musicTracks) side.clear();
    missing = 0;

    // Decode each distinct filename once, then point every cue that named it at
    // the same clip.
    std::vector<std::string> loaded;
    const auto clipFor = [&](const char* file) -> int {
        if (!file || !*file) return -1;
        for (size_t i = 0; i < loaded.size(); ++i) {
            if (loaded[i] == file) return static_cast<int>(i);
        }
        Clip clip = loadWav(dir + "/" + file);
        if (!clip.valid()) {
            ++missing;
            return -1;
        }
        resample(clip, kSampleRate);
        loaded.emplace_back(file);
        clips.push_back(std::move(clip));
        return static_cast<int>(clips.size() - 1);
    };

    for (size_t i = 0; i < kCues.size(); ++i) {
        cueClip[i] = clipFor(kCues[i].file);
    }
    for (size_t i = 0; i < kAmbientFiles.size(); ++i) {
        ambientClip[i] = clipFor(kAmbientFiles[i]);
    }
}

Audio::Audio() = default;

Audio::~Audio() {
    shutdown();
}

bool Audio::init(const std::string& soundDir, bool openDevice, const std::string& commDir,
                 const std::string& musicDir) {
    bank_.load(soundDir);
    // Before setClips, and before anything can be playing: a voice holds a
    // pointer into bank_.clips, and appending to that vector after one exists
    // would leave it pointing at freed memory.
    if (!commDir.empty()) bank_.loadComm(commDir);
    if (!musicDir.empty()) bank_.loadMusic(musicDir);
    mixer_.setClips(&bank_.clips);

    int silent = 0;
    for (int index : bank_.cueClip) silent += index < 0 ? 1 : 0;
    if (silent > 0) {
        log::warn("sound: {} of {} cues have no clip and will be silent", silent,
                  static_cast<int>(bank_.cueClip.size()));
    }

    if (!openDevice) return false;

    device_ = std::make_unique<Device>();
    deviceOpen_ = device_->open(mixer_);
    if (!deviceOpen_) {
        // A container with no sound card is the ordinary case here, not a
        // failure worth stopping for. The mixer still runs; nothing pulls from
        // it, and the game is silent.
        log::warn("sound: no playback device, running silently");
        device_.reset();
        return false;
    }
    log::info("sound: {} clips, {} voices, {} Hz on \"{}\"", bank_.clips.size(), kMaxVoices,
              kSampleRate, device_->name());
    return true;
}

void Audio::shutdown() {
    if (device_) {
        device_->close();
        device_.reset();
    }
    deviceOpen_ = false;
}

void Audio::setListener(const glm::vec3& position, const glm::quat& orientation) {
    const glm::mat3 basis = glm::mat3_cast(orientation);
    Command command;
    command.kind = Command::Kind::SetListener;
    command.listener.position = position;
    command.listener.right = basis * glm::vec3(1.0f, 0.0f, 0.0f);
    command.listener.forward = basis * glm::vec3(0.0f, 0.0f, -1.0f);
    mixer_.push(command);
}

float Audio::jitter(float half) {
    if (half <= 0.0f) return 1.0f;
    // xorshift, local to the audio system. Nothing here feeds back into the
    // simulation, so this deliberately does not share the sim's generator -
    // drawing from that would make a replay depend on how many sounds played.
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    const float unit = static_cast<float>(rng_ & 0xFFFFFFu) / 16777215.0f;   // 0..1
    return 1.0f + (unit * 2.0f - 1.0f) * half;
}

void Audio::play(sim::Cue cue, const glm::vec3& position, float gain) {
    const auto index = static_cast<size_t>(cue);
    if (index >= bank_.cueClip.size()) return;
    const int clip = bank_.cueClip[index];
    if (clip < 0) return;

    Command command;
    command.kind = Command::Kind::Play;
    command.clip = clip;
    command.position = position;
    command.gain = kCues[index].gain * gain;
    command.pitch = jitter(kCues[index].pitchJitter);
    command.positional = true;
    mixer_.push(command);
}

void Audio::playLocal(sim::Cue cue, float gain) {
    const auto index = static_cast<size_t>(cue);
    if (index >= bank_.cueClip.size()) return;
    const int clip = bank_.cueClip[index];
    if (clip < 0) return;

    Command command;
    command.kind = Command::Kind::Play;
    command.clip = clip;
    command.gain = kCues[index].gain * gain;
    command.pitch = jitter(kCues[index].pitchJitter);
    command.positional = false;
    mixer_.push(command);
}

void Audio::submit(const sim::EventQueue& events) {
    for (int i = 0; i < events.count; ++i) {
        const sim::SoundEvent& e = events.items[static_cast<size_t>(i)];
        const auto index = static_cast<size_t>(e.cue);
        if (index < played_.size()) ++played_[index];
        if (e.atListener) {
            playLocal(e.cue, e.gain);
        } else {
            play(e.cue, e.position, e.gain);
        }
    }
    // Assigned, not accumulated: EventQueue::dropped is a running total for the
    // life of the queue, and clear() deliberately does not reset it.
    dropped_ = static_cast<uint32_t>(events.dropped);
}

uint32_t Audio::totalPlayed() const {
    uint32_t total = 0;
    for (uint32_t n : played_) total += n;
    return total;
}

void Audio::setAmbient(Ambient channel, float gain, float pitch) {
    const auto index = static_cast<size_t>(channel);
    if (index >= bank_.ambientClip.size()) return;

    Command command;
    command.kind = Command::Kind::SetAmbient;
    command.ambient = static_cast<uint8_t>(index);
    command.clip = gain > 0.0f ? bank_.ambientClip[index] : -1;
    command.gain = gain;
    command.pitch = pitch;
    // Ambient channels ride with the listener: the engine is the player's own
    // ship and the klaxon is the whole mine.
    command.positional = false;
    mixer_.push(command);
}

void Audio::playComm(int clip, float gain) {
    Command command;
    command.kind = Command::Kind::SetAmbient;
    command.ambient = static_cast<uint8_t>(Ambient::Comm);
    command.clip = gain > 0.0f ? clip : -1;
    command.gain = gain;
    command.pitch = 1.0f;
    // A voice in the player's own cockpit, not a point in the mine.
    command.positional = false;
    // The channel is for the protection, not the repetition.
    command.loop = false;
    mixer_.push(command);
}

void Audio::stopComm() {
    playComm(-1, 0.0f);
}

const std::string& Audio::musicTrackName(int side) const {
    static const std::string none;
    const std::vector<MusicTrack>& tracks = bank_.music(side);
    if (tracks.empty()) return none;
    const size_t index = static_cast<size_t>(musicTrackIndex_[static_cast<size_t>(side)]) %
                         tracks.size();
    return tracks[index].name;
}

bool Audio::nextMusicTrack(int side) {
    if (side != 0 && side != 1) return false;
    const size_t which = static_cast<size_t>(side);
    const std::vector<MusicTrack>& tracks = bank_.music(side);
    // One track is not a playlist, and a skip already diving is not worth
    // interrupting: the second press would restart the dip from wherever the
    // first had got to and the player would hear the music hesitate.
    if (tracks.size() < 2 || musicPendingTrack_[which] >= 0) return false;

    musicPendingTrack_[which] =
        static_cast<int>((static_cast<size_t>(musicTrackIndex_[which]) + 1) % tracks.size());
    return true;
}

void Audio::cueMusicTrack(size_t side, int index) {
    if (!musicRunning_) {
        musicTrackIndex_[side] = index;
        musicPendingTrack_[side] = -1;
        musicSwapGain_[side] = 1.0f;
        return;
    }
    if (index == musicTrackIndex_[side] && musicPendingTrack_[side] < 0) return;
    musicPendingTrack_[side] = index;
}

void Audio::shuffleMusicTracks() {
    // Seeded from the device rather than from rng_, which is fixed so cue
    // variation replays the same way. The level's opening track is meant to
    // differ from one launch to the next.
    std::random_device device;
    std::minstd_rand pick(device());
    for (size_t side = 0; side < musicTrackIndex_.size(); ++side) {
        const size_t count = bank_.music(static_cast<int>(side)).size();
        if (count == 0) continue;
        cueMusicTrack(side, static_cast<int>(pick() % count));
    }
}

void Audio::advanceMusicTracks() {
    for (size_t side = 0; side < musicTrackIndex_.size(); ++side) {
        const size_t count = bank_.music(static_cast<int>(side)).size();
        if (count < 2) continue;
        // From wherever a skip still in flight was heading, so a restart
        // pressed mid-dip lands one past it rather than on it.
        const int from = musicPendingTrack_[side] >= 0 ? musicPendingTrack_[side]
                                                       : musicTrackIndex_[side];
        cueMusicTrack(side, static_cast<int>((static_cast<size_t>(from) + 1) % count));
    }
}

void Audio::setMusicState(float combat, float volume, bool commDucked, float dt) {
    // Two seconds each way. The transition reads as one crossfade rather than
    // as two different gestures, and the soundtrack takes the same time to
    // notice a fight as it does to let one go.
    constexpr float kCombatFadeIn = 2.00f;
    constexpr float kCombatFadeOut = 2.00f;
    constexpr float kDuckGain = 0.22f;
    constexpr float kDuckAttack = 0.12f;
    constexpr float kDuckRelease = 0.70f;
    // The skip: down fast, because the track being left is the one the player
    // has asked to stop hearing, and back up more slowly so the new one
    // arrives rather than appears.
    constexpr float kSwapOut = 0.15f;
    constexpr float kSwapIn = 0.30f;

    combat = std::clamp(combat, 0.0f, 1.0f);
    volume = std::clamp(volume, 0.0f, 1.0f);
    dt = std::max(dt, 0.0f);

    const float duckTarget = commDucked ? kDuckGain : 1.0f;
    if (!musicRunning_) {
        // A level that opens in combat opens in combat. Fading in from the
        // lull here would be a transition between two states, one of which
        // the player was never in.
        musicCombatMix_ = combat;
        musicDuck_ = duckTarget;
    } else {
        const float fade = combat > musicCombatMix_ ? kCombatFadeIn : kCombatFadeOut;
        musicCombatMix_ = approach(musicCombatMix_, combat, dt / fade);
        const float duckFade = duckTarget < musicDuck_ ? kDuckAttack : kDuckRelease;
        musicDuck_ = approach(musicDuck_, duckTarget, dt * (1.0f - kDuckGain) / duckFade);
    }

    // Equal power, so the crossfade has no hole in the middle: at the halfway
    // point both tracks are at about 0.71 rather than 0.5, and the pair keeps
    // a constant total.
    const float angle = musicCombatMix_ * 0.5f * 3.14159265358979323846f;
    const std::array<float, 2> gains{{
        volume * musicDuck_ * std::cos(angle),
        volume * musicDuck_ * std::sin(angle),
    }};
    constexpr std::array<Ambient, 2> channels{{Ambient::MusicLull, Ambient::MusicCombat}};

    for (size_t i = 0; i < channels.size(); ++i) {
        // The skip envelope, walked the same way and on the same clock as the
        // crossfade above. The track index only moves at the bottom, where the
        // channel is silent, so the mixer's restart-from-the-top is inaudible
        // rather than a click in the middle of a waveform.
        if (musicPendingTrack_[i] >= 0) {
            musicSwapGain_[i] = approach(musicSwapGain_[i], 0.0f, dt / kSwapOut);
            if (musicSwapGain_[i] <= 0.0f) {
                musicTrackIndex_[i] = musicPendingTrack_[i];
                musicPendingTrack_[i] = -1;
            }
        } else if (musicSwapGain_[i] < 1.0f) {
            musicSwapGain_[i] = approach(musicSwapGain_[i], 1.0f, dt / kSwapIn);
        }

        const std::vector<MusicTrack>& tracks = bank_.music(static_cast<int>(i));
        const int clip =
            tracks.empty()
                ? -1
                : tracks[static_cast<size_t>(musicTrackIndex_[i]) % tracks.size()].clip;

        Command command;
        command.kind = Command::Kind::SetAmbient;
        command.ambient = static_cast<uint8_t>(channels[i]);
        command.clip = clip;
        command.gain = gains[i] * musicSwapGain_[i];
        command.positional = false;
        command.loop = true;
        command.keepAliveAtZero = true;
        // The envelopes above are the transition, so the mixer only has to
        // keep a frame's step from clicking. Asking it for a long glide as
        // well would be two smoothers fighting over the same gain - and its
        // own is overwritten every frame by the next command anyway, which is
        // what used to collapse a multi-second fade into the anti-zipper
        // default one frame in.
        command.gainGlideSeconds = 0.0f;
        mixer_.push(command);
    }

    musicRunning_ = true;
}

void Audio::stopMusic() {
    if (!musicRunning_) return;
    for (Ambient channel : {Ambient::MusicLull, Ambient::MusicCombat}) {
        Command command;
        command.kind = Command::Kind::SetAmbient;
        command.ambient = static_cast<uint8_t>(channel);
        command.clip = -1;
        command.gain = 0.0f;
        mixer_.push(command);
    }
    musicRunning_ = false;
    musicCombatMix_ = 0.0f;
    musicDuck_ = 1.0f;
    // A skip that was still diving when the music stopped has nothing left to
    // dive for, so the envelope is put back rather than resumed into a level
    // the player is no longer in. Which track each side had reached is kept
    // until the shell picks the next level's tracks: see shuffleMusicTracks
    // and advanceMusicTracks.
    musicSwapGain_.fill(1.0f);
    musicPendingTrack_.fill(-1);
}

void Audio::setMuted(bool muted) {
    if (muted == muted_) return;
    muted_ = muted;
    if (muted) {
        unmutedGain_ = mixer_.masterGain();
        mixer_.setMasterGain(0.0f);
    } else {
        mixer_.setMasterGain(unmutedGain_);
    }
}

void Audio::setMasterGain(float gain) {
    unmutedGain_ = gain;
    mixer_.setMasterGain(muted_ ? 0.0f : gain);
}

} // namespace dvk::audio

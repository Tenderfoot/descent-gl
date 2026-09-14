#pragma once

#include <string>

namespace dvk::audio {

class Mixer;

// The playback device, and the only thing in the project that knows miniaudio
// exists. Everything else in src/audio is plain C++ over plain arrays.
//
// This is the same containment render/gl.h gives glad, for the same reason:
// one place to look when the platform layer misbehaves, and a mixer that can be
// tested without one.
class Device {
public:
    Device();
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    // Starts a stereo float playback device at kSampleRate that pulls from
    // `mixer`. The mixer must outlive the device. Returns false if no device
    // could be opened - which in a headless container is the expected answer,
    // not a failure.
    bool open(Mixer& mixer);
    void close();

    bool isOpen() const { return open_; }
    const std::string& name() const { return name_; }

private:
    // miniaudio's ma_device is a large concrete struct and its address must not
    // change, so it lives behind a pointer rather than in a member whose type
    // would drag miniaudio.h into every file that includes this one.
    struct Impl;
    Impl* impl_ = nullptr;
    bool open_ = false;
    std::string name_ = "none";
};

} // namespace dvk::audio

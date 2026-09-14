#include "audio/device.h"

#include "audio/mixer.h"
#include "core/log.h"

// The only translation unit that compiles miniaudio. Everything not needed is
// switched off before the include: this project reads WAV files with its own
// loader (see audio/wav.cpp) and does its own mixing, so all miniaudio does
// here is open a device and call back for samples.
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
// No null backend. miniaudio tries it last and it always succeeds, so leaving
// it in would mean a container with no sound card reporting a working device
// and the game claiming to have sound it is not producing. Without it, such a
// machine fails to open, which is what Audio::init expects and warns about.
#define MA_NO_NULL
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996 4244 4267 4456 4457 4459 4189 4100)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include <miniaudio.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace dvk::audio {
namespace {

// Called on a thread owned by the audio driver, not the game thread. Whatever
// happens in here happens under a hard deadline: return late and the driver
// plays whatever was left in the buffer, which is an audible click. That is why
// Mixer::render allocates nothing, takes no lock, and reads its work from a
// lock-free queue.
void dataCallback(ma_device* device, void* output, const void* input, ma_uint32 frameCount) {
    (void)input;
    auto* mixer = static_cast<Mixer*>(device->pUserData);
    if (!mixer) return;
    mixer->render(static_cast<float*>(output), static_cast<int>(frameCount));
}

} // namespace

struct Device::Impl {
    ma_device device{};
};

Device::Device() = default;

Device::~Device() {
    close();
}

bool Device::open(Mixer& mixer) {
    close();
    impl_ = new Impl();

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = kChannels;
    config.sampleRate = kSampleRate;
    config.dataCallback = dataCallback;
    config.pUserData = &mixer;

    if (ma_device_init(nullptr, &config, &impl_->device) != MA_SUCCESS) {
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        ma_device_uninit(&impl_->device);
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    // The device may not have given us the rate we asked for. The mixer has one
    // rate compiled in, so a device that refused it would play everything at the
    // wrong pitch; better to say so than to sound subtly wrong.
    if (impl_->device.sampleRate != kSampleRate) {
        log::warn("sound: device opened at {} Hz, not {}; miniaudio is resampling",
                  impl_->device.sampleRate, kSampleRate);
    }

    name_ = impl_->device.playback.name[0] != '\0' ? impl_->device.playback.name : "default";
    open_ = true;
    return true;
}

void Device::close() {
    if (impl_) {
        if (open_) ma_device_uninit(&impl_->device);
        delete impl_;
        impl_ = nullptr;
    }
    open_ = false;
    name_ = "none";
}

} // namespace dvk::audio

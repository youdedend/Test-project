// SPDX-License-Identifier: GPL-2.0-or-later
// Native UWP XAudio2.9 Stereo Float Output Sink for src/runtime_audio.c.
// Avoids DirectSound and XACT (both unsupported in Xbox Dev Mode AppContainer).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace bbport::uwp {

class XAudio2Sink {
public:
    static XAudio2Sink& Instance();

    bool Initialize(int sample_rate, int channels);
    void Shutdown();

    // Pushes interleaved float32 samples; returns queued bytes before this push for pacing.
    int PushSamples(const float* interleaved_samples, int byte_len);
    int QueuedBytes() const;

private:
    XAudio2Sink() = default;
    ~XAudio2Sink();

    struct RingChunk {
        std::vector<float> samples;
    };

    mutable std::mutex mutex_;
    int sample_rate_ = 48000;
    int channels_ = 2;
    bool initialized_ = false;

    static constexpr size_t kPoolSlots = 24;
    RingChunk slots_[kPoolSlots]{};
    size_t write_slot_ = 0;
    std::atomic<int> queued_bytes_{0};

#if defined(_WIN32)
    struct Impl;
    Impl* impl_ = nullptr;
#endif
};

} // namespace bbport::uwp

#ifdef __cplusplus
extern "C" {
#endif
int  bbport_uwp_xaudio2_init(int sample_rate, int channels);
int  bbport_uwp_xaudio2_push(const float* interleaved_samples, int byte_len);
int  bbport_uwp_xaudio2_queued_bytes(void);
void bbport_uwp_xaudio2_shutdown(void);
#ifdef __cplusplus
}
#endif

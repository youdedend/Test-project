// SPDX-License-Identifier: GPL-2.0-or-later
#include "XAudio2Sink.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#include <xaudio2.h>
#endif

namespace bbport::uwp {

#if defined(_WIN32)
struct XAudio2Sink::Impl : public IXAudio2VoiceCallback {
    IXAudio2* engine = nullptr;
    IXAudio2MasteringVoice* mastering = nullptr;
    IXAudio2SourceVoice* source = nullptr;
    std::atomic<int>* queued_bytes_ref = nullptr;

    explicit Impl(std::atomic<int>* qb) : queued_bytes_ref(qb) {}

    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void* context) override {
        const auto byte_count = static_cast<int>(reinterpret_cast<uintptr_t>(context));
        if (queued_bytes_ref && byte_count > 0) {
            const int prev = queued_bytes_ref->fetch_sub(byte_count, std::memory_order_relaxed);
            if (prev < byte_count) {
                queued_bytes_ref->store(0, std::memory_order_relaxed);
            }
        }
    }
};
#endif

XAudio2Sink& XAudio2Sink::Instance() {
    static XAudio2Sink inst;
    return inst;
}

XAudio2Sink::~XAudio2Sink() {
    Shutdown();
}

bool XAudio2Sink::Initialize(int sample_rate, int channels) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return true;

    sample_rate_ = (sample_rate > 0) ? sample_rate : 48000;
    channels_    = (channels > 0) ? channels : 2;
    queued_bytes_.store(0, std::memory_order_relaxed);

#if defined(_WIN32)
    impl_ = new Impl(&queued_bytes_);
    HRESULT hr = XAudio2Create(&impl_->engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !impl_->engine) {
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    hr = impl_->engine->CreateMasteringVoice(&impl_->mastering,
                                             static_cast<UINT32>(channels_),
                                             static_cast<UINT32>(sample_rate_));
    if (FAILED(hr) || !impl_->mastering) {
        impl_->engine->Release();
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    WAVEFORMATEX wfx{};
    wfx.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels       = static_cast<WORD>(channels_);
    wfx.nSamplesPerSec  = static_cast<DWORD>(sample_rate_);
    wfx.wBitsPerSample  = 32;
    wfx.nBlockAlign     = static_cast<WORD>((wfx.nChannels * wfx.wBitsPerSample) / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    hr = impl_->engine->CreateSourceVoice(&impl_->source, &wfx, 0,
                                          XAUDIO2_DEFAULT_FREQ_RATIO, impl_);
    if (FAILED(hr) || !impl_->source) {
        impl_->mastering->DestroyVoice();
        impl_->engine->Release();
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    impl_->source->Start(0);
#endif

    initialized_ = true;
    return true;
}

void XAudio2Sink::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return;

#if defined(_WIN32)
    if (impl_) {
        if (impl_->source) {
            impl_->source->Stop(0);
            impl_->source->FlushSourceBuffers();
            impl_->source->DestroyVoice();
        }
        if (impl_->mastering) {
            impl_->mastering->DestroyVoice();
        }
        if (impl_->engine) {
            impl_->engine->Release();
        }
        delete impl_;
        impl_ = nullptr;
    }
#endif

    queued_bytes_.store(0, std::memory_order_relaxed);
    initialized_ = false;
}

int XAudio2Sink::PushSamples(const float* interleaved_samples, int byte_len) {
    if (!interleaved_samples || byte_len <= 0) {
        return queued_bytes_.load(std::memory_order_relaxed);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return -1;

    const int before = queued_bytes_.load(std::memory_order_relaxed);

#if defined(_WIN32)
    if (impl_ && impl_->source) {
        XAUDIO2_VOICE_STATE state{};
        impl_->source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (state.BuffersQueued >= kPoolSlots - 1) {
            return before;
        }

        const size_t float_count = static_cast<size_t>(byte_len) / sizeof(float);
        auto& slot = slots_[write_slot_];
        write_slot_ = (write_slot_ + 1) % kPoolSlots;
        slot.samples.assign(interleaved_samples, interleaved_samples + float_count);

        XAUDIO2_BUFFER buf{};
        buf.AudioBytes = static_cast<UINT32>(slot.samples.size() * sizeof(float));
        buf.pAudioData = reinterpret_cast<const BYTE*>(slot.samples.data());
        buf.pContext   = reinterpret_cast<void*>(static_cast<uintptr_t>(buf.AudioBytes));

        if (SUCCEEDED(impl_->source->SubmitSourceBuffer(&buf))) {
            queued_bytes_.fetch_add(static_cast<int>(buf.AudioBytes), std::memory_order_relaxed);
        }
    }
#else
    auto& slot = slots_[write_slot_];
    write_slot_ = (write_slot_ + 1) % kPoolSlots;
    const size_t float_count = static_cast<size_t>(byte_len) / sizeof(float);
    slot.samples.assign(interleaved_samples, interleaved_samples + float_count);
#endif

    return before;
}

int XAudio2Sink::QueuedBytes() const {
    return queued_bytes_.load(std::memory_order_relaxed);
}

} // namespace bbport::uwp

extern "C" int bbport_uwp_xaudio2_init(int sample_rate, int channels) {
    return bbport::uwp::XAudio2Sink::Instance().Initialize(sample_rate, channels) ? 0 : -1;
}

extern "C" int bbport_uwp_xaudio2_push(const float* interleaved_samples, int byte_len) {
    return bbport::uwp::XAudio2Sink::Instance().PushSamples(interleaved_samples, byte_len);
}

extern "C" int bbport_uwp_xaudio2_queued_bytes(void) {
    return bbport::uwp::XAudio2Sink::Instance().QueuedBytes();
}

extern "C" void bbport_uwp_xaudio2_shutdown(void) {
    bbport::uwp::XAudio2Sink::Instance().Shutdown();
}

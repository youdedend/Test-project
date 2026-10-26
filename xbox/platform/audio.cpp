// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio.h"
#include <windows.h>
#include <mmreg.h>
#include <winrt/base.h>
#include <wrl/client.h>
#include <xaudio2.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
constexpr size_t kBufferSlots = 64;

std::mutex g_engine_mutex;
ComPtr<IXAudio2> g_engine;
IXAudio2MasteringVoice* g_master = nullptr;
int g_device_channels = 2;

bool EnsureEngineLocked() {
    if (g_engine && g_master) return true;
    thread_local bool apartment = false;
    if (!apartment) {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (winrt::hresult_error const&) {
        }
        apartment = true;
    }
    if (FAILED(XAudio2Create(&g_engine, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
        g_engine.Reset();
        return false;
    }
    if (FAILED(g_engine->CreateMasteringVoice(&g_master))) {
        g_master = nullptr;
        g_engine.Reset();
        return false;
    }
    DWORD mask = 0;
    if (SUCCEEDED(g_master->GetChannelMask(&mask)) && mask != 0) {
        int bits = 0;
        for (DWORD m = mask; m; m >>= 1) bits += int(m & 1u);
        g_device_channels = bits >= 8 ? 8 : 2;
    } else {
        g_device_channels = 2;
    }
    std::printf("Audio: XAudio2 mastering voice ready (%d channels)\n", g_device_channels);
    return true;
}

struct Voice final : IXAudio2VoiceCallback {
    IXAudio2SourceVoice* source = nullptr;
    std::vector<uint8_t> slots[kBufferSlots];
    size_t write_index = 0;
    std::atomic<int> queued_bytes{0};

    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
    void STDMETHODCALLTYPE OnBufferEnd(void* context) noexcept override {
        const auto bytes = static_cast<int>(reinterpret_cast<uintptr_t>(context));
        queued_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    }
    void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) noexcept override {}
};
} // namespace

extern "C" int bbport_uwp_xaudio2_device_channels(void) {
    std::scoped_lock lock(g_engine_mutex);
    EnsureEngineLocked();
    return g_device_channels;
}

extern "C" SDL_AudioStream* bbport_uwp_xaudio2_open(int channels, int is_float, int sample_rate) {
    if (channels != 1 && channels != 2 && channels != 8) return nullptr;
    if (sample_rate <= 0) sample_rate = 48000;

    std::scoped_lock lock(g_engine_mutex);
    if (!EnsureEngineLocked()) return nullptr;

    auto* voice = new Voice();
    const WORD bits = is_float ? 32 : 16;
    const WORD block_align = static_cast<WORD>(channels * (bits / 8));

    WAVEFORMATEXTENSIBLE ext{};
    ext.Format.wFormatTag = (channels > 2) ? WAVE_FORMAT_EXTENSIBLE
                                           : (is_float ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM);
    ext.Format.nChannels = static_cast<WORD>(channels);
    ext.Format.nSamplesPerSec = static_cast<DWORD>(sample_rate);
    ext.Format.wBitsPerSample = bits;
    ext.Format.nBlockAlign = block_align;
    ext.Format.nAvgBytesPerSec = static_cast<DWORD>(sample_rate) * block_align;
    if (channels > 2) {
        constexpr DWORD kSurround7Point1 = 0x63fu;
        ext.Format.cbSize = sizeof(ext) - sizeof(ext.Format);
        ext.Samples.wValidBitsPerSample = bits;
        ext.dwChannelMask = kSurround7Point1;
        ext.SubFormat = {is_float ? 3u : 1u, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
    }

    if (FAILED(g_engine->CreateSourceVoice(&voice->source, &ext.Format, 0,
                                           XAUDIO2_DEFAULT_FREQ_RATIO, voice)) ||
        FAILED(voice->source->Start(0))) {
        if (voice->source) voice->source->DestroyVoice();
        delete voice;
        return nullptr;
    }
    return reinterpret_cast<SDL_AudioStream*>(voice);
}

extern "C" void bbport_uwp_xaudio2_close(SDL_AudioStream* handle) {
    if (!handle) return;
    auto* voice = reinterpret_cast<Voice*>(handle);
    std::scoped_lock lock(g_engine_mutex);
    if (voice->source) {
        voice->source->Stop(0);
        voice->source->FlushSourceBuffers();
        voice->source->DestroyVoice();
        voice->source = nullptr;
    }
    delete voice;
}

extern "C" int bbport_uwp_xaudio2_queued_bytes(SDL_AudioStream* handle) {
    if (!handle) return 0;
    auto* voice = reinterpret_cast<Voice*>(handle);
    return std::max(0, voice->queued_bytes.load(std::memory_order_relaxed));
}

extern "C" void bbport_uwp_xaudio2_submit(SDL_AudioStream* handle, const void* data, int bytes) {
    if (!handle || !data || bytes <= 0) return;
    auto* voice = reinterpret_cast<Voice*>(handle);
    if (!voice->source) return;

    XAUDIO2_VOICE_STATE state{};
    voice->source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (state.BuffersQueued >= kBufferSlots - 1) return;

    auto& slot = voice->slots[voice->write_index];
    voice->write_index = (voice->write_index + 1) % kBufferSlots;
    slot.resize(static_cast<size_t>(bytes));
    std::memcpy(slot.data(), data, static_cast<size_t>(bytes));

    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = static_cast<UINT32>(bytes);
    buffer.pAudioData = slot.data();
    buffer.pContext = reinterpret_cast<void*>(static_cast<uintptr_t>(bytes));
    voice->queued_bytes.fetch_add(bytes, std::memory_order_relaxed);
    if (FAILED(voice->source->SubmitSourceBuffer(&buffer))) {
        voice->queued_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    }
}

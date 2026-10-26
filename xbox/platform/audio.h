// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
struct SDL_AudioStream;

#ifdef __cplusplus
extern "C" {
#endif
SDL_AudioStream *bbport_uwp_xaudio2_open(int channels, int is_float, int sample_rate);
void bbport_uwp_xaudio2_close(SDL_AudioStream *voice);
int bbport_uwp_xaudio2_queued_bytes(SDL_AudioStream *voice);
void bbport_uwp_xaudio2_submit(SDL_AudioStream *voice, const void *data, int bytes);
int bbport_uwp_xaudio2_device_channels(void);
#ifdef __cplusplus
}
#endif

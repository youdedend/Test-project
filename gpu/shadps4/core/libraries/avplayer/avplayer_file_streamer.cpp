// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm> // std::max, std::min
#include <cstring>
#include "core/libraries/avplayer/avplayer_file_streamer.h"

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#ifdef BB_UWP
extern "C" void* runtime_low_calloc(size_t count, size_t size);
extern "C" void runtime_low_free(void* ptr);
#endif

constexpr u32 AVPLAYER_AVIO_BUFFER_SIZE = 4096;
#ifdef BB_UWP
constexpr u32 AVPLAYER_LOW_BOUNCE_SIZE = 65536;
#endif

namespace Libraries::AvPlayer {

AvPlayerFileStreamer::AvPlayerFileStreamer(const AvPlayerFileReplacement& file_replacement)
    : m_file_replacement(file_replacement) {}

AvPlayerFileStreamer::~AvPlayerFileStreamer() {
    if (m_avio_context != nullptr) {
        avio_context_free(&m_avio_context);
    }
    if (m_file_replacement.close != nullptr && m_fd >= 0) {
        const auto close = m_file_replacement.close;
        const auto ptr = m_file_replacement.object_ptr;
        close(ptr);
    }
#ifdef BB_UWP
    if (m_low_bounce != nullptr) {
        runtime_low_free(m_low_bounce);
        m_low_bounce = nullptr;
        m_low_bounce_cap = 0;
    }
#endif
}

bool AvPlayerFileStreamer::Init(std::string_view path) {
    const auto ptr = m_file_replacement.object_ptr;
#ifdef BB_UWP
    char* low_path = static_cast<char*>(runtime_low_calloc(1, path.size() + 1));
    if (!low_path) {
        return false;
    }
    if (!path.empty()) {
        std::memcpy(low_path, path.data(), path.size());
    }
    low_path[path.size()] = '\0';
    m_fd = m_file_replacement.open(ptr, low_path);
    runtime_low_free(low_path);
    if (m_fd < 0) {
        return false;
    }
    if (!m_low_bounce) {
        m_low_bounce = static_cast<u8*>(runtime_low_calloc(1, AVPLAYER_LOW_BOUNCE_SIZE));
        if (m_low_bounce) {
            m_low_bounce_cap = AVPLAYER_LOW_BOUNCE_SIZE;
        }
    }
#else
    std::string path_copy{path};
    m_fd = m_file_replacement.open(ptr, path_copy.c_str());
    if (m_fd < 0) {
        return false;
    }
#endif
    m_file_size = m_file_replacement.size(ptr);
    // avio_buffer is deallocated in `avio_context_free`
    const auto avio_buffer = reinterpret_cast<u8*>(av_malloc(AVPLAYER_AVIO_BUFFER_SIZE));
    m_avio_context =
        avio_alloc_context(avio_buffer, AVPLAYER_AVIO_BUFFER_SIZE, 0, this,
                           &AvPlayerFileStreamer::ReadPacket, nullptr, &AvPlayerFileStreamer::Seek);
    return true;
}

void AvPlayerFileStreamer::Reset() {
    m_position = 0;
}

s32 AvPlayerFileStreamer::ReadPacket(void* opaque, u8* buffer, s32 size) {
    const auto self = reinterpret_cast<AvPlayerFileStreamer*>(opaque);
    if (self->m_position >= self->m_file_size) {
        return AVERROR_EOF;
    }
    if (self->m_position + size > self->m_file_size) {
        size = self->m_file_size - self->m_position;
    }
    const auto read_offset = self->m_file_replacement.read_offset;
    const auto ptr = self->m_file_replacement.object_ptr;
#ifdef BB_UWP
    if (size > 0 && reinterpret_cast<uintptr_t>(buffer) >= (uintptr_t{1} << 40) &&
        self->m_low_bounce && self->m_low_bounce_cap > 0) {
        s32 total_read = 0;
        while (total_read < size) {
            const u32 chunk = std::min<u32>(static_cast<u32>(size - total_read),
                                            self->m_low_bounce_cap);
            const s32 got = read_offset(ptr, self->m_low_bounce, self->m_position, chunk);
            if (got <= 0) {
                break;
            }
            std::memcpy(buffer + total_read, self->m_low_bounce, static_cast<size_t>(got));
            self->m_position += static_cast<u64>(got);
            total_read += got;
            if (static_cast<u32>(got) < chunk) {
                break;
            }
        }
        return (total_read == 0 && size != 0) ? AVERROR_EOF : total_read;
    }
#endif
    const auto bytes_read = read_offset(ptr, buffer, self->m_position, size);
    if (bytes_read == 0 && size != 0) {
        return AVERROR_EOF;
    }
    self->m_position += bytes_read;
    return bytes_read;
}

s64 AvPlayerFileStreamer::Seek(void* opaque, s64 offset, int whence) {
    const auto self = reinterpret_cast<AvPlayerFileStreamer*>(opaque);
    if (whence & AVSEEK_SIZE) {
        return self->m_file_size;
    }

    if (whence == SEEK_CUR) {
        self->m_position =
            std::min(u64(std::max(s64(0), s64(self->m_position) + offset)), self->m_file_size);
        return self->m_position;
    } else if (whence == SEEK_SET) {
        self->m_position = std::min(u64(std::max(s64(0), offset)), self->m_file_size);
        return self->m_position;
    } else if (whence == SEEK_END) {
        self->m_position =
            std::min(u64(std::max(s64(0), s64(self->m_file_size) + offset)), self->m_file_size);
        return self->m_position;
    }

    return -1;
}

} // namespace Libraries::AvPlayer

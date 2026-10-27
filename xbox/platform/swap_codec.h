// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport: the page codec for the UWP swap file.
//
// Why this exists: the console accepted every page we wrote until `disk pages=1171 MiB` and then
// answered ERROR_DISK_FULL (.908, docs/hardware-test-23) - with 767 MiB of guest memory still
// pageable and the app limit 780 MiB away. The device is simply almost full, and 1.17 GiB of swap
// is all the room this port will ever get on it. Guest private pages compress well (a heap of
// pointers, float data, unused buffers and zeroed blocks), so storing each page compressed is the
// only remaining way to raise how much guest memory can live on that 1.17 GiB.
//
// Format: LZ4 block, one page at a time, no frame. The caller writes a 2-byte little-endian length
// in front of the payload and reads it back; a stored length equal to the page size means the page
// was written raw (the codec did not shrink it). Compressed payloads are always smaller than the
// page, so a corrupt length is detectable by the caller's bound check.
//
// Both functions are plain C with no Windows dependency: xbox/tests/swap_codec_test.cpp round-trips
// random, zero, text-like and page-like data through them in CI, and the runtime verifies every
// page with uwp_swap_hash on the way back in, so a codec bug shows up as a failed read rather than
// as silently corrupted guest memory.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UWP_LZ_HASH_BITS 14
#define UWP_LZ_MIN_MATCH 4
#define UWP_LZ_LAST_LITERALS 8
#define UWP_LZ_MAX_OFFSET 65535
#define UWP_LZ_MIN_LENGTH 13

static uint32_t uwp_lz_read32(const unsigned char *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint32_t uwp_lz_hash(uint32_t sequence) {
    return (sequence * UINT32_C(2654435761)) >> (32 - UWP_LZ_HASH_BITS);
}

// Returns the compressed size, or 0 when the page does not compress into `capacity` bytes (the
// caller then stores it raw). `capacity` must be at least the source size for the 0 case to be
// meaningful; the runtime passes the page size.
static size_t uwp_lz_compress(const unsigned char *source, size_t size, unsigned char *dest,
                              size_t capacity, void *scratch) {
    // `scratch` is UWP_LZ_HASH_BITS worth of int32 offsets; the caller owns it so this function is
    // reentrant and allocates nothing.
    int32_t *table = (int32_t *)scratch;
    memset(table, 0xFF, sizeof(int32_t) << UWP_LZ_HASH_BITS);
    size_t ip = 0;
    size_t anchor = 0;
    size_t op = 0;
    if (size < UWP_LZ_MIN_LENGTH) {
        return 0;
    }
    const size_t limit = size - UWP_LZ_LAST_LITERALS;
    while (ip < limit) {
        const uint32_t sequence = uwp_lz_read32(source + ip);
        const uint32_t hash = uwp_lz_hash(sequence);
        const int32_t reference = table[hash];
        table[hash] = (int32_t)ip;
        if (reference < 0 || (size_t)(ip - (size_t)reference) > UWP_LZ_MAX_OFFSET ||
            uwp_lz_read32(source + (size_t)reference) != sequence) {
            ++ip;
            continue;
        }
        const size_t start = (size_t)reference;
        size_t match = UWP_LZ_MIN_MATCH;
        // A match never reaches the end of the block: the last UWP_LZ_LAST_LITERALS bytes stay
        // literals, which is what makes the final sequence a literals-only one (and therefore what
        // lets the decoder finish exactly at the last byte of the page).
        const size_t match_limit = size - UWP_LZ_LAST_LITERALS;
        while (ip + match < match_limit && source[start + match] == source[ip + match]) {
            ++match;
        }
        const size_t literals = ip - anchor;
        const size_t match_code = match - UWP_LZ_MIN_MATCH;
        // token + literal extensions + literals + 2-byte offset + match extensions. The match
        // length itself is never stored as a byte, only its extension bytes past the nibble.
        size_t need = 1 + literals + 2;
        if (literals >= 15) {
            need += (literals - 15) / 255 + 1;
        }
        if (match_code >= 15) {
            need += (match_code - 15) / 255 + 1;
        }
        if (op + need > capacity) {
            return 0;
        }
        size_t token_at = op++;
        const size_t literal_header = literals >= 15 ? 15 : literals;
        unsigned char token = (unsigned char)(literal_header << 4);
        if (literals >= 15) {
            size_t rest = literals - 15;
            while (rest >= 255) {
                dest[op++] = 255;
                rest -= 255;
            }
            dest[op++] = (unsigned char)rest;
        }
        memcpy(dest + op, source + anchor, literals);
        op += literals;
        const size_t offset = ip - start;
        dest[op++] = (unsigned char)(offset & 0xFF);
        dest[op++] = (unsigned char)(offset >> 8);
        const size_t match_header = match_code >= 15 ? 15 : match_code;
        token |= (unsigned char)match_header;
        dest[token_at] = token;
        if (match_code >= 15) {
            size_t rest = match_code - 15;
            while (rest >= 255) {
                dest[op++] = 255;
                rest -= 255;
            }
            dest[op++] = (unsigned char)rest;
        }
        ip += match;
        anchor = ip;
        if (ip >= limit) {
            break;
        }
        // Index the position just behind the match, as LZ4 does: it is the one most likely to be
        // useful for the next sequence and costs nothing.
        if (ip >= 2) {
            table[uwp_lz_hash(uwp_lz_read32(source + ip - 2))] = (int32_t)(ip - 2);
        }
    }
    const size_t literals = size - anchor;
    size_t need = 1 + literals;
    if (literals >= 15) {
        need += (literals - 15) / 255 + 1;
    }
    if (op + need > capacity) {
        return 0;
    }
    const size_t token_at = op++;
    const size_t literal_header = literals >= 15 ? 15 : literals;
    dest[token_at] = (unsigned char)(literal_header << 4);
    if (literals >= 15) {
        size_t rest = literals - 15;
        while (rest >= 255) {
            dest[op++] = 255;
            rest -= 255;
        }
        dest[op++] = (unsigned char)rest;
    }
    memcpy(dest + op, source + anchor, literals);
    op += literals;
    return op;
}

// Returns the number of bytes written to `dest` (exactly `capacity` on success) or 0 on malformed
// input - a length overrun, a back-reference before the start of the block, or a short block.
static size_t uwp_lz_decompress(const unsigned char *source, size_t size, unsigned char *dest,
                                size_t capacity) {
    size_t ip = 0;
    size_t op = 0;
    while (ip < size) {
        const unsigned char token = source[ip++];
        size_t literals = token >> 4;
        if (literals == 15) {
            unsigned char extra;
            do {
                if (ip >= size) return 0;
                extra = source[ip++];
                literals += extra;
            } while (extra == 255);
        }
        if (literals > size - ip || literals > capacity - op) return 0;
        memcpy(dest + op, source + ip, literals);
        ip += literals;
        op += literals;
        if (ip == size) {
            break; // the last sequence carries literals only
        }
        if (size - ip < 2) return 0;
        const size_t offset = (size_t)source[ip] | ((size_t)source[ip + 1] << 8);
        ip += 2;
        if (offset == 0 || offset > op) return 0;
        size_t match = (size_t)(token & 0x0F);
        if (match == 15) {
            unsigned char extra;
            do {
                if (ip >= size) return 0;
                extra = source[ip++];
                match += extra;
            } while (extra == 255);
        }
        match += UWP_LZ_MIN_MATCH;
        if (match > capacity - op) return 0;
        const unsigned char *from = dest + op - offset;
        for (size_t i = 0; i < match; ++i) {
            dest[op + i] = from[i];
        }
        op += match;
    }
    return op == capacity ? op : 0;
}

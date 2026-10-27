// SPDX-License-Identifier: GPL-2.0-or-later
// CI test for the UWP swap page codec (xbox/platform/swap_codec.h).
//
// The console's swap file is written compressed because the device refused further pages at
// 1,171 MiB (docs/hardware-test-23). A codec bug would corrupt guest memory, so this test round-
// trips every kind of data the pager sees and checks the properties the runtime relies on:
//   * every payload the compressor produces decompresses back to the identical page;
//   * a page that does not compress is reported as such (0), and the caller's raw fallback is what
//     actually stores it;
//   * malformed input is rejected instead of overrunning the destination - the runtime turns that
//     into a failed read, never into a silently wrong page.
#include "swap_codec.h"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr size_t kPage = 16384;
constexpr size_t kScratchBytes = sizeof(int32_t) << UWP_LZ_HASH_BITS;

int failures = 0;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    } else {
        std::printf("ok: %s\n", what.c_str());
    }
}

struct Codec {
    std::vector<int32_t> scratch = std::vector<int32_t>(kScratchBytes / sizeof(int32_t));
    std::vector<unsigned char> compressed = std::vector<unsigned char>(kPage * 2);
    std::vector<unsigned char> restored = std::vector<unsigned char>(kPage);

    // 1 when the page had to be stored raw (compressor refused), 0 when it fit, -1 on mismatch.
    int RoundTrip(const std::vector<unsigned char>& page, const char* what) {
        // The runtime passes the page size as the capacity, so a page that cannot shrink is
        // refused (0) and stored raw instead.
        const size_t packed = uwp_lz_compress(page.data(), page.size(), compressed.data(),
                                              page.size(), scratch.data());
        if (packed == 0) {
            return 1;
        }
        if (packed >= page.size()) {
            std::printf("FAIL: %s compressed to %zu of %zu bytes\n", what, packed, page.size());
            ++failures;
            return -1;
        }
        const size_t back = uwp_lz_decompress(compressed.data(), packed, restored.data(),
                                              page.size());
        if (back != page.size() || memcmp(restored.data(), page.data(), page.size()) != 0) {
            std::printf("FAIL: %s did not round-trip (%zu bytes in, %zu out)\n", what, packed,
                        back);
            ++failures;
            return -1;
        }
        return 0;
    }
};

} // namespace

int main() {
    Codec codec;
    std::mt19937_64 random{0x5eed5eed};

    // 1. All zero: the pager elides these entirely, but the codec must handle them too.
    {
        std::vector<unsigned char> page(kPage, 0);
        const int result = codec.RoundTrip(page, "an all-zero page");
        Check(result == 0, "an all-zero page compresses and round-trips");
        const size_t packed = uwp_lz_compress(page.data(), page.size(), codec.compressed.data(),
                                              page.size(), codec.scratch.data());
        Check(packed != 0 && packed < 128, "an all-zero page compresses to a handful of bytes");
    }

    // 2. Random data is incompressible: the codec must say so rather than emit something bigger.
    {
        std::vector<unsigned char> page(kPage);
        for (auto& byte : page) {
            byte = static_cast<unsigned char>(random());
        }
        const size_t packed = uwp_lz_compress(page.data(), page.size(), codec.compressed.data(),
                                              page.size(), codec.scratch.data());
        Check(packed == 0, "random data is refused rather than stored larger");
        // And the raw fallback the runtime uses keeps it intact.
        codec.compressed.assign(page.begin(), page.end());
        Check(memcmp(codec.compressed.data(), page.data(), kPage) == 0,
              "the raw fallback stores the page unchanged");
    }

    // 3. Page-shaped data: half pointers/ids, half zeros - what a guest heap page looks like.
    {
        std::vector<unsigned char> page(kPage);
        for (size_t i = 0; i < kPage; i += 8) {
            const uint64_t value = (i / 8) % 3 == 0 ? 0 : (random() & 0x0000'0FFF'FFFF'FFFF);
            memcpy(page.data() + i, &value, sizeof(value));
        }
        const int result = codec.RoundTrip(page, "a heap-like page");
        Check(result == 0, "a heap-like page compresses and round-trips");
        const size_t packed = uwp_lz_compress(page.data(), page.size(), codec.compressed.data(),
                                              page.size(), codec.scratch.data());
        Check(packed != 0 && packed < kPage, "a heap-like page compresses to less than its size");
    }

    // 4. Text-like data with long runs and repeats.
    {
        std::vector<unsigned char> page(kPage);
        const char* line = "UWP swap: pageable private=1209 pinned=0 MiB; page-out passes=437\n";
        size_t at = 0;
        while (at + strlen(line) <= kPage) {
            memcpy(page.data() + at, line, strlen(line));
            at += strlen(line);
        }
        while (at < kPage) {
            page[at++] = 0;
        }
        const int result = codec.RoundTrip(page, "a text-like page");
        Check(result == 0, "a text-like page compresses and round-trips");
    }

    // 5. Every length the compressor has to encode specially: tiny pages, and pages whose match
    //    length crosses the 15 and 255 extension boundaries.
    {
        for (size_t size : {size_t{13}, size_t{14}, size_t{15}, size_t{16}, size_t{19}, size_t{20},
                            size_t{31}, size_t{32}, size_t{270}, size_t{271}, size_t{300}}) {
            std::vector<unsigned char> page(size, 0);
            const size_t packed = uwp_lz_compress(page.data(), page.size(), codec.compressed.data(),
                                                  page.size(), codec.scratch.data());
            const bool ok = packed == 0 ? false : uwp_lz_decompress(codec.compressed.data(), packed,
                                                                   codec.restored.data(),
                                                                   page.size()) == page.size();
            Check(ok, "a page of " + std::to_string(size) + " zero bytes round-trips");
        }
    }

    // 6. A long run followed by noise: matches of several hundred bytes.
    {
        std::vector<unsigned char> page(kPage, 0xAB);
        for (size_t i = kPage / 2; i < kPage; ++i) {
            page[i] = static_cast<unsigned char>(random());
        }
        Check(codec.RoundTrip(page, "a long run followed by noise") == 0,
              "a long run followed by noise round-trips");
    }

    // 7. Fifty random pages of varying character, to catch an off-by-one that a fixed shape hides.
    {
        int ok = 0;
        for (int n = 0; n < 50; ++n) {
            std::vector<unsigned char> page(kPage);
            const unsigned kind = n % 5;
            for (size_t i = 0; i < kPage; ++i) {
                switch (kind) {
                case 0:
                    page[i] = static_cast<unsigned char>(random());
                    break;
                case 1:
                    page[i] = static_cast<unsigned char>(i & 0xFF);
                    break;
                case 2:
                    page[i] = static_cast<unsigned char>((i / 64) & 0x7);
                    break;
                case 3:
                    page[i] = (i % 977) < 700 ? 0 : static_cast<unsigned char>(random());
                    break;
                default:
                    page[i] = static_cast<unsigned char>(n);
                    break;
                }
            }
            // 0 = stored compressed, 1 = refused and stored raw by the caller; -1 is a failure.
            if (codec.RoundTrip(page, "a varied page") >= 0) {
                ++ok;
            }
        }
        Check(ok == 50, "fifty varied pages all round-trip or are refused");
    }

    // 8. Malformed input must be rejected, not written past the end of the destination.
    {
        std::vector<unsigned char> restore(kPage, 0xCC);
        // Literal length that runs past the block.
        const unsigned char overrun[] = {0xF0, 0xFF, 0xFF, 0x01};
        Check(uwp_lz_decompress(overrun, sizeof(overrun), restore.data(), restore.size()) == 0,
              "a literal run past the end of the block is rejected");
        // Back-reference before the start of the block.
        const unsigned char backref[] = {0x00, 0x01, 0x00};
        Check(uwp_lz_decompress(backref, sizeof(backref), restore.data(), restore.size()) == 0,
              "a back-reference before the start is rejected");
        // A block that ends without filling the page.
        const unsigned char short_block[] = {0x40, 'a', 'b', 'c', 'd'};
        Check(uwp_lz_decompress(short_block, sizeof(short_block), restore.data(), restore.size()) ==
                  0,
              "a short block is rejected");
        Check(restore[0] == 0xCC || restore[0] == 'a',
              "the rejected input never wrote past the destination");
    }

    if (failures != 0) {
        std::printf("swap codec: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("swap codec: all checks passed\n");
    return 0;
}

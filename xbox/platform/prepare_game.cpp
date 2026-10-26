// SPDX-License-Identifier: GPL-2.0-or-later
#include "prepare_game.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>

namespace BbPrepare {
namespace {

using View = std::span<const uint8_t>;
using Wide = __int128;

constexpr uint64_t kImageLimit = 512ull << 20;
constexpr uint64_t kEbootBase = 0x400000;
constexpr const char* kKnownTitles[] = {
    "CUSA00900", "CUSA00207", "CUSA00208", "CUSA00299", "CUSA01363",
    "CUSA03010", "CUSA03014", "CUSA03023", "CUSA03173", "CUSA03179",
};
constexpr std::pair<uint64_t, uint64_t> kRelocationTables[] = {
    {0x61000029, 0x6100002d},
    {0x6100002f, 0x61000031},
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::string hex(uint64_t value) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%#llx", static_cast<unsigned long long>(value));
    return buf;
}

uint64_t add_u64(uint64_t a, uint64_t b) {
    if (b > UINT64_MAX - a) fail("out of bounds: " + hex(a) + "+" + hex(b));
    return a + b;
}

uint64_t as_u64(Wide v) {
    if (v < 0 || v > Wide(UINT64_MAX)) fail("value out of range");
    return static_cast<uint64_t>(v);
}

int64_t as_i64(Wide v) {
    if (v < Wide(INT64_MIN) || v > Wide(INT64_MAX)) fail("value out of range");
    return static_cast<int64_t>(v);
}

View span(View data, uint64_t offset, uint64_t size) {
    if (offset > data.size() || size > data.size() - offset) {
        fail("out of bounds: " + hex(offset) + "+" + hex(size) + " / " + hex(data.size()));
    }
    return data.subspan(static_cast<size_t>(offset), static_cast<size_t>(size));
}

template <class T>
T read_le(View data, uint64_t offset) {
    T value;
    std::memcpy(&value, span(data, offset, sizeof(value)).data(), sizeof(value));
    return value;
}

template <class T>
void put_le(Bytes& out, T value) {
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), p, p + sizeof(value));
}

void put_text(Bytes& out, const char* text, size_t size) {
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(text),
               reinterpret_cast<const uint8_t*>(text) + size);
}

void store_u64(Bytes& image, uint64_t offset, uint64_t value) {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

// ---- Pure C++ SHA-256 (for exact Bloodborne 01.09 PATCH_FINGERPRINTS) ----
struct Sha256 {
    uint32_t state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    uint8_t block[64]{};
    uint64_t total_bytes = 0;
    size_t buffered = 0;

    static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

    void compress(const uint8_t b[64]) {
        static constexpr uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
        };
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(b[i * 4]) << 24) | (uint32_t(b[i * 4 + 1]) << 16) |
                   (uint32_t(b[i * 4 + 2]) << 8) | uint32_t(b[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = state[0], bb = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = h + s1 + ch + k[i] + w[i];
            uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & bb) ^ (a & c) ^ (bb & c);
            uint32_t t2 = s0 + maj;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = bb; bb = a; a = t1 + t2;
        }
        state[0] += a; state[1] += bb; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f;  state[6] += g; state[7] += h;
    }

    void update(const void* ptr, size_t len) {
        const auto* p = static_cast<const uint8_t*>(ptr);
        total_bytes += len;
        while (len > 0) {
            const size_t take = std::min(len, 64 - buffered);
            std::memcpy(block + buffered, p, take);
            buffered += take;
            p += take;
            len -= take;
            if (buffered == 64) {
                compress(block);
                buffered = 0;
            }
        }
    }

    std::string finish_hex() {
        const uint64_t bits = total_bytes * 8;
        const uint8_t one = 0x80;
        update(&one, 1);
        const uint8_t zero = 0;
        while (buffered != 56) update(&zero, 1);
        uint8_t len_be[8];
        for (int i = 0; i < 8; ++i) len_be[i] = static_cast<uint8_t>(bits >> ((7 - i) * 8));
        update(len_be, 8);
        char out[65];
        for (int i = 0; i < 8; ++i) std::snprintf(out + i * 8, 9, "%08x", state[i]);
        return std::string(out, 64);
    }
};

Bytes parse_hex_bytes(std::string_view hex_str) {
    Bytes out;
    int hi = -1;
    for (char ch : hex_str) {
        if (std::isspace(static_cast<unsigned char>(ch))) continue;
        int v = (ch >= '0' && ch <= '9') ? (ch - '0')
              : (ch >= 'a' && ch <= 'f') ? (ch - 'a' + 10)
              : (ch >= 'A' && ch <= 'F') ? (ch - 'A' + 10)
              : -1;
        if (v < 0) fail("invalid hex byte string");
        if (hi < 0) hi = v;
        else {
            out.push_back(static_cast<uint8_t>((hi << 4) | v));
            hi = -1;
        }
    }
    if (hi >= 0) fail("odd-length hex byte string");
    return out;
}

Wide parse_int(std::string text, bool any_base) {
    auto is_sp = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!text.empty() && is_sp(text.back())) text.pop_back();
    size_t i = 0;
    while (i < text.size() && is_sp(text[i])) ++i;
    bool neg = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) neg = (text[i++] == '-');
    unsigned base = 10;
    if (any_base && i + 1 < text.size() && text[i] == '0' && std::strchr("xXoObB", text[i + 1])) {
        base = std::strchr("xX", text[i + 1]) ? 16 : (std::strchr("oO", text[i + 1]) ? 8 : 2);
        i += 2;
    }
    uint64_t val = 0;
    bool digits = false;
    for (; i < text.size(); ++i) {
        char c = text[i];
        if (c == '_') continue;
        unsigned d = (c >= '0' && c <= '9') ? unsigned(c - '0')
                   : (c >= 'a' && c <= 'z') ? unsigned(c - 'a' + 10)
                   : (c >= 'A' && c <= 'Z') ? unsigned(c - 'A' + 10)
                   : 99u;
        if (d >= base) fail("invalid integer literal: '" + text + "'");
        if (val > (UINT64_MAX - d) / base) fail("integer overflow: '" + text + "'");
        val = val * base + d;
        digits = true;
    }
    if (!digits) fail("empty integer literal");
    return neg ? -Wide(val) : Wide(val);
}

// ---- SELF / ELF unpacking (scripts/prepare.py parse_self) ----
struct Segment {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

struct Self {
    Bytes elf;
    uint64_t entry = 0;
    std::vector<Segment> ph;
};

Self parse_self(View data) {
    if (data.empty()) fail("empty SELF/ELF (0 bytes)");
    View magic = span(data, 0, 4);
    const bool plain_elf = (std::memcmp(magic.data(), "\x7f" "ELF", 4) == 0);
    uint64_t count = 0, base = 0;
    if (!plain_elf) {
        if (std::memcmp(magic.data(), "O\x15=\x1d", 4) != 0) {
            fail("expected decrypted PS4 SELF or ELF (is the dump still encrypted?)");
        }
        count = read_le<uint16_t>(data, 24);
        base = 32 + count * 32;
    }
    View header = span(data, base, 64);
    if (std::memcmp(header.data(), "\x7f" "ELF\x02\x01\x01", 7) != 0 ||
        read_le<uint16_t>(header, 18) != 62) {
        fail("expected little-endian x86-64 ELF");
    }
    const uint16_t phentsize = read_le<uint16_t>(header, 54);
    const uint16_t phnum = read_le<uint16_t>(header, 56);
    if (phentsize != 56 || phnum == 0 || phnum >= 256) fail("unsupported program headers");

    Self self;
    self.entry = read_le<uint64_t>(header, 24);
    const uint64_t phoff = read_le<uint64_t>(header, 32);
    for (uint64_t i = 0; i < phnum; ++i) {
        View entry = span(data, add_u64(add_u64(base, phoff), i * 56), 56);
        self.ph.push_back({
            read_le<uint32_t>(entry, 0),  read_le<uint32_t>(entry, 4),
            read_le<uint64_t>(entry, 8),  read_le<uint64_t>(entry, 16),
            read_le<uint64_t>(entry, 24), read_le<uint64_t>(entry, 32),
            read_le<uint64_t>(entry, 40), read_le<uint64_t>(entry, 48),
        });
    }
    const uint64_t header_end = add_u64(phoff, uint64_t(phnum) * 56);
    uint64_t end = header_end;
    for (const auto& p : self.ph) end = std::max(end, add_u64(p.offset, p.filesz));
    if (end > kImageLimit) fail("probe image exceeds 512 MiB limit");

    if (plain_elf) {
        View whole = span(data, 0, end);
        self.elf.assign(whole.begin(), whole.end());
        return self;
    }

    View headers = span(data, base, header_end);
    self.elf.assign(static_cast<size_t>(end), 0);
    std::copy(headers.begin(), headers.end(), self.elf.begin());
    std::vector<std::pair<uint64_t, uint64_t>> covered{{0, header_end}};

    for (uint64_t i = 0; i < count; ++i) {
        View entry = span(data, 32 + i * 32, 32);
        const uint64_t flags = read_le<uint64_t>(entry, 0);
        const uint64_t offset = read_le<uint64_t>(entry, 8);
        const uint64_t size = read_le<uint64_t>(entry, 16);
        const uint64_t memory = read_le<uint64_t>(entry, 24);
        if (!(flags & 0x800)) continue;
        if (flags & 10) fail("encrypted/compressed SELF segment is unsupported");
        const uint64_t index = (flags >> 20) & 4095;
        if (index >= self.ph.size()) fail("invalid segment index");
        const auto& p = self.ph[static_cast<size_t>(index)];
        if (size != p.filesz || size != memory) fail("unsupported blocked segment layout");
        View bytes = span(data, offset, size);
        std::copy(bytes.begin(), bytes.end(), self.elf.begin() + p.offset);
        covered.emplace_back(p.offset, p.offset + size);
    }

    for (size_t i = 0; i < self.ph.size(); ++i) {
        const auto& p = self.ph[i];
        if (!p.filesz || std::any_of(covered.begin(), covered.end(), [&](const auto& c) {
                return c.first <= p.offset && p.offset + p.filesz <= c.second;
            })) {
            continue;
        }
        if (p.type == 1 || p.type == 2 || p.type == 0x61000000 || p.type == 0x61000010) {
            fail("required segment " + std::to_string(i) + " is unavailable");
        }
    }
    return self;
}

const Segment* find_segment(const std::vector<Segment>& ph, uint32_t type) {
    for (const auto& p : ph) {
        if (p.type == type) return &p;
    }
    return nullptr;
}

const Segment& require_segment(const std::vector<Segment>& ph, uint32_t type) {
    if (const auto* p = find_segment(ph, type)) return *p;
    fail("program header " + hex(type) + " is missing");
}

struct Dynamic {
    std::vector<std::pair<uint64_t, uint64_t>> entries;
    std::map<uint64_t, uint64_t> tags;
    View blob, strings;
};

uint64_t require_tag(const std::map<uint64_t, uint64_t>& tags, uint64_t key) {
    auto it = tags.find(key);
    if (it == tags.end()) fail("dynamic tag " + hex(key) + " is missing");
    return it->second;
}

Dynamic parse_dynamic(const Self& self, bool stop_at_null) {
    Dynamic d;
    const auto& dp = require_segment(self.ph, 2);
    for (uint64_t pos = dp.offset; pos < dp.offset + dp.filesz; pos += 16) {
        View entry = span(self.elf, pos, 16);
        const uint64_t key = read_le<uint64_t>(entry, 0);
        const uint64_t value = read_le<uint64_t>(entry, 8);
        if (stop_at_null && !key) break;
        d.entries.emplace_back(key, value);
        d.tags[key] = value;
    }
    const auto& lib = require_segment(self.ph, 0x61000000);
    d.blob = span(self.elf, lib.offset, lib.filesz);
    d.strings = span(d.blob, require_tag(d.tags, 0x61000035), require_tag(d.tags, 0x61000037));
    return d;
}

std::string read_ascii(View strings, uint64_t offset) {
    span(strings, offset, 1);
    auto begin = strings.begin() + static_cast<ptrdiff_t>(offset);
    auto end = std::find(begin, strings.end(), uint8_t(0));
    if (end == strings.end()) fail("unterminated symbol string");
    if (std::any_of(begin, end, [](uint8_t c) { return c >= 0x80; })) fail("non-ASCII symbol name");
    return std::string(begin, end);
}

View symbol_table(const Dynamic& d) {
    return span(d.blob, require_tag(d.tags, 0x61000039), require_tag(d.tags, 0x6100003f));
}

bool init_env_is_ret(View source) {
    const Self self = parse_self(source);
    const Dynamic d = parse_dynamic(self, false);
    View syms = symbol_table(d);
    bool result = false;
    for (uint64_t pos = 0; pos < syms.size(); pos += 24) {
        View entry = span(syms, pos, 24);
        const uint8_t info = entry[4];
        const uint16_t section = read_le<uint16_t>(entry, 6);
        const uint64_t value = read_le<uint64_t>(entry, 8);
        const uint64_t size = read_le<uint64_t>(entry, 16);
        const std::string symbol = read_ascii(d.strings, read_le<uint32_t>(entry, 0));
        const std::string key = symbol.substr(0, symbol.find('#'));
        const bool is_init_env = (key == "bzQExy189ZI"); // nid("_init_env")
        if ((!is_init_env && key != "sqWytnhYdEg") || !section || (info & 15) != 2) continue;
        const Segment* code_seg = nullptr;
        for (const auto& p : self.ph) {
            if (p.type == 1 && (p.flags & 1) && p.vaddr <= value &&
                Wide(value) < Wide(p.vaddr) + p.filesz) {
                code_seg = &p;
                break;
            }
        }
        if (!code_seg) fail("libc.prx: " + symbol + " lies outside executable code");
        View code = span(self.elf, code_seg->offset + value - code_seg->vaddr, size);
        if (is_init_env) result = (code.size() == 1 && code[0] == 0xc3);
    }
    return result;
}

// ---- Eboot preparation & module linking (scripts/prepare.py + scripts/link_modules.py) ----
struct Relocation {
    uint64_t target, kind;
    int64_t value, addend;
};

struct Probe {
    uint64_t size = 0, entry = 0, flags = 0;
    std::vector<std::array<uint64_t, 3>> segments;
    std::vector<std::string> names;
    std::vector<Relocation> relocs;
    Bytes image;
};

bool is_mapped(const std::vector<const Segment*>& loads, uint64_t address, uint64_t size) {
    return std::any_of(loads.begin(), loads.end(), [&](const Segment* p) {
        return p->vaddr <= address && Wide(address) + size <= Wide(p->vaddr) + p->memsz;
    });
}

std::vector<const Segment*> load_segments(const Self& self) {
    std::vector<const Segment*> loads;
    for (const auto& p : self.ph) {
        if (p.type == 1 || p.type == 0x61000010) loads.push_back(&p);
    }
    if (loads.empty()) fail("no loadable segments");
    return loads;
}

uint64_t memory_size(const std::vector<const Segment*>& loads) {
    Wide size = 0;
    for (const auto* p : loads) size = std::max(size, Wide(p->vaddr) + p->memsz);
    return size > Wide(kImageLimit) ? (kImageLimit + 1) : static_cast<uint64_t>(size);
}

Probe prepare_eboot(const Self& self, bool init_env) {
    const auto loads = load_segments(self);
    Probe probe;
    probe.size = memory_size(loads);
    if (probe.size > kImageLimit || !is_mapped(loads, self.entry, 1)) fail("invalid memory image");
    probe.entry = self.entry;
    probe.image.assign(static_cast<size_t>(probe.size), 0);
    for (const auto* p : loads) {
        if (p->filesz > p->memsz) fail("segment file size exceeds memory size");
        View bytes = span(self.elf, p->offset, p->filesz);
        std::copy(bytes.begin(), bytes.end(), probe.image.begin() + static_cast<ptrdiff_t>(p->vaddr));
    }
    const Dynamic d = parse_dynamic(self, true);
    View syms = symbol_table(d);
    if (syms.size() % 24) fail("bad symbol table size");

    struct Sym {
        std::string name;
        unsigned type;
        uint16_t section;
        uint64_t value;
    };
    std::vector<Sym> symbols;
    symbols.reserve(syms.size() / 24);
    for (uint64_t pos = 0; pos < syms.size(); pos += 24) {
        View entry = span(syms, pos, 24);
        symbols.push_back({read_ascii(d.strings, read_le<uint32_t>(entry, 0)),
                           entry[4] & 15u, read_le<uint16_t>(entry, 6), read_le<uint64_t>(entry, 8)});
    }

    std::map<uint64_t, uint64_t> imports;
    std::vector<uint64_t> import_order;
    for (auto [offset_tag, size_tag] : kRelocationTables) {
        View table = span(d.blob, require_tag(d.tags, offset_tag), require_tag(d.tags, size_tag));
        if (table.size() % 24) fail("bad relocation table size");
        for (uint64_t pos = 0; pos < table.size(); pos += 24) {
            View entry = span(table, pos, 24);
            const uint64_t target = read_le<uint64_t>(entry, 0);
            const uint64_t info = read_le<uint64_t>(entry, 8);
            const int64_t addend = read_le<int64_t>(entry, 16);
            const uint64_t kind = info & 0xffffffffu;
            const uint64_t sym = info >> 32;
            if (!is_mapped(loads, target, 8)) fail("relocation outside mapped segments: " + hex(target));
            if (kind == 8) {
                probe.relocs.push_back({target, 0, addend, 0});
            } else if (kind == 1 || kind == 6 || kind == 7) {
                if (sym >= symbols.size()) fail("relocation symbol out of range");
                const auto& s = symbols[static_cast<size_t>(sym)];
                if (s.section) {
                    if (s.section == 0xfff1) store_u64(probe.image, target, as_u64(Wide(s.value) + addend));
                    else probe.relocs.push_back({target, 0, as_i64(Wide(s.value) + addend), 0});
                } else {
                    auto [imp, inserted] = imports.try_emplace(sym, imports.size());
                    if (inserted) import_order.push_back(sym);
                    const uint64_t imp_kind = (s.type == 1) ? 2 : 1;
                    if ((imp_kind == 1 && addend) || addend < 0 || addend >= 4096) {
                        fail("unsupported import addend");
                    }
                    probe.relocs.push_back({target, imp_kind, static_cast<int64_t>(imp->second), addend});
                }
            } else {
                fail("unsupported relocation " + std::to_string(kind));
            }
        }
    }
    for (uint64_t sym : import_order) probe.names.push_back(symbols[static_cast<size_t>(sym)].name);
    for (const auto& name : probe.names) {
        if (name.size() >= 128) fail("import name too long");
    }
    for (const auto* p : loads) probe.segments.push_back({p->vaddr, p->memsz, p->flags});
    probe.flags = init_env ? 1 : 0;
    return probe;
}

using LibIdentity = std::pair<std::string, uint64_t>;

struct Identity {
    std::string nid;
    LibIdentity lib;
    LibIdentity mod;
    auto operator<=>(const Identity&) const = default;
};

using Key = std::optional<Identity>;

struct Symbol {
    std::string name;
    unsigned type, binding;
    uint16_t section;
    uint64_t value, size;
    Key identity;
};

struct ModuleRelocation {
    uint64_t target, kind, symbol;
    int64_t addend;
};

std::string encode_id(uint64_t value) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string result(1, kAlphabet[value & 63]);
    for (value >>= 6; value; value >>= 6) {
        result.insert(result.begin(), kAlphabet[value & 63]);
    }
    return result;
}

struct Module {
    Self self;
    Dynamic d;
    std::map<std::string, LibIdentity> libraries, modules;
    std::vector<Symbol> symbols;
    std::vector<ModuleRelocation> relocs;

    Key identity(const std::string& name) const {
        const auto first = name.find('#');
        const auto second = (first == std::string::npos) ? first : name.find('#', first + 1);
        if (second == std::string::npos || name.find('#', second + 1) != std::string::npos) {
            return std::nullopt;
        }
        auto lib = libraries.find(name.substr(first + 1, second - first - 1));
        if (lib == libraries.end()) fail("unknown library in " + name);
        auto mod = modules.find(name.substr(second + 1));
        if (mod == modules.end()) fail("unknown module in " + name);
        return Identity{name.substr(0, first), lib->second, mod->second};
    }
};

Module load_module(View source) {
    Module m;
    m.self = parse_self(source);
    m.d = parse_dynamic(m.self, true);
    for (auto [key, value] : m.d.entries) {
        if (key != 0x61000013 && key != 0x61000015 && key != 0x6100000d && key != 0x6100000f) {
            continue;
        }
        auto& table = (key == 0x61000013 || key == 0x61000015) ? m.libraries : m.modules;
        const std::string id = encode_id(value >> 48);
        LibIdentity ident{read_ascii(m.d.strings, value & 0xffffffffu), (value >> 32) & 65535u};
        auto found = table.find(id);
        if (found != table.end() && found->second != ident) fail("conflicting module/library IDs");
        table[id] = ident;
    }
    View syms = symbol_table(m.d);
    for (uint64_t pos = 0; pos < syms.size(); pos += 24) {
        View entry = span(syms, pos, 24);
        Symbol s;
        s.name = read_ascii(m.d.strings, read_le<uint32_t>(entry, 0));
        s.type = entry[4] & 15u;
        s.binding = entry[4] >> 4;
        s.section = read_le<uint16_t>(entry, 6);
        s.value = read_le<uint64_t>(entry, 8);
        s.size = read_le<uint64_t>(entry, 16);
        if (!s.name.empty()) s.identity = m.identity(s.name);
        m.symbols.push_back(std::move(s));
    }
    for (auto [offset_tag, size_tag] : kRelocationTables) {
        View table = span(m.d.blob, require_tag(m.d.tags, offset_tag), require_tag(m.d.tags, size_tag));
        for (uint64_t pos = 0; pos < table.size(); pos += 24) {
            View entry = span(table, pos, 24);
            const uint64_t info = read_le<uint64_t>(entry, 8);
            m.relocs.push_back({read_le<uint64_t>(entry, 0), info & 0xffffffffu,
                                info >> 32, read_le<int64_t>(entry, 16)});
        }
    }
    return m;
}

void patch_fs_loads(Bytes& image, const std::vector<Segment>& ph, uint64_t base) {
    static constexpr uint8_t kFsLoad[] = {0x64, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0};
    for (const auto& p : ph) {
        if (p.type != 1 || !(p.flags & 1)) continue;
        const uint64_t end = std::min<uint64_t>(base + p.vaddr + p.filesz, image.size());
        for (uint64_t at = base + p.vaddr; at + sizeof(kFsLoad) <= end;) {
            auto found = std::search(image.begin() + static_cast<ptrdiff_t>(at),
                                     image.begin() + static_cast<ptrdiff_t>(end),
                                     std::begin(kFsLoad), std::end(kFsLoad));
            if (found == image.begin() + static_cast<ptrdiff_t>(end)) break;
            *found = 0x65;
            at = static_cast<uint64_t>(found - image.begin()) + sizeof(kFsLoad);
        }
    }
}

// ---- Red-zone leaf store protection (scripts/link_modules.py protect_red_zones) ----
struct RedZoneSite {
    uint64_t offset;
    Bytes original;
    std::vector<int> lengths;
};

std::vector<RedZoneSite> parse_redzone_json(const std::string& json) {
    std::vector<RedZoneSite> sites;
    if (json.empty()) return sites;
    const auto key_pos = json.find("\"sites\"");
    if (key_pos == std::string::npos) return sites;
    size_t pos = json.find('[', key_pos);
    if (pos == std::string::npos) return sites;
    ++pos; // inside outer array

    auto skip_ws = [&]() {
        while (pos < json.size() &&
               (std::isspace(static_cast<unsigned char>(json[pos])) || json[pos] == ',')) {
            ++pos;
        }
    };

    for (;;) {
        skip_ws();
        if (pos >= json.size() || json[pos] == ']') break;
        if (json[pos] != '[') fail("malformed redzone.json site entry");
        ++pos;
        skip_ws();
        // 1. offset (uint64)
        size_t num_end = pos;
        while (num_end < json.size() && std::isdigit(static_cast<unsigned char>(json[num_end]))) ++num_end;
        if (num_end == pos) fail("malformed redzone.json offset");
        RedZoneSite site{};
        site.offset = static_cast<uint64_t>(std::strtoull(json.substr(pos, num_end - pos).c_str(), nullptr, 10));
        pos = num_end;
        skip_ws();
        // 2. original hex string
        if (pos >= json.size() || json[pos] != '"') fail("malformed redzone.json hex string");
        ++pos;
        const size_t str_end = json.find('"', pos);
        if (str_end == std::string::npos) fail("unterminated redzone.json hex string");
        site.original = parse_hex_bytes(std::string_view(json).substr(pos, str_end - pos));
        pos = str_end + 1;
        skip_ws();
        // 3. lengths array
        if (pos >= json.size() || json[pos] != '[') fail("malformed redzone.json lengths");
        ++pos;
        for (;;) {
            skip_ws();
            if (pos >= json.size() || json[pos] == ']') break;
            size_t len_end = pos;
            if (json[len_end] == '-') ++len_end;
            while (len_end < json.size() && std::isdigit(static_cast<unsigned char>(json[len_end]))) ++len_end;
            if (len_end == pos || (len_end == pos + 1 && json[pos] == '-')) {
                fail("malformed redzone.json instruction length");
            }
            site.lengths.push_back(std::atoi(json.substr(pos, len_end - pos).c_str()));
            pos = len_end;
        }
        if (pos >= json.size() || json[pos] != ']') fail("unclosed redzone.json lengths");
        ++pos;
        skip_ws();
        if (pos >= json.size() || json[pos] != ']') fail("unclosed redzone.json site");
        ++pos;
        sites.push_back(std::move(site));
    }
    return sites;
}

std::string parse_redzone_text_sha256(const std::string& json) {
    const auto key = json.find("\"text_sha256\"");
    if (key == std::string::npos) return {};
    const auto colon = json.find(':', key);
    if (colon == std::string::npos) return {};
    const auto q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return {};
    const auto q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return json.substr(q1 + 1, q2 - q1 - 1);
}

std::string compute_eboot_text_sha256(const Self& self, const Bytes& raw_image) {
    const Segment* text = nullptr;
    for (const auto& p : self.ph) {
        if (p.type == 1 && (p.flags & 1)) {
            text = &p;
            break;
        }
    }
    const Segment* hdr_seg = find_segment(self.ph, 0x6474e550u);
    if (!text || !hdr_seg) return {};
    const uint64_t hdr = hdr_seg->vaddr;
    if (hdr + 12 > raw_image.size() || raw_image[static_cast<size_t>(hdr + 2)] != 0x03 ||
        raw_image[static_cast<size_t>(hdr + 3)] != 0x3b) {
        return {};
    }
    const uint32_t count = read_le<uint32_t>(View(raw_image.data(), raw_image.size()), hdr + 8);
    if (hdr + 12 + uint64_t(count) * 8 > raw_image.size()) return {};
    const uint64_t text_end = std::min<uint64_t>(text->vaddr + text->filesz, raw_image.size());
    uint64_t first_start = text_end;
    View img_view(raw_image.data(), raw_image.size());
    for (uint32_t i = 0; i < count; ++i) {
        const int64_t s = static_cast<int64_t>(hdr) + read_le<int32_t>(img_view, hdr + 12 + uint64_t(i) * 8);
        if (s >= static_cast<int64_t>(text->vaddr) && static_cast<uint64_t>(s) < text_end) {
            first_start = std::min<uint64_t>(first_start, static_cast<uint64_t>(s));
        }
    }
    if (first_start >= text_end) return {};
    Sha256 sha;
    sha.update(raw_image.data() + first_start, static_cast<size_t>(text_end - first_start));
    return sha.finish_hex();
}

void apply_red_zones(Bytes& image, std::vector<std::array<uint64_t, 3>>& segments,
                     const std::vector<Relocation>& relocs, const std::string& redzone_json,
                     const std::string& actual_text_sha256) {
    const std::string expected_sha = parse_redzone_text_sha256(redzone_json);
    if (!expected_sha.empty() && actual_text_sha256 != expected_sha) {
        std::printf("Red-zone trampolines: skipped (eboot text_sha256 %.12s != 01.09 %.12s)\n",
                    actual_text_sha256.empty() ? "unknown" : actual_text_sha256.c_str(),
                    expected_sha.c_str());
        return;
    }
    const auto sites = parse_redzone_json(redzone_json);
    if (sites.empty()) return;

    std::vector<int64_t> skip;
    skip.reserve(relocs.size());
    for (const auto& r : relocs) skip.push_back(static_cast<int64_t>(r.target));
    std::sort(skip.begin(), skip.end());

    static constexpr uint8_t kLeaDown[] = {0x48, 0x8d, 0x64, 0x24, 0x80};             // lea rsp, [rsp-0x80]
    static constexpr uint8_t kLeaUp[] = {0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00}; // lea rsp, [rsp+0x80]

    const uint64_t base = (image.size() + 65535u) & ~uint64_t(65535u);
    Bytes code;
    size_t applied = 0;

    for (const auto& site : sites) {
        const uint64_t offset = site.offset;
        const uint64_t end = offset + site.original.size();
        if (end > image.size() || site.original.size() < 5) continue;
        if (std::memcmp(image.data() + offset, site.original.data(), site.original.size()) != 0) {
            continue;
        }
        auto near = std::upper_bound(skip.begin(), skip.end(), static_cast<int64_t>(offset) - 8);
        if (near != skip.end() && *near < static_cast<int64_t>(end)) continue;

        const uint64_t here = base + code.size();
        uint64_t pos = offset;
        for (int len : site.lengths) {
            const size_t n = static_cast<size_t>(std::abs(len));
            if (len < 0) code.insert(code.end(), std::begin(kLeaDown), std::end(kLeaDown));
            code.insert(code.end(), image.begin() + static_cast<ptrdiff_t>(pos),
                        image.begin() + static_cast<ptrdiff_t>(pos + n));
            if (len < 0) code.insert(code.end(), std::begin(kLeaUp), std::end(kLeaUp));
            pos += n;
        }
        code.push_back(0xe9);
        const int32_t back_rel = static_cast<int32_t>(
            static_cast<int64_t>(end) - static_cast<int64_t>(base + code.size() + 4));
        put_le<int32_t>(code, back_rel);
        const size_t pad = (16 - (code.size() % 16)) % 16;
        code.insert(code.end(), pad, 0xcc);

        image[static_cast<size_t>(offset)] = 0xe9;
        const int32_t fwd_rel = static_cast<int32_t>(
            static_cast<int64_t>(here) - static_cast<int64_t>(offset + 5));
        std::memcpy(image.data() + offset + 1, &fwd_rel, sizeof(fwd_rel));
        if (site.original.size() > 5) {
            std::memset(image.data() + offset + 5, 0xcc, site.original.size() - 5);
        }
        ++applied;
    }

    if (!code.empty()) {
        const uint64_t seg_size = (code.size() + 4095u) & ~uint64_t(4095u);
        image.resize(static_cast<size_t>(base), 0);
        image.insert(image.end(), code.begin(), code.end());
        image.resize(static_cast<size_t>(base + seg_size), 0xcc);
        segments.push_back({base, seg_size, 5});
    }
    std::printf("Red-zone trampolines: applied %zu of %zu sites\n", applied, sites.size());
}

Bytes link_bbprobe5(const Module& main, Probe& probe, const std::vector<Module>& modules,
                    const std::string& redzone_json, bool enable_red_zones) {
    // Match scripts/link_modules.py exactly:
    // identities = {main['identity'](name): i for i, name in enumerate(names)}
    // Note: Python dict comprehension overwrites duplicate keys while keeping insertion order of first occurrence.
    std::vector<Key> keys;
    std::vector<uint64_t> indexes;
    std::map<Key, size_t> positions;
    auto set_identity = [&](const Key& key, uint64_t index) {
        auto [it, inserted] = positions.try_emplace(key, keys.size());
        if (inserted) {
            keys.push_back(key);
            indexes.push_back(index);
        } else {
            indexes[it->second] = index;
        }
    };

    std::vector<std::string> names = probe.names;
    for (size_t i = 0; i < names.size(); ++i) {
        set_identity(main.identity(names[i]), i);
    }

    std::map<LibIdentity, std::string> main_libraries, main_modules;
    for (const auto& [k, ident] : main.libraries) main_libraries[ident] = k;
    for (const auto& [k, ident] : main.modules) main_modules[ident] = k;

    auto imported = [&](const Symbol& symbol) -> uint64_t {
        if (!symbol.identity) fail("imported symbol lacks identity: " + symbol.name);
        auto pos = positions.find(symbol.identity);
        if (pos != positions.end()) return indexes[pos->second];
        const auto& ident = *symbol.identity;
        auto lib_it = main_libraries.find(ident.lib);
        auto mod_it = main_modules.find(ident.mod);
        std::string spelled = (lib_it != main_libraries.end() && mod_it != main_modules.end())
                            ? (ident.nid + "#" + lib_it->second + "#" + mod_it->second)
                            : (ident.nid + "#" + ident.lib.first);
        const uint64_t idx = names.size();
        set_identity(symbol.identity, idx);
        names.push_back(std::move(spelled));
        return idx;
    };

    struct Export {
        uint64_t address, kind;
    };
    std::map<Identity, Export> exports;
    std::map<std::pair<std::string, std::string>, std::vector<Export>> by_nid;
    std::vector<std::array<uint64_t, 7>> table;

    Bytes& image = probe.image;
    const std::string eboot_text_sha256 = compute_eboot_text_sha256(main.self, image);
    uint64_t base = (probe.size + 65535u) & ~uint64_t(65535u);
    patch_fs_loads(image, main.self.ph, 0);
    uint64_t tls_module = 2;

    for (const auto& m : modules) {
        const auto loads = load_segments(m.self);
        const uint64_t modsize = memory_size(loads);
        if (Wide(base) + modsize > Wide(kImageLimit)) fail("linked image exceeds probe limit");
        if (base + modsize < image.size()) fail("module overlaps image");
        image.resize(static_cast<size_t>(base + modsize), 0);

        for (const auto* p : loads) {
            if (p->filesz > p->memsz) fail("invalid module segment size");
            View bytes = span(m.self.elf, p->offset, p->filesz);
            std::copy(bytes.begin(), bytes.end(),
                      image.begin() + static_cast<ptrdiff_t>(base + p->vaddr));
            probe.segments.push_back({base + p->vaddr, p->memsz, p->flags});
        }

        const Segment* tls = find_segment(m.self.ph, 7);
        uint64_t module_id = 0;
        if (tls) {
            if (tls->filesz > tls->memsz || !is_mapped(loads, tls->vaddr, tls->memsz)) {
                fail("unsupported module TLS layout");
            }
            module_id = tls_module++;
        }

        for (const auto& r : m.relocs) {
            if (!is_mapped(loads, r.target, 8)) {
                fail("module relocation outside image " + hex(r.target));
            }
            const uint64_t target = r.target + base;
            if (r.kind == 8) {
                probe.relocs.push_back({target, 0, as_i64(Wide(base) + r.addend), 0});
            } else if (r.kind == 16 || r.kind == 1 || r.kind == 6 || r.kind == 7) {
                if (r.symbol >= m.symbols.size()) fail("module relocation symbol out of range");
                const auto& sym = m.symbols[static_cast<size_t>(r.symbol)];
                if (r.kind == 16) {
                    if (!sym.name.empty() || sym.type != 3 || r.addend || !module_id) {
                        fail("unsupported external TLS relocation");
                    }
                    store_u64(image, target, module_id);
                } else if (sym.section) {
                    if (sym.section == 0xfff1) {
                        store_u64(image, target, as_u64(Wide(sym.value) + r.addend));
                    } else {
                        probe.relocs.push_back({target, 0, as_i64(Wide(base) + sym.value + r.addend), 0});
                    }
                } else if (!sym.identity && sym.binding == 2) {
                    store_u64(image, target, 0);
                } else {
                    const uint64_t rk = (sym.type == 1) ? 2 : 1;
                    if ((rk == 1 && r.addend) || r.addend < 0 || r.addend >= 4096) {
                        fail("unsupported module import addend");
                    }
                    probe.relocs.push_back({target, rk, static_cast<int64_t>(imported(sym)), r.addend});
                }
            } else {
                fail("unsupported module relocation type " + std::to_string(r.kind));
            }
        }

        for (const auto& s : m.symbols) {
            if (!s.section || (s.binding != 1 && s.binding != 2) || !s.identity ||
                (s.type != 1 && s.type != 2)) {
                continue;
            }
            if (!is_mapped(loads, s.value, std::max<uint64_t>(1, s.size))) {
                fail("export outside load segments");
            }
            if (exports.count(*s.identity)) fail("ambiguous export");
            const Export ex{base + s.value, (s.type == 1) ? 2u : 1u};
            exports[*s.identity] = ex;
            by_nid[{s.identity->nid, s.identity->lib.first}].push_back(ex);
        }

        patch_fs_loads(image, m.self.ph, base);
        auto init_it = m.d.tags.find(12);
        table.push_back({base,
                         modsize,
                         as_u64(Wide(base) + (init_it == m.d.tags.end() ? 0 : init_it->second)),
                         tls ? (base + tls->vaddr) : 0,
                         tls ? tls->memsz : 0,
                         tls ? tls->filesz : 0,
                         module_id});
        base = (base + modsize + 65535u) & ~uint64_t(65535u);
    }

    std::vector<std::array<uint64_t, 3>> bindings;
    for (size_t i = 0; i < keys.size(); ++i) {
        const auto& key = keys[i];
        if (!key) continue;
        const Export* found = nullptr;
        if (auto it = exports.find(*key); it != exports.end()) found = &it->second;
        if (!found && key->lib.first == "libSceLibcInternal") {
            auto cand = by_nid.find({key->nid, "libc"});
            if (cand != by_nid.end() && cand->second.size() == 1) found = &cand->second[0];
        }
        if (found) bindings.push_back({indexes[i], found->address, found->kind});
    }

    std::array<uint64_t, 4> main_tls{};
    if (const Segment* tls = find_segment(main.self.ph, 7)) {
        main_tls = {tls->vaddr, tls->filesz, tls->memsz, tls->align};
        if (tls->filesz > tls->memsz || tls->memsz > 1024 * 1024 ||
            Wide(tls->vaddr) + tls->filesz > Wide(probe.size)) {
            fail("unsupported eboot TLS layout");
        }
    }
    const auto& procparam = require_segment(main.self.ph, 0x61000001);

    if (enable_red_zones && !redzone_json.empty()) {
        apply_red_zones(image, probe.segments, probe.relocs, redzone_json, eboot_text_sha256);
    }

    Bytes out;
    out.reserve(image.size() + probe.relocs.size() * 32 + names.size() * 128 + (1 << 16));
    put_text(out, "BBPROBE5", 8);
    for (uint64_t v : {uint64_t(image.size()), probe.entry, uint64_t(probe.segments.size()),
                       uint64_t(probe.relocs.size()), uint64_t(names.size()), probe.flags,
                       procparam.vaddr}) {
        put_le(out, v);
    }
    for (uint64_t v : main_tls) put_le(out, v);
    put_le<uint64_t>(out, table.size());
    for (const auto& entry : table) {
        for (uint64_t v : entry) put_le(out, v);
    }
    put_le<uint64_t>(out, bindings.size());
    for (const auto& b : bindings) {
        for (uint64_t v : b) put_le(out, v);
    }
    for (const auto& seg : probe.segments) {
        for (uint64_t v : seg) put_le(out, v);
    }
    for (const auto& name : names) {
        if (name.size() >= 128) fail("name too long");
        put_text(out, name.data(), name.size());
        out.resize(out.size() + 128 - name.size(), 0);
    }
    for (const auto& r : probe.relocs) {
        put_le(out, r.target);
        put_le(out, r.kind);
        put_le(out, r.value);
        put_le(out, r.addend);
    }
    out.insert(out.end(), image.begin(), image.end());
    return out;
}

// ---- SFO & BBCONT01 (scripts/prepare.py sfo + scripts/content_profile.py) ----
struct SfoValue {
    std::optional<uint32_t> number;
    std::string text;
};

std::map<std::string, SfoValue> parse_sfo(View data) {
    if (data.empty()) fail("param.sfo is empty (0 bytes)");
    View head = span(data, 0, 20);
    if (std::memcmp(head.data(), "\0PSF", 4) != 0) fail("bad SFO signature");
    const uint32_t keys = read_le<uint32_t>(head, 8);
    const uint32_t values = read_le<uint32_t>(head, 12);
    const uint32_t count = read_le<uint32_t>(head, 16);
    std::map<std::string, SfoValue> result;
    for (uint32_t i = 0; i < count; ++i) {
        View entry = span(data, 20 + uint64_t(i) * 16, 16);
        const uint16_t key = read_le<uint16_t>(entry, 0);
        const uint16_t format = read_le<uint16_t>(entry, 2);
        const uint32_t size = read_le<uint32_t>(entry, 4);
        const uint32_t offset = read_le<uint32_t>(entry, 12);
        const uint64_t start = uint64_t(keys) + key;
        if (start >= data.size()) fail("bad SFO key");
        auto end = std::find(data.begin() + static_cast<ptrdiff_t>(start), data.end(), uint8_t(0));
        if (end == data.end()) fail("bad SFO key");
        View value = span(data, uint64_t(values) + offset, size);
        SfoValue parsed;
        if (format == 0x404) {
            if (size != 4) fail("bad SFO integer");
            parsed.number = read_le<uint32_t>(value, 0);
        } else {
            size_t len = value.size();
            while (len && value[len - 1] == 0) --len;
            parsed.text.assign(reinterpret_cast<const char*>(value.data()), len);
        }
        result[std::string(data.begin() + static_cast<ptrdiff_t>(start), end)] = std::move(parsed);
    }
    return result;
}

Bytes build_content_profile(const std::map<std::string, SfoValue>& values) {
    Bytes out;
    put_text(out, "BBCONT01", 8);
    put_le<uint32_t>(out, 3u); // full SKU
    for (int i = 1; i <= 4; ++i) {
        auto it = values.find("USER_DEFINED_PARAM_" + std::to_string(i));
        uint32_t v = 0;
        if (it != values.end()) {
            if (!it->second.number) fail("invalid user-defined parameter in param.sfo");
            v = *it->second.number;
        }
        put_le<uint32_t>(out, v);
    }
    return out;
}

// ---- XML & BBPATCH2 compilation (scripts/patches.py) ----
struct XmlLine {
    std::string type;
    std::string address;
    std::string value;
};

struct XmlMetadata {
    std::string name;
    std::string app_ver;
    std::string app_elf = "eboot.bin";
    std::vector<XmlLine> lines;
};

std::string xml_unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            if (s.substr(i, 5) == "&amp;") { out.push_back('&'); i += 4; continue; }
            if (s.substr(i, 6) == "&quot;") { out.push_back('"'); i += 5; continue; }
            if (s.substr(i, 6) == "&apos;") { out.push_back('\''); i += 5; continue; }
            if (s.substr(i, 4) == "&lt;") { out.push_back('<'); i += 3; continue; }
            if (s.substr(i, 4) == "&gt;") { out.push_back('>'); i += 3; continue; }
        }
        out.push_back(s[i]);
    }
    return out;
}

std::map<std::string, std::string> parse_xml_attrs(std::string_view tag_body) {
    std::map<std::string, std::string> attrs;
    size_t pos = 0;
    while (pos < tag_body.size()) {
        while (pos < tag_body.size() &&
               (std::isspace(static_cast<unsigned char>(tag_body[pos])) || tag_body[pos] == '/')) {
            ++pos;
        }
        if (pos >= tag_body.size()) break;
        const size_t eq = tag_body.find('=', pos);
        if (eq == std::string_view::npos) break;
        std::string key(tag_body.substr(pos, eq - pos));
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
        pos = eq + 1;
        while (pos < tag_body.size() && std::isspace(static_cast<unsigned char>(tag_body[pos]))) ++pos;
        if (pos >= tag_body.size() || (tag_body[pos] != '"' && tag_body[pos] != '\'')) break;
        const char quote = tag_body[pos++];
        const size_t end_q = tag_body.find(quote, pos);
        if (end_q == std::string_view::npos) break;
        attrs[key] = xml_unescape(tag_body.substr(pos, end_q - pos));
        pos = end_q + 1;
    }
    return attrs;
}

std::vector<XmlMetadata> parse_patch_xml(std::string_view xml) {
    std::vector<XmlMetadata> metas;
    size_t pos = 0;
    while (pos < xml.size()) {
        const size_t m_open = xml.find("<Metadata", pos);
        if (m_open == std::string_view::npos) break;
        const size_t m_tag_end = xml.find('>', m_open + 9);
        if (m_tag_end == std::string_view::npos) break;
        const size_t m_close = xml.find("</Metadata>", m_tag_end + 1);
        if (m_close == std::string_view::npos) break;

        auto attrs = parse_xml_attrs(xml.substr(m_open + 9, m_tag_end - (m_open + 9)));
        XmlMetadata meta;
        meta.name = attrs["Name"];
        meta.app_ver = attrs["AppVer"];
        if (attrs.count("AppElf")) meta.app_elf = attrs["AppElf"];

        std::string_view inner = xml.substr(m_tag_end + 1, m_close - (m_tag_end + 1));
        size_t lpos = 0;
        while (lpos < inner.size()) {
            const size_t l_open = inner.find("<Line", lpos);
            if (l_open == std::string_view::npos) break;
            const size_t l_end = inner.find('>', l_open + 5);
            if (l_end == std::string_view::npos) break;
            auto lattrs = parse_xml_attrs(inner.substr(l_open + 5, l_end - (l_open + 5)));
            if (lattrs.count("Type") && lattrs.count("Address") && lattrs.count("Value")) {
                meta.lines.push_back({lattrs["Type"], lattrs["Address"], lattrs["Value"]});
            }
            lpos = l_end + 1;
        }
        metas.push_back(std::move(meta));
        pos = m_close + 11;
    }
    return metas;
}

Bytes encode_patch_line(const XmlLine& line) {
    if (line.type == "bytes") return parse_hex_bytes(line.value);
    if (line.type == "bytes16" || line.type == "bytes32" || line.type == "bytes64") {
        const size_t width = (line.type == "bytes16") ? 2 : (line.type == "bytes32" ? 4 : 8);
        const Wide raw = parse_int(line.value, true);
        uint64_t u = 0;
        if (raw < 0) {
            const int64_t s = as_i64(raw);
            std::memcpy(&u, &s, sizeof(u));
        } else {
            u = as_u64(raw);
        }
        Bytes out(width);
        for (size_t i = 0; i < width; ++i) out[i] = static_cast<uint8_t>((u >> (i * 8)) & 0xffu);
        return out;
    }
    if (line.type == "float32") {
        float f = std::strtof(line.value.c_str(), nullptr);
        Bytes out(4);
        std::memcpy(out.data(), &f, 4);
        return out;
    }
    if (line.type == "float64") {
        double d = std::strtod(line.value.c_str(), nullptr);
        Bytes out(8);
        std::memcpy(out.data(), &d, 8);
        return out;
    }
    if (line.type == "utf8") {
        Bytes out(line.value.begin(), line.value.end());
        out.push_back(0);
        return out;
    }
    fail("unsupported patch line type: " + line.type);
}

std::vector<std::pair<uint64_t, uint64_t>> eboot_pt_load_segments(View elf) {
    const uint64_t phoff = read_le<uint64_t>(elf, 0x20);
    const uint16_t phentsize = read_le<uint16_t>(elf, 0x36);
    const uint16_t phnum = read_le<uint16_t>(elf, 0x38);
    std::vector<std::pair<uint64_t, uint64_t>> segs;
    for (uint16_t i = 0; i < phnum; ++i) {
        View ph = span(elf, phoff + uint64_t(i) * phentsize, 56);
        if (read_le<uint32_t>(ph, 0) == 1) {
            const uint64_t vaddr = read_le<uint64_t>(ph, 16);
            const uint64_t memsz = read_le<uint64_t>(ph, 40);
            segs.emplace_back(vaddr, vaddr + memsz);
        }
    }
    return segs;
}

Bytes eboot_slice_bytes(View elf, uint64_t start, uint64_t end) {
    const uint64_t phoff = read_le<uint64_t>(elf, 0x20);
    const uint16_t phentsize = read_le<uint16_t>(elf, 0x36);
    const uint16_t phnum = read_le<uint16_t>(elf, 0x38);
    Bytes out(static_cast<size_t>(end - start), 0);
    for (uint16_t i = 0; i < phnum; ++i) {
        View ph = span(elf, phoff + uint64_t(i) * phentsize, 56);
        if (read_le<uint32_t>(ph, 0) != 1) continue;
        const uint64_t offset = read_le<uint64_t>(ph, 8);
        const uint64_t vaddr = read_le<uint64_t>(ph, 16);
        const uint64_t filesz = read_le<uint64_t>(ph, 32);
        const uint64_t low = std::max(start, vaddr);
        const uint64_t high = std::min(end, vaddr + filesz);
        if (low < high) {
            View src = span(elf, offset + (low - vaddr), high - low);
            std::copy(src.begin(), src.end(), out.begin() + static_cast<ptrdiff_t>(low - start));
        }
    }
    return out;
}

std::string compute_group_fingerprint(View elf, const std::vector<XmlMetadata>& metas,
                                      const std::string& app_ver,
                                      const std::set<std::string>& group_names) {
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    for (const auto& m : metas) {
        if (m.app_ver != app_ver || !group_names.count(m.name)) continue;
        for (const auto& line : m.lines) {
            const uint64_t addr = as_u64(parse_int(line.address, true));
            if (addr < kEbootBase) fail("patch address below EBOOT_BASE");
            const uint64_t off = addr - kEbootBase;
            const Bytes enc = encode_patch_line(line);
            ranges.emplace_back(off, off + enc.size());
        }
    }
    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<uint64_t, uint64_t>> merged;
    for (auto [s, e] : ranges) {
        if (!merged.empty() && s <= merged.back().second) {
            merged.back().second = std::max(merged.back().second, e);
        } else {
            merged.emplace_back(s, e);
        }
    }
    Sha256 sha;
    for (auto [s, e] : merged) {
        sha.update(&s, sizeof(s));
        sha.update(&e, sizeof(e));
        const Bytes bytes = eboot_slice_bytes(elf, s, e);
        sha.update(bytes.data(), bytes.size());
    }
    return sha.finish_hex();
}

std::vector<std::pair<uint64_t, Bytes>> compile_named_patches(
    const std::vector<XmlMetadata>& metas, const std::vector<std::string>& names,
    const std::string& app_ver, const std::vector<std::pair<uint64_t, uint64_t>>& segments) {
    std::map<std::string, const XmlMetadata*> found;
    for (const auto& m : metas) {
        if (m.app_ver == app_ver && m.app_elf == "eboot.bin") {
            found[m.name] = &m;
        }
    }
    std::vector<std::pair<uint64_t, Bytes>> writes;
    for (const auto& name : names) {
        auto it = found.find(name);
        if (it == found.end()) fail("patch not found for " + app_ver + ": " + name);
        for (const auto& line : it->second->lines) {
            const uint64_t addr = as_u64(parse_int(line.address, true));
            if (addr < kEbootBase) fail(name + ": address below EBOOT_BASE");
            const uint64_t offset = addr - kEbootBase;
            Bytes data = encode_patch_line(line);
            const bool ok = std::any_of(segments.begin(), segments.end(), [&](const auto& seg) {
                return seg.first <= offset && offset + data.size() <= seg.second;
            });
            if (!ok) fail(name + ": address " + line.address + " is outside eboot PT_LOAD");
            writes.emplace_back(offset, std::move(data));
        }
    }
    return writes;
}

} // namespace

std::string CheckTitle(const Bytes& sfo, std::string* out_title_id, std::string* out_app_ver) {
    std::map<std::string, SfoValue> values;
    try {
        values = parse_sfo(sfo);
    } catch (const std::exception& e) {
        return std::string("has an unreadable sce_sys/param.sfo (") + e.what() + ").";
    }
    const std::string id = values.count("TITLE_ID") ? values["TITLE_ID"].text : "";
    const std::string title = values.count("TITLE") ? values["TITLE"].text : "another game";
    const std::string ver = values.count("APP_VER") ? values["APP_VER"].text : "";
    if (out_title_id) *out_title_id = id;
    if (out_app_ver) *out_app_ver = ver;
    for (const char* known : kKnownTitles) {
        if (id == known) return {};
    }
    return "is " + title + (id.empty() ? "" : (" (" + id + ")")) +
           ", not Bloodborne (CUSA00900 / CUSA00207 / CUSA03173 / etc.).";
}

uint64_t Fingerprint(const Dump& dump, const std::string& redzone_json) {
    uint64_t hash = 0xcbf29ce484222325ull;
    auto mix = [&](const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        const uint64_t n = size;
        for (size_t i = 0; i < sizeof(n); ++i) {
            hash = (hash ^ ((n >> (i * 8)) & 0xffu)) * 0x100000001b3ull;
        }
        for (size_t i = 0; i < size; ++i) {
            hash = (hash ^ bytes[i]) * 0x100000001b3ull;
        }
    };
    mix(dump.eboot.data(), dump.eboot.size());
    mix(dump.libc.data(), dump.libc.size());
    mix(dump.fios.data(), dump.fios.size());
    mix(dump.sfo.data(), dump.sfo.size());
    mix(redzone_json.data(), redzone_json.size());
    return hash;
}

BootPrepared PrepareBoot(const Dump& dump, const std::string& redzone_json, bool enable_red_zones) {
    std::string title_id, app_ver;
    if (auto problem = CheckTitle(dump.sfo, &title_id, &app_ver); !problem.empty()) {
        fail("param.sfo " + problem);
    }
    const auto sfo_values = parse_sfo(dump.sfo);
    const Module main_mod = load_module(dump.eboot);
    Probe probe = prepare_eboot(main_mod.self, init_env_is_ret(dump.libc));
    std::vector<Module> bundled;
    bundled.push_back(load_module(dump.libc));
    bundled.push_back(load_module(dump.fios));

    BootPrepared out;
    out.boot_linked = link_bbprobe5(main_mod, probe, bundled, redzone_json, enable_red_zones);
    out.content = build_content_profile(sfo_values);
    out.eboot_elf = main_mod.self.elf;
    out.title_id = title_id;
    out.title = sfo_values.count("TITLE") ? sfo_values.at("TITLE").text : "Bloodborne";
    out.app_ver = app_ver;
    return out;
}

PatchBuildResult BuildPatches(const Bytes& eboot_elf, const std::string& app_ver,
                              const std::string& patch_xml,
                              const std::map<std::string, std::string>& settings,
                              const std::string& fps_mode,
                              const std::string& render_res_override,
                              bool force_patches) {
    auto make_empty = [&](const std::string& reason) {
        PatchBuildResult res;
        put_text(res.patches_bin, "BBPATCH2", 8);
        put_le<uint64_t>(res.patches_bin, kEbootBase);
        put_le<uint64_t>(res.patches_bin, 0);
        res.summary = reason;
        return res;
    };

    const std::string target_ver = "01.09";
    if (!app_ver.empty() && app_ver != target_ver && !force_patches) {
        return make_empty("game version " + app_ver + " != 01.09; running unpatched 30 FPS");
    }

    const auto metas = parse_patch_xml(patch_xml);
    const auto segments = eboot_pt_load_segments(eboot_elf);

    struct EffectSpec {
        const char* key;
        const char* when_off;
        const char* when_on;
    };
    static constexpr EffectSpec kEffects[] = {
        {"effect_chromatic_aberration", "Disable Chromatic Aberration", nullptr},
        {"effect_dof", "Disable DoF", nullptr},
        {"effect_motion_blur", "Disable Motion Blur (perf increase)", nullptr},
        {"effect_ssao", "Disable SSAO", nullptr},
        {"effect_game_aa", "Disable AA", nullptr},
        {"effect_dynamic_shadows", "Disable Dynamic Light Shadows (perf increase)", nullptr},
        {"effect_ssr", nullptr, "Enable Screen Space Reflections (READ NOTE)"},
        {"skip_intro", nullptr, "Skip Intro"},
        {"debug_camera", nullptr, "Restore Debug Camera"},
        {"cheat_no_death", nullptr, "Player No Dead (Read note)"},
        {"cheat_stealth", nullptr, "Player Stealth (Read note)"},
        {"cheat_silent", nullptr, "Player Silent (Read note)"},
        {"cheat_rally_no_decay", nullptr, "No Rally Decay"},
        {"cheat_enemy_control", nullptr, "Enemy Control"},
        {"tweak_no_rally", nullptr, "Disable Rally (HP Regain)"},
        {"tweak_camera_distance", nullptr, "Increased camera distance"},
        {"tweak_no_camera_rotation", nullptr, "Disable Camera Auto Rotation via Movement"},
        {"tweak_easy_run", nullptr, "Sensitive Analog Input (easier to run)"},
        {"tweak_ragdoll", nullptr, "DS1-like physics"},
        {"tweak_circle_confirm", nullptr, "Circle Confirm (Japanese/Asian button layout)"},
    };

    const std::set<std::string> group_fps = {
        "60 FPS++", "90 FPS++", "Uncap FPS++", "Enemy Control",
    };
    const std::set<std::string> group_res = {
        "Increased Graphics Heap Sizes",
        "Resolution Patch 1280x720 (16:9)",
        "Resolution Patch 2560x1080 (21:9)",
    };
    std::set<std::string> group_effects = {
        "Restore Debug Menu (READ NOTES)",
        "Model LOD -2 (Highest)", "Model LOD 1 (Lower)", "Model LOD 2 (Lowest)",
    };
    for (const auto& ef : kEffects) {
        if (ef.when_off && std::string(ef.when_off) != "Enemy Control") group_effects.insert(ef.when_off);
        if (ef.when_on && std::string(ef.when_on) != "Enemy Control") group_effects.insert(ef.when_on);
    }

    bool ok_fps = force_patches ||
                  compute_group_fingerprint(eboot_elf, metas, target_ver, group_fps) ==
                      "6a0f9b8e586dfb9e8cbfe15a852b72d07404589a4728497ebee5568f01bb1197";
    bool ok_res = force_patches ||
                  compute_group_fingerprint(eboot_elf, metas, target_ver, group_res) ==
                      "47357d5f72288b628769eb3f7b7a77b6cc60b29acfd215c9e259a6940c9d77f4";
    bool ok_effects = force_patches ||
                      compute_group_fingerprint(eboot_elf, metas, target_ver, group_effects) ==
                          "49fbe0e9fddc3d02edc4b121f4f0b5a9a79705cb802460b7cf2d4d406771d70b";

    if (!ok_fps && !ok_res && !ok_effects) {
        return make_empty("eboot.bin fingerprint does not match clean 01.09; running unpatched");
    }

    std::vector<std::string> names;
    if (ok_fps) {
        if (fps_mode == "60") names.push_back("60 FPS++");
        else if (fps_mode == "90") names.push_back("90 FPS++");
        else if (fps_mode == "uncap") names.push_back("Uncap FPS++");
    }

    for (const auto& ef : kEffects) {
        auto it = settings.find(ef.key);
        if (it == settings.end()) continue;
        const char* chosen = (it->second == "1") ? ef.when_on : ef.when_off;
        if (!chosen) continue;
        const bool in_fps = (std::string(chosen) == "Enemy Control");
        if ((in_fps && !ok_fps) || (!in_fps && !ok_effects)) continue;
        if (std::find(names.begin(), names.end(), chosen) == names.end()) {
            names.emplace_back(chosen);
        }
    }
    // Resolve mutually conflicting toggles identically to scripts/patches.py:
    if (std::find(names.begin(), names.end(), "Restore Debug Camera") != names.end()) {
        names.erase(std::remove(names.begin(), names.end(), "Enemy Control"), names.end());
    }
    if (std::find(names.begin(), names.end(), "Disable Rally (HP Regain)") != names.end()) {
        names.erase(std::remove(names.begin(), names.end(), "No Rally Decay"), names.end());
    }
    if (ok_effects) {
        auto lod_it = settings.find("model_lod");
        const std::string lod = (lod_it != settings.end()) ? lod_it->second : "0";
        if (lod == "-2") names.emplace_back("Model LOD -2 (Highest)");
        else if (lod == "1") names.emplace_back("Model LOD 1 (Lower)");
        else if (lod == "2") names.emplace_back("Model LOD 2 (Lowest)");
    }

    auto writes = compile_named_patches(metas, names, target_ver, segments);

    // Determine scene render resolution (default 1280x720 on Xbox via preset=1, or explicit WxH).
    int render_w = 1920, render_h = 1080;
    if (!render_res_override.empty()) {
        int w = 0, h = 0;
        if (std::sscanf(render_res_override.c_str(), "%dx%d", &w, &h) == 2 && w >= 320 && h >= 180) {
            render_w = w;
            render_h = h;
        }
    } else {
        int preset = 1; // Xbox default: Quality (1280x720 scene + 1920x1080 native HUD)
        if (auto it = settings.find("preset"); it != settings.end() && !it->second.empty()) {
            preset = std::clamp(std::atoi(it->second.c_str()), 0, 4);
        }
        static constexpr double kScales[5] = {1.0, 1.5, 1.7, 2.0, 3.0};
        const double scale = kScales[preset];
        if (scale > 1.0) {
            render_w = std::max(2, static_cast<int>(std::lround(1920.0 / scale / 2.0)) * 2);
            render_h = std::max(2, static_cast<int>(std::lround(1080.0 / scale / 2.0)) * 2);
        }
    }

    if (ok_res && (render_w != 1920 || render_h != 1080)) {
        auto res_writes = compile_named_patches(
            metas, {"Resolution Patch 1280x720 (16:9)"}, target_ver, segments);
        constexpr uint64_t kSceneWidth = 0x02196A6B - kEbootBase;
        constexpr uint64_t kSceneHeight = 0x02196A7A - kEbootBase;
        constexpr uint64_t kUiWidth = 0x02358554 - kEbootBase;
        constexpr uint64_t kUiHeight = 0x0235855D - kEbootBase;
        struct Rep {
            uint64_t offset;
            uint8_t opcode;
            uint32_t expected;
            uint32_t replacement;
        };
        const Rep reps[4] = {
            {kSceneWidth, 0xB8, 0x500, static_cast<uint32_t>(render_w)},
            {kSceneHeight, 0xB8, 0x2D0, static_cast<uint32_t>(render_h)},
            {kUiWidth, 0xB8, 0x500, 1920u},
            {kUiHeight, 0xB9, 0x2D0, 1080u},
        };
        unsigned matched_mask = 0;
        for (auto& [off, data] : res_writes) {
            for (int r = 0; r < 4; ++r) {
                if (off != reps[r].offset) continue;
                const uint8_t exp[4] = {
                    reps[r].opcode,
                    static_cast<uint8_t>(reps[r].expected & 0xffu),
                    static_cast<uint8_t>((reps[r].expected >> 8) & 0xffu),
                    static_cast<uint8_t>((reps[r].expected >> 16) & 0xffu),
                };
                if (data.size() != 4 || std::memcmp(data.data(), exp, 4) != 0 ||
                    (matched_mask & (1u << r))) {
                    fail("unexpected resolution patch bytes at " + hex(off + kEbootBase));
                }
                data = {
                    reps[r].opcode,
                    static_cast<uint8_t>(reps[r].replacement & 0xffu),
                    static_cast<uint8_t>((reps[r].replacement >> 8) & 0xffu),
                    static_cast<uint8_t>((reps[r].replacement >> 16) & 0xffu),
                };
                matched_mask |= (1u << r);
            }
            writes.emplace_back(off, std::move(data));
        }
        if (matched_mask != 0x0fu) {
            fail("resolution patch template missing scene/UI viewport instructions");
        }
        if (render_w * render_h > 1920 * 1080) {
            auto heap_writes = compile_named_patches(
                metas, {"Increased Graphics Heap Sizes"}, target_ver, segments);
            for (auto& w : heap_writes) writes.push_back(std::move(w));
        }
    }

    PatchBuildResult out;
    out.render_width = render_w;
    out.render_height = render_h;
    put_text(out.patches_bin, "BBPATCH2", 8);
    put_le<uint64_t>(out.patches_bin, kEbootBase);
    put_le<uint64_t>(out.patches_bin, writes.size());
    for (const auto& [off, data] : writes) {
        put_le<uint64_t>(out.patches_bin, off);
        put_le<uint64_t>(out.patches_bin, data.size());
        out.patches_bin.insert(out.patches_bin.end(), data.begin(), data.end());
    }
    out.summary = "FPS=" + fps_mode + ", scene=" + std::to_string(render_w) + "x" +
                  std::to_string(render_h) + ", UI=1920x1080, writes=" +
                  std::to_string(writes.size());
    return out;
}

} // namespace BbPrepare

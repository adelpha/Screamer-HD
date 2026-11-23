#include "core/archive.h"

#include "core/gamedata.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace scr {
namespace {

uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

} // namespace

std::optional<Archive> Archive::from_bytes(std::vector<uint8_t> bytes) {
    if (bytes.size() < 4) return std::nullopt;
    const uint32_t dir_size = le32(bytes.data() + bytes.size() - 4);
    if (dir_size > bytes.size() - 4) return std::nullopt;
    Archive a;
    size_t pos = bytes.size() - 4 - dir_size;
    while (pos + 0x20 <= bytes.size() - 4) {
        const uint8_t* e = bytes.data() + pos;
        ArchiveEntry entry;
        for (int i = 0; i < 16; ++i) {
            const char c = static_cast<char>(e[i] ^ 0x3A);
            if (c == '\0') break;
            entry.name.push_back(c);
        }
        if (entry.name.empty()) break;
        entry.offset = le32(e + 16);
        entry.packed_size = le32(e + 20);
        entry.size = le32(e + 24);
        entry.flags = le32(e + 28);
        const uint32_t stored = entry.compressed() ? entry.packed_size : entry.size;
        if (entry.offset > bytes.size() || stored > bytes.size() - entry.offset) return std::nullopt;
        a.entries_.push_back(std::move(entry));
        pos += 0x20;
    }
    a.bytes_ = std::move(bytes);
    return a;
}

std::optional<Archive> Archive::open(const std::string& path) {
    auto bytes = read_file(path);
    if (!bytes) return std::nullopt;
    return from_bytes(std::move(*bytes));
}

const ArchiveEntry* Archive::find(std::string_view name) const {
    for (const auto& e : entries_) {
        if (iequals(e.name, name)) return &e;
    }
    return nullptr;
}

std::optional<std::vector<uint8_t>> Archive::read(std::string_view name) const {
    const ArchiveEntry* e = find(name);
    if (!e) return std::nullopt;
    return read(*e);
}

std::optional<std::vector<uint8_t>> Archive::read(const ArchiveEntry& e) const {
    const uint8_t* src = bytes_.data() + e.offset;
    if (!e.compressed()) return std::vector<uint8_t>(src, src + e.size);
    auto out = lzw_decode(src, e.packed_size, e.size);
    if (!out || out->size() != e.size) return std::nullopt;
    return out;
}

std::optional<std::vector<uint8_t>> lzw_decode(const uint8_t* src, size_t src_size, size_t out_size) {
    constexpr uint32_t kClear = 0x100, kEnd = 0x101, kFirst = 0x102, kMaxBits = 12;
    std::vector<uint16_t> prefix(1u << kMaxBits);
    std::vector<uint8_t> suffix(1u << kMaxBits);
    std::vector<uint8_t> stack;
    std::vector<uint8_t> out;
    out.reserve(out_size);

    size_t bitpos = 0;
    uint32_t width = 9, limit = 0x200, next = kFirst;
    auto read_code = [&](uint32_t& code) -> bool {
        const size_t byte = bitpos >> 3;
        if (byte >= src_size) return false;
        uint32_t v = 0;
        for (size_t i = 0; i < 3 && byte + i < src_size; ++i) v |= uint32_t(src[byte + i]) << (8 * i);
        code = (v >> (bitpos & 7)) & ((1u << width) - 1);
        bitpos += width;
        return true;
    };
    auto expand = [&](uint32_t code) {
        stack.clear();
        while (code > 0xFF) {
            stack.push_back(suffix[code]);
            code = prefix[code];
        }
        stack.push_back(static_cast<uint8_t>(code));
        std::reverse(stack.begin(), stack.end());
    };

    uint32_t prev = 0;
    bool have_prev = false;
    while (out.size() < out_size) {
        uint32_t code;
        if (!read_code(code) || code == kEnd) break;
        if (code == kClear) {
            width = 9;
            limit = 0x200;
            next = kFirst;
            if (!read_code(prev) || prev > 0xFF) return std::nullopt;
            out.push_back(static_cast<uint8_t>(prev));
            have_prev = true;
            continue;
        }
        if (!have_prev) {  // a stream that does not start with a clear code
            if (code > 0xFF) return std::nullopt;
            out.push_back(static_cast<uint8_t>(code));
            prev = code;
            have_prev = true;
            continue;
        }
        uint8_t first;
        if (code < next) {
            expand(code);
            first = stack.front();
        } else if (code == next) {  // the KwKwK case
            expand(prev);
            first = stack.front();
            stack.push_back(first);
        } else {
            return std::nullopt;
        }
        out.insert(out.end(), stack.begin(), stack.end());
        if (next < (1u << kMaxBits)) {
            prefix[next] = static_cast<uint16_t>(prev);
            suffix[next] = first;
            ++next;
        }
        if (next >= limit && width != kMaxBits) {
            ++width;
            limit <<= 1;
        }
        prev = code;
    }
    if (out.size() > out_size) out.resize(out_size);
    return out;
}

} // namespace scr

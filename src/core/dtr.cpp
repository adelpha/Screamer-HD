#include "core/dtr.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace scr {
namespace {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

// Splits into lines, dropping '\r' like archive_read_line does.
std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            std::string_view line = text.substr(start, i - start);
            while (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            lines.push_back(line);
            start = i + 1;
        }
    }
    return lines;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

} // namespace

std::optional<std::vector<DtrValue>> DtrFile::section(std::string_view name) const {
    const auto lines = split_lines(text_);
    size_t i = 0;
    // The section header is matched against whole lines.
    while (i < lines.size() && !iequals(trim(lines[i]), name)) ++i;
    if (i == lines.size()) return std::nullopt;

    std::vector<DtrValue> values;
    for (++i; i < lines.size(); ++i) {
        std::string_view line = lines[i];
        size_t p = 0;
        while (p < line.size()) {
            while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
            size_t q = p;
            while (q < line.size() && line[q] != ' ' && line[q] != '\t') ++q;
            const std::string_view tok = line.substr(p, q - p);
            p = q;
            if (tok.empty()) continue;
            if (iequals(tok, "END_LIST")) return values;
            if (tok == "*") break;  // comment: skip the rest of the line
            const std::string body(tok.substr(1));
            DtrValue v;
            v.type = tok[0];
            switch (tok[0]) {
            case 'd': v.i = std::atoi(body.c_str()); break;
            case 'x': v.i = static_cast<int32_t>(std::strtoul(body.c_str(), nullptr, 16)); break;
            case 'f': v.f = static_cast<float>(std::strtod(body.c_str(), nullptr)); break;
            case 's': v.text = body; break;
            default: continue;  // the game ignores other tokens
            }
            values.push_back(std::move(v));
        }
    }
    return values;  // file ended without END_LIST
}

std::optional<std::vector<uint8_t>> DtrFile::pack_section(std::string_view name, size_t stride, size_t record_bytes,
                                                          size_t* count) const {
    auto values = section(name);
    if (!values || record_bytes == 0 || stride < record_bytes) return std::nullopt;
    std::vector<uint8_t> out;
    size_t record = 0, offset = 0;
    for (const auto& v : *values) {
        const size_t need = record * stride + offset + v.byte_size();
        if (out.size() < need) out.resize(record * stride + record_bytes, 0);
        uint8_t* dst = out.data() + record * stride + offset;
        if (v.type == 's') {
            // strcpy into the record: up to 11 characters plus the terminator (the game
            // does not check the length; longer names would run into the next field).
            const size_t n = std::min<size_t>(v.text.size(), 11);
            std::memcpy(dst, v.text.data(), n);
            dst[n] = 0;
        } else if (v.type == 'f') {
            std::memcpy(dst, &v.f, 4);
        } else {
            std::memcpy(dst, &v.i, 4);
        }
        offset += v.byte_size();
        if (offset == record_bytes) {
            offset = 0;
            ++record;
        }
    }
    if (count) *count = record;
    out.resize(record * stride);
    return out;
}

} // namespace scr

#include "core/texture_pack.h"

#include "core/image.h"
#include "core/pcx.h"
#include "core/track.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace scr {
namespace {

struct Fnv {
    uint64_t h = 0xcbf29ce484222325ull;
    void byte(uint8_t b) {
        h ^= b;
        h *= 0x100000001b3ull;
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) byte(uint8_t(v >> (8 * i)));
    }
    uint64_t value() const { return (h ^ (h >> 48)) & 0xffffffffffffull; }
};

} // namespace

bool texture_has_holes(const Material& m) { return m.type >= 3 && m.type <= 7; }

bool texture_replaceable(const Material& m) {
    const int kind = m.type >> 1;
    return m.width > 0 && m.height > 0 && kind != 4 && kind != 6 && kind != 7;
}

int texture_frames(const Material& m) { return std::max(1, int(m.frame_pixels.size())); }

RgbaImage material_rgba(const MaterialLibrary& lib, const Material& m, int frame, const Palette& palette) {
    RgbaImage img;
    img.width = std::max(0, m.width);
    img.height = std::max(0, m.height);
    img.pixels.resize(size_t(img.width) * size_t(img.height) * 4);
    const bool holes = texture_has_holes(m);
    for (int v = 0; v < img.height; ++v)
        for (int u = 0; u < img.width; ++u) {
            const uint8_t idx = lib.texel(m, u, v, frame);
            uint8_t* o = img.pixels.data() + (size_t(v) * size_t(img.width) + size_t(u)) * 4;
            o[0] = palette[idx].r;
            o[1] = palette[idx].g;
            o[2] = palette[idx].b;
            o[3] = holes && idx == 0 ? 0 : 255;
        }
    return img;
}

RgbaImage panorama_rgba(const PcxImage& pano, const Palette& palette) {
    RgbaImage img;
    img.width = pano.width;
    img.height = pano.height;
    img.pixels.resize(size_t(img.width) * size_t(img.height) * 4);
    for (size_t i = 0; i < size_t(img.width) * size_t(img.height) && i < pano.pixels.size(); ++i) {
        const uint8_t idx = pano.pixels[i];
        img.pixels[i * 4 + 0] = palette[idx].r;
        img.pixels[i * 4 + 1] = palette[idx].g;
        img.pixels[i * 4 + 2] = palette[idx].b;
        img.pixels[i * 4 + 3] = 255;
    }
    return img;
}

uint64_t texture_hash(const MaterialLibrary& lib, const Material& m, const Palette& palette) {
    Fnv f;
    f.u32(uint32_t(m.width));
    f.u32(uint32_t(m.height));
    const int frames = texture_frames(m);
    f.u32(uint32_t(frames));
    for (int fr = 0; fr < frames; ++fr) {
        const RgbaImage img = material_rgba(lib, m, fr, palette);
        for (uint8_t b : img.pixels) f.byte(b);
    }
    return f.value();
}

uint64_t texture_index_hash(const MaterialLibrary& lib, const Material& m) {
    Fnv f;
    f.u32(0x696478);  // "idx"
    f.u32(uint32_t(m.width));
    f.u32(uint32_t(m.height));
    const int frames = texture_frames(m);
    f.u32(uint32_t(frames));
    for (int fr = 0; fr < frames; ++fr)
        for (int v = 0; v < m.height; ++v)
            for (int u = 0; u < m.width; ++u) f.byte(lib.texel(m, u, v, fr));
    return f.value();
}

uint64_t texture_hash(const PcxImage& pano, const Palette& palette) {
    Fnv f;
    f.u32(uint32_t(pano.width));
    f.u32(uint32_t(pano.height));
    f.u32(0x736b79);  // "sky"
    const RgbaImage img = panorama_rgba(pano, palette);
    for (uint8_t b : img.pixels) f.byte(b);
    return f.value();
}

std::string hash_name(uint64_t hash) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%012llx", static_cast<unsigned long long>(hash & 0xffffffffffffull));
    return buf;
}

bool parse_texture_name(const std::string& file_name, uint64_t& hash, int& frame) {
    std::string s = file_name;
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (s.size() < 4 || s.compare(s.size() - 4, 4, ".png") != 0) return false;
    s.resize(s.size() - 4);
    frame = -1;
    // a frame suffix: _f<digits>
    const size_t us = s.rfind('_');
    if (us != std::string::npos && s.size() > us + 2 && s[us + 1] == 'f' &&
        std::all_of(s.begin() + ptrdiff_t(us + 2), s.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        if (s.size() - us - 2 > 4) return false;
        frame = std::atoi(s.c_str() + us + 2);
        s.resize(us);
    }
    if (s.size() < 13 || s[s.size() - 13] != '_') return false;
    uint64_t h = 0;
    for (size_t i = s.size() - 12; i < s.size(); ++i) {
        const char c = s[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return false;
        h = (h << 4) | uint64_t(d);
    }
    hash = h;
    return true;
}

TexturePack TexturePack::scan(const std::string& dir) {
    TexturePack p;
    p.dir_ = dir;
    namespace fs = std::filesystem;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(fs::path(dir), ec)) return p;
    std::vector<fs::path> found;
    for (fs::recursive_directory_iterator it(fs::path(dir), fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) found.push_back(it->path());
    }
    std::sort(found.begin(), found.end());
    for (const fs::path& f : found) {
        uint64_t hash;
        int frame;
        if (!parse_texture_name(f.filename().string(), hash, frame)) continue;
        auto& frames = p.files_[hash];
        if (frames.count(frame)) {
            SDL_Log("textures: %s is another file for %s, not used", f.string().c_str(), frames[frame].c_str());
            continue;
        }
        frames[frame] = f.string();
    }
    return p;
}

std::string TexturePack::path(uint64_t hash, int frame) const {
    const auto it = files_.find(hash);
    if (it == files_.end()) return {};
    auto f = it->second.find(frame);
    if (f == it->second.end()) f = it->second.find(-1);
    if (f == it->second.end() && frame == 0 && it->second.size() == 1) f = it->second.begin();
    return f == it->second.end() ? std::string() : f->second;
}

std::optional<RgbaImage> TexturePack::load(uint64_t hash, int frame) const {
    const std::string file = path(hash, frame);
    if (file.empty()) return std::nullopt;
    SurfacePtr s = load_image_rgba(file);
    if (!s) {
        SDL_Log("textures: cannot read %s: %s", file.c_str(), SDL_GetError());
        return std::nullopt;
    }
    RgbaImage img;
    img.width = s->w;
    img.height = s->h;
    img.pixels.resize(size_t(s->w) * size_t(s->h) * 4);
    for (int y = 0; y < s->h; ++y)
        std::memcpy(img.pixels.data() + size_t(y) * size_t(s->w) * 4, static_cast<const uint8_t*>(s->pixels) + size_t(y) * size_t(s->pitch),
                    size_t(s->w) * 4);
    return img;
}

RgbaImage resample(const RgbaImage& img, int w, int h) {
    if (img.width == w && img.height == h) return img;
    RgbaImage out;
    out.width = w;
    out.height = h;
    out.pixels.resize(size_t(w) * size_t(h) * 4);
    if (img.width <= 0 || img.height <= 0) return out;
    if (w % img.width == 0 && h % img.height == 0) {
        // whole multiples larger: each texel repeated (nearest)
        const int sx = w / img.width, sy = h / img.height;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                std::memcpy(out.pixels.data() + (size_t(y) * size_t(w) + size_t(x)) * 4,
                            img.pixels.data() + (size_t(y / sy) * size_t(img.width) + size_t(x / sx)) * 4, 4);
        return out;
    }
    if (img.width % w == 0 && img.height % h == 0) {
        // box filter (colour weighted by alpha, so holes do not darken the edges)
        const int bx = img.width / w, by = img.height / h;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                double r = 0, g = 0, b = 0, a = 0;
                for (int j = 0; j < by; ++j)
                    for (int i = 0; i < bx; ++i) {
                        const uint8_t* p = img.pixels.data() + (size_t(y * by + j) * size_t(img.width) + size_t(x * bx + i)) * 4;
                        const double wa = p[3] / 255.0;
                        r += p[0] * wa;
                        g += p[1] * wa;
                        b += p[2] * wa;
                        a += wa;
                    }
                uint8_t* o = out.pixels.data() + (size_t(y) * size_t(w) + size_t(x)) * 4;
                const double n = double(bx * by);
                o[0] = uint8_t(a > 0 ? std::lround(r / a) : 0);
                o[1] = uint8_t(a > 0 ? std::lround(g / a) : 0);
                o[2] = uint8_t(a > 0 ? std::lround(b / a) : 0);
                o[3] = uint8_t(std::lround(a / n * 255.0));
            }
        return out;
    }
    // bilinear, wrapping (textures repeat)
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double sx = (x + 0.5) * img.width / w - 0.5, sy = (y + 0.5) * img.height / h - 0.5;
            const int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
            const double fx = sx - x0, fy = sy - y0;
            double acc[4] = {};
            for (int k = 0; k < 4; ++k) {
                const int xx = ((x0 + (k & 1)) % img.width + img.width) % img.width;
                const int yy = ((y0 + (k >> 1)) % img.height + img.height) % img.height;
                const double wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
                const uint8_t* p = img.pixels.data() + (size_t(yy) * size_t(img.width) + size_t(xx)) * 4;
                for (int c = 0; c < 4; ++c) acc[c] += p[c] * wgt;
            }
            uint8_t* o = out.pixels.data() + (size_t(y) * size_t(w) + size_t(x)) * 4;
            for (int c = 0; c < 4; ++c) o[c] = uint8_t(std::clamp(std::lround(acc[c]), 0l, 255l));
        }
    return out;
}

} // namespace scr

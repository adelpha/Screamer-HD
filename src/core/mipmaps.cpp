#include "core/mipmaps.h"

#include "core/track.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace scr {
namespace {

struct Rgba {
    float r = 0, g = 0, b = 0, a = 0;  // linear light, times a; a: how much of it is not a hole
};

int level_size(int size, int level) { return std::max(1, (size + ((1 << level) >> 1)) >> level); }

// The nearest palette colour to a 6-bit-per-channel colour, looked up once per colour.
class Nearest {
public:
    Nearest(const Palette& palette, int first) : palette_(palette), first_(first), cache_(64 * 64 * 64, -1) {}
    uint8_t operator()(int r, int g, int b) {
        int16_t& slot = cache_[size_t((r >> 2) << 12 | (g >> 2) << 6 | (b >> 2))];
        if (slot < 0) {
            int best = first_;
            long best_d = -1;
            for (int i = first_; i < 256; ++i) {
                const Rgb& p = palette_[size_t(i)];
                const long dr = p.r - r, dg = p.g - g, db = p.b - b;
                const long d = 3 * dr * dr + 6 * dg * dg + db * db;
                if (best_d < 0 || d < best_d) {
                    best_d = d;
                    best = i;
                }
            }
            slot = int16_t(best);
        }
        return uint8_t(slot);
    }

private:
    const Palette& palette_;
    int first_;
    std::vector<int16_t> cache_;
};

// Averages `in` (n texels, `stride` apart) into `out` (m texels, `out_stride` apart): out
// texel i covers in texels i n / m .. (i + 1) n / m.
void shrink(const Rgba* in, int n, size_t stride, Rgba* out, int m, size_t out_stride) {
    for (int i = 0; i < m; ++i) {
        const double x0 = double(i) * n / m, x1 = double(i + 1) * n / m;
        Rgba sum;
        for (int k = int(x0); k < n && double(k) < x1; ++k) {
            const float w = float(std::min(x1, double(k + 1)) - std::max(x0, double(k)));
            const Rgba& p = in[size_t(k) * stride];
            sum.r += p.r * w;
            sum.g += p.g * w;
            sum.b += p.b * w;
            sum.a += p.a * w;
        }
        const float inv = float(m) / float(n);
        out[size_t(i) * out_stride] = {sum.r * inv, sum.g * inv, sum.b * inv, sum.a * inv};
    }
}

} // namespace

MipSet build_mipmaps(const MaterialLibrary& lib, const Palette& palette) {
    MipSet set;
    const auto& mats = lib.materials();
    const auto& data = lib.data();
    set.first.assign(mats.size(), -1);
    set.frames.assign(mats.size(), 0);

    std::array<float, 256> to_linear{};
    for (size_t i = 0; i < 256; ++i) to_linear[i] = std::pow(float(i) / 255.0f, 2.2f);
    auto to_byte = [](float v) { return int(std::lround(std::pow(std::clamp(v, 0.0f, 1.0f), 1.0f / 2.2f) * 255.0f)); };
    Nearest any(palette, 0), solid(palette, 1);  // (texel 0 is the hole of the materials with holes)

    struct Pending {
        size_t texture;
        size_t offset[detail::kMipLevels][detail::kMipLevels];
    };
    std::vector<Pending> pending;
    std::vector<Rgba> full, across, level;
    for (size_t mi = 0; mi < mats.size(); ++mi) {
        const Material& m = mats[mi];
        const int kind = m.type >> 1;
        if (kind == 4 || kind == 6 || kind == 7) continue;  // flat, darken, translucent
        if (m.width < 1 || m.height < 1 || m.width > 256 || m.height > 256) continue;
        // Not the textures only a few texels across: those are the road's painted lines (six
        // texels wide, white with uneven edges). Averaged along their length the edge columns
        // turn grey, and the line is a third narrower from where the smaller version takes over.
        if (m.width < 8 || m.height < 8) continue;
        const bool holes = m.type >= 3 && m.type <= 7;
        const size_t frames = std::max<size_t>(1, m.frame_pixels.size());
        bool inside = true;
        for (size_t f = 0; f < frames; ++f) {
            const size_t base = m.frame_pixels.empty() ? m.pixels : m.frame_pixels[f];
            if (base + size_t(m.height - 1) * 256 + size_t(m.width) > data.size()) inside = false;
        }
        if (!inside) continue;
        set.first[mi] = int32_t(set.textures.size());
        set.frames[mi] = int32_t(frames);
        const int w = m.width, h = m.height;
        for (size_t f = 0; f < frames; ++f) {
            const uint8_t* src = data.data() + (m.frame_pixels.empty() ? m.pixels : m.frame_pixels[f]);
            detail::MipTexture t{};
            auto axis = [](detail::MipAxis* a, int size) {
                int levels = 0;
                for (int l = 0; l < detail::kMipLevels; ++l) {
                    const int s = level_size(size, l);
                    a[l].size = s;
                    a[l].mask = (s & (s - 1)) == 0 ? s - 1 : -1;
                    a[l].bias = (16384 + s - 1) / s * s;
                    a[l].magic = 65536 / s;
                    a[l].scale = float(s) / float(size);
                    levels = l + 1;
                    // (nothing smaller than eight texels along either side: a fifth of the
                    // textures are 8 to 15 texels across - rails, posts, kerb stripes, more
                    // lines - and halved they lose their edges as the six-texel lines did)
                    if (level_size(size, l + 1) < 8) break;
                }
                return levels;
            };
            t.levels_u = axis(t.u, w);
            t.levels_v = axis(t.v, h);
            Pending p{set.textures.size(), {}};
            full.assign(size_t(w) * size_t(h), {});
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const uint8_t c = src[size_t(y) * 256 + size_t(x)];
                    if (holes && c == 0) continue;
                    const Rgb& rgb = palette[c];
                    full[size_t(y) * size_t(w) + size_t(x)] = {to_linear[rgb.r], to_linear[rgb.g], to_linear[rgb.b], 1.0f};
                }
            for (int lu = 0; lu < t.levels_u; ++lu) {
                const int wl = t.u[lu].size;
                across.assign(size_t(wl) * size_t(h), {});
                for (int y = 0; y < h; ++y) shrink(full.data() + size_t(y) * size_t(w), w, 1, across.data() + size_t(y) * size_t(wl), wl, 1);
                for (int lv = 0; lv < t.levels_v; ++lv) {
                    const int hl = t.v[lv].size;
                    p.offset[lv][lu] = set.data.size();
                    set.data.resize(set.data.size() + size_t(wl) * size_t(hl));
                    uint8_t* out = set.data.data() + p.offset[lv][lu];
                    if (lu == 0 && lv == 0) {
                        // the texture itself, texel for texel
                        for (int y = 0; y < h; ++y) std::copy(src + size_t(y) * 256, src + size_t(y) * 256 + w, out + size_t(y) * size_t(w));
                        continue;
                    }
                    level.assign(size_t(wl) * size_t(hl), {});
                    for (int x = 0; x < wl; ++x) shrink(across.data() + x, h, size_t(wl), level.data() + x, hl, size_t(wl));
                    for (size_t i = 0; i < level.size(); ++i) {
                        const Rgba& c = level[i];
                        if (holes && c.a < 0.5f) {
                            out[i] = 0;
                            continue;
                        }
                        const float a = c.a > 0 ? 1.0f / c.a : 0.0f;
                        const int r = to_byte(c.r * a), g = to_byte(c.g * a), b = to_byte(c.b * a);
                        out[i] = holes ? solid(r, g, b) : any(r, g, b);
                    }
                }
            }
            set.textures.push_back(t);
            pending.push_back(p);
        }
    }
    set.data.resize(set.data.size() + 4, 0);  // (the fillers read four bytes at a texel)
    for (const Pending& p : pending) {
        detail::MipTexture& t = set.textures[p.texture];
        for (int lv = 0; lv < t.levels_v; ++lv)
            for (int lu = 0; lu < t.levels_u; ++lu) t.texels[lv][lu] = set.data.data() + p.offset[lv][lu];
    }
    return set;
}

} // namespace scr

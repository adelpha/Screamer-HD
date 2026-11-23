#include "core/image.h"

#include "core/framebuffer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace scr {
namespace {

bool ends_with_ci(const std::string& s, const char* suffix) {
    const size_t n = SDL_strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != suffix[i]) return false;
    }
    return true;
}

SurfacePtr to_rgba32(SDL_Surface* s) {
    if (!s) return nullptr;
    return SurfacePtr(SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32));
}

} // namespace

SurfacePtr load_image_rgba(const std::string& path) {
    SurfacePtr raw(SDL_LoadSurface(path.c_str()));
    return to_rgba32(raw.get());
}

bool save_image(SDL_Surface* surface, const std::string& path) {
    if (!surface) return false;
    if (ends_with_ci(path, ".bmp")) return SDL_SaveBMP(surface, path.c_str());
    return SDL_SavePNG(surface, path.c_str());
}

SurfacePtr framebuffer_to_surface(const Framebuffer& fb) {
    SurfacePtr s(SDL_CreateSurface(fb.width(), fb.height(), SDL_PIXELFORMAT_RGBA32));
    if (!s) return nullptr;
    fb.to_rgba(static_cast<uint8_t*>(s->pixels), s->pitch);
    return s;
}

SurfacePtr scale_nearest(SDL_Surface* src, int w, int h) {
    if (!src) return nullptr;
    SurfacePtr rgba = to_rgba32(src);
    if (!rgba) return nullptr;
    if (rgba->w == w && rgba->h == h) return rgba;
    return SurfacePtr(SDL_ScaleSurface(rgba.get(), w, h, SDL_SCALEMODE_NEAREST));
}

DiffResult compare_images(SDL_Surface* a, SDL_Surface* b, int tolerance, SurfacePtr* diff_out) {
    DiffResult r;
    if (!a || !b || a->w != b->w || a->h != b->h) {
        r.size_mismatch = true;
        if (a) { r.width = a->w; r.height = a->h; }
        return r;
    }
    SurfacePtr ca = to_rgba32(a), cb = to_rgba32(b);
    r.width = ca->w;
    r.height = ca->h;

    SurfacePtr diff;
    if (diff_out) diff.reset(SDL_CreateSurface(r.width, r.height, SDL_PIXELFORMAT_RGBA32));

    uint64_t abs_sum = 0;
    for (int y = 0; y < r.height; ++y) {
        const uint8_t* pa = static_cast<const uint8_t*>(ca->pixels) + static_cast<size_t>(y) * ca->pitch;
        const uint8_t* pb = static_cast<const uint8_t*>(cb->pixels) + static_cast<size_t>(y) * cb->pitch;
        uint8_t* pd = diff ? static_cast<uint8_t*>(diff->pixels) + static_cast<size_t>(y) * diff->pitch : nullptr;
        for (int x = 0; x < r.width; ++x) {
            int worst = 0;
            for (int c = 0; c < 3; ++c) {
                const int d = std::abs(int(pa[x * 4 + c]) - int(pb[x * 4 + c]));
                abs_sum += static_cast<uint64_t>(d);
                worst = std::max(worst, d);
            }
            r.max_channel_delta = std::max(r.max_channel_delta, worst);
            const bool differs = worst > tolerance;
            if (differs) ++r.differing_pixels;
            if (pd) {
                if (differs) {
                    pd[x * 4 + 0] = 255; pd[x * 4 + 1] = 0; pd[x * 4 + 2] = 0;
                } else {
                    const uint8_t g = static_cast<uint8_t>((pa[x * 4 + 0] * 77 + pa[x * 4 + 1] * 150 + pa[x * 4 + 2] * 29) >> 10);
                    pd[x * 4 + 0] = pd[x * 4 + 1] = pd[x * 4 + 2] = g;
                }
                pd[x * 4 + 3] = 255;
            }
        }
    }
    r.mean_abs_error = r.total_pixels() ? static_cast<double>(abs_sum) / (3.0 * static_cast<double>(r.total_pixels())) : 0.0;
    if (diff_out) *diff_out = std::move(diff);
    return r;
}

} // namespace scr

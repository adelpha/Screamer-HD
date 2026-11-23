#include "core/framebuffer.h"

#include <algorithm>

namespace scr {

Palette palette_from_vga(const uint8_t* rgb6, size_t count) {
    Palette p{};
    count = std::min<size_t>(count, p.size());
    for (size_t i = 0; i < count; ++i) {
        p[i] = {vga6_to_8(rgb6[i * 3 + 0] & 63), vga6_to_8(rgb6[i * 3 + 1] & 63), vga6_to_8(rgb6[i * 3 + 2] & 63)};
    }
    return p;
}

Framebuffer::Framebuffer(int width, int height)
    : width_(width), height_(height), pixels_(static_cast<size_t>(width) * height, 0) {}

void Framebuffer::clear(uint8_t index) { std::fill(pixels_.begin(), pixels_.end(), index); }

void Framebuffer::fill_rect(int x, int y, int w, int h, uint8_t index) {
    const int x0 = std::max(x, 0), y0 = std::max(y, 0);
    const int x1 = std::min(x + w, width_), y1 = std::min(y + h, height_);
    for (int yy = y0; yy < y1; ++yy) {
        std::fill_n(&pixels_[static_cast<size_t>(yy) * width_ + x0], std::max(x1 - x0, 0), index);
    }
}

void Framebuffer::to_rgba(uint8_t* dst, int dst_pitch) const {
    for (int y = 0; y < height_; ++y) {
        const uint8_t* src = &pixels_[static_cast<size_t>(y) * width_];
        uint8_t* row = dst + static_cast<size_t>(y) * dst_pitch;
        for (int x = 0; x < width_; ++x) {
            const Rgb& c = palette_[src[x]];
            row[x * 4 + 0] = c.r;
            row[x * 4 + 1] = c.g;
            row[x * 4 + 2] = c.b;
            row[x * 4 + 3] = 255;
        }
    }
}

} // namespace scr

#include "core/framebuffer.h"

#include "core/worker_pool.h"

#include <algorithm>
#include <cstring>

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

void Framebuffer::to_rgba(uint8_t* dst, int dst_pitch, WorkerPool* pool) const {
    // one 32-bit word per palette entry, in memory order R, G, B, A
    uint32_t lut[256];
    for (int i = 0; i < 256; ++i) {
        const uint8_t px[4] = {palette_[size_t(i)].r, palette_[size_t(i)].g, palette_[size_t(i)].b, 255};
        std::memcpy(&lut[i], px, 4);
    }
    auto rows = [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const uint8_t* src = &pixels_[static_cast<size_t>(y) * width_];
            uint32_t* row = reinterpret_cast<uint32_t*>(dst + static_cast<size_t>(y) * dst_pitch);
            for (int x = 0; x < width_; ++x) row[x] = lut[src[x]];
        }
    };
    if (!pool || height_ < 64) {
        rows(0, height_);
        return;
    }
    const int bands = std::min(height_ / 16, pool->threads() * 2);
    pool->run(bands, [&](int b) { rows(height_ * b / bands, height_ * (b + 1) / bands); });
}

} // namespace scr

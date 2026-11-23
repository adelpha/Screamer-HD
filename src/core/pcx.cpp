#include "core/pcx.h"

namespace scr {

std::optional<PcxImage> decode_pcx(const std::vector<uint8_t>& file) {
    if (file.size() < 0x80 + 0x300) return std::nullopt;
    auto le16 = [&](size_t o) { return int(int16_t(file[o] | file[o + 1] << 8)); };
    PcxImage img;
    img.width = le16(8) - le16(4) + 1;
    img.height = le16(10) - le16(6) + 1;
    if (img.width <= 0 || img.height <= 0 || img.width > 4096 || img.height > 4096) return std::nullopt;

    const size_t total = size_t(img.width) * size_t(img.height);
    img.pixels.reserve(total);
    size_t p = 0x80;
    const size_t end = file.size() - 0x300;
    while (img.pixels.size() < total && p < end) {
        const uint8_t b = file[p++];
        if ((b & 0xC0) == 0xC0) {
            if (p >= end) break;
            const uint8_t v = file[p++];
            img.pixels.insert(img.pixels.end(), b & 0x3F, v);
        } else {
            img.pixels.push_back(b);
        }
    }
    if (img.pixels.size() < total) return std::nullopt;
    img.pixels.resize(total);  // a final run may overshoot, as in the game

    img.vga_pal.assign(file.end() - 0x300, file.end());
    for (auto& c : img.vga_pal) c >>= 2;
    return img;
}

void blit_pcx(const PcxImage& img, Framebuffer& fb, int x, int y, bool set_palette) {
    for (int row = 0; row < img.height; ++row) {
        const int dy = y + row;
        if (dy < 0 || dy >= fb.height()) continue;
        for (int col = 0; col < img.width; ++col) {
            const int dx = x + col;
            if (dx < 0 || dx >= fb.width()) continue;
            fb.at(dx, dy) = img.pixels[size_t(row) * img.width + col];
        }
    }
    if (set_palette) fb.palette() = img.palette();
}

} // namespace scr

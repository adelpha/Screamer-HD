#include "core/testpattern.h"

#include "core/framebuffer.h"

namespace scr {

void draw_test_pattern(Framebuffer& fb, uint64_t frame) {
    // Palette: 0..63 grey ramp, 64..127 red, 128..191 green, 192..255 blue (6-bit VGA style).
    Palette& pal = fb.palette();
    for (int i = 0; i < 64; ++i) {
        const uint8_t v = vga6_to_8(static_cast<uint8_t>(i));
        pal[i] = {v, v, v};
        pal[64 + i] = {v, 0, 0};
        pal[128 + i] = {0, v, 0};
        pal[192 + i] = {0, 0, v};
    }

    const int w = fb.width(), h = fb.height();
    // Background: diagonal grey gradient.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) fb.at(x, y) = static_cast<uint8_t>(((x + y) >> 3) & 63);

    // Three colour bars.
    for (int i = 0; i < 64; ++i) {
        const int x = 32 + i * 4;
        fb.fill_rect(x, 20, 4, 16, static_cast<uint8_t>(64 + i));
        fb.fill_rect(x, 40, 4, 16, static_cast<uint8_t>(128 + i));
        fb.fill_rect(x, 60, 4, 16, static_cast<uint8_t>(192 + i));
    }

    // Moving square: position depends only on the frame number.
    const int span = w - 24;
    const int px = static_cast<int>((frame * 3) % static_cast<uint64_t>(span * 2));
    const int sx = px < span ? px : span * 2 - px;
    fb.fill_rect(sx, 120, 24, 24, 63);
    fb.fill_rect(sx + 4, 124, 16, 16, 64 + 63);

    // Border so scaling/letterbox errors are visible.
    fb.fill_rect(0, 0, w, 1, 63);
    fb.fill_rect(0, h - 1, w, 1, 63);
    fb.fill_rect(0, 0, 1, h, 63);
    fb.fill_rect(w - 1, 0, 1, h, 63);
}

} // namespace scr

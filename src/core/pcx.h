#pragma once
// PCX images as Screamer reads them (graf_load_pcx / pcx_load in STARTL.EXE): an
// 8-bit RLE image whose 768-byte palette sits at the end of the file. The game
// decodes width*height pixels as one continuous run (it ignores the per-line padding
// field) and shifts the palette right by 2 to get 6-bit VGA DAC values.

#include "core/framebuffer.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace scr {

struct PcxImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;   // width * height palette indices
    std::vector<uint8_t> vga_pal;  // 768 bytes, 6-bit DAC values (as the game uploads them)
    Palette palette() const { return palette_from_vga(vga_pal.data()); }
};

std::optional<PcxImage> decode_pcx(const std::vector<uint8_t>& file);

// Copies an image into the framebuffer at (x, y), clipped, and sets its palette.
void blit_pcx(const PcxImage& img, Framebuffer& fb, int x = 0, int y = 0, bool set_palette = true);

} // namespace scr

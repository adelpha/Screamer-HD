#pragma once
// 8-bit indexed framebuffer + 256 colour palette, mirroring the VGA mode the
// original game renders into. The frontend converts it to RGBA for display.

#include <array>
#include <cstdint>
#include <vector>

namespace scr {

struct Rgb {
    uint8_t r = 0, g = 0, b = 0;
    bool operator==(const Rgb&) const = default;
};

using Palette = std::array<Rgb, 256>;

// VGA DAC values are 6-bit (0..63). Expand to 8-bit the same way DOSBox does
// (v << 2 | v >> 4) so our output can be compared pixel-exact with its captures.
constexpr uint8_t vga6_to_8(uint8_t v) { return static_cast<uint8_t>((v << 2) | (v >> 4)); }

// Builds a palette from 768 bytes of 6-bit VGA DAC data.
Palette palette_from_vga(const uint8_t* rgb6, size_t count = 256);

class Framebuffer {
public:
    static constexpr int kDefaultWidth = 320;
    static constexpr int kDefaultHeight = 200;

    Framebuffer(int width = kDefaultWidth, int height = kDefaultHeight);

    int width() const { return width_; }
    int height() const { return height_; }

    uint8_t* pixels() { return pixels_.data(); }
    const uint8_t* pixels() const { return pixels_.data(); }
    uint8_t& at(int x, int y) { return pixels_[static_cast<size_t>(y) * width_ + x]; }
    uint8_t at(int x, int y) const { return pixels_[static_cast<size_t>(y) * width_ + x]; }

    void clear(uint8_t index = 0);
    void fill_rect(int x, int y, int w, int h, uint8_t index);

    Palette& palette() { return palette_; }
    const Palette& palette() const { return palette_; }

    // Writes width*height RGBA8888 pixels (byte order R,G,B,A) into dst; with a pool, bands
    // of rows on all its threads.
    void to_rgba(uint8_t* dst, int dst_pitch, class WorkerPool* pool = nullptr) const;

private:
    int width_, height_;
    std::vector<uint8_t> pixels_;
    Palette palette_{};
};

} // namespace scr

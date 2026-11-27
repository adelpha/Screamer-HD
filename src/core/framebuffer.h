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

    // The shape of a pixel on the screen. The original's 320x200 fills a 4:3 screen, so its
    // pixels are 5/6 as wide as they are high, and by default a picture of any size is taken
    // to be those 320 columns stretched across its width (wide = false). With wide set the
    // pixels are square whatever the picture's shape (ours, for widescreen): the 320 columns
    // take height * 4/3 of them, the 3D view shows more to the sides on a wider picture and
    // 2D graphics keep their proportions (HudSurface).
    bool wide = false;
    // Pixels per column of the original's 320 in the 3D view.
    // (not wide: the width's 320th, whatever the picture's shape, e.g. 1280x960 for RES NATIVE)
    float unit_x() const { return wide ? float(height_) / 240.0f : float(width_) / 320.0f; }

    // Layer mode (the hardware renderer): the 3D view is drawn elsewhere (on the graphics card)
    // and this picture only holds what goes over it, the HUD and the like (HudSurface). Each
    // pixel's cover byte says what shows there: kCovered, this picture's pixel; otherwise the
    // 3D view, darkened that many times (HudSurface::darken over it; each step x0.75).
    static constexpr uint8_t kCovered = 255;
    void set_layered(bool on);
    bool layered() const { return !cover_.empty(); }
    uint8_t* cover() { return cover_.empty() ? nullptr : cover_.data(); }
    const uint8_t* cover() const { return cover_.empty() ? nullptr : cover_.data(); }
    // Nothing over the 3D view (pixels 0, cover 0).
    void clear_layer();
    // How the 3D view under a layered picture is shown: dimmed (the attract mode's fade) and
    // mirrored (the MIRRO / UPDOW cheats flip the 3D picture, not the HUD).
    float scene_brightness = 1.0f;
    bool scene_flip_x = false, scene_flip_y = false;

private:
    int width_, height_;
    std::vector<uint8_t> pixels_;
    std::vector<uint8_t> cover_;
    Palette palette_{};
};

} // namespace scr

#pragma once
// Image helpers for screenshots and the comparison harness. Built on SDL3
// surfaces (SDL3 reads/writes BMP and PNG natively, no extra libraries).

#include <SDL3/SDL_surface.h>

#include <cstdint>
#include <memory>
#include <string>

namespace scr {

class Framebuffer;

struct SurfaceDeleter {
    void operator()(SDL_Surface* s) const { SDL_DestroySurface(s); }
};
using SurfacePtr = std::unique_ptr<SDL_Surface, SurfaceDeleter>;

// Loads a .png or .bmp and converts it to RGBA32. Returns null on failure (see SDL_GetError()).
SurfacePtr load_image_rgba(const std::string& path);

// Saves as PNG or BMP depending on the file extension (defaults to PNG).
bool save_image(SDL_Surface* surface, const std::string& path);

// Converts an indexed framebuffer to a new RGBA32 surface.
SurfacePtr framebuffer_to_surface(const Framebuffer& fb);

// Nearest-neighbour rescale to an RGBA32 surface of the given size.
SurfacePtr scale_nearest(SDL_Surface* src, int w, int h);

struct DiffResult {
    bool size_mismatch = false;
    int width = 0, height = 0;
    uint64_t differing_pixels = 0;  // pixels where any channel differs by more than the tolerance
    int max_channel_delta = 0;      // largest per-channel difference seen anywhere
    double mean_abs_error = 0.0;    // mean per-channel absolute difference over RGB

    uint64_t total_pixels() const { return static_cast<uint64_t>(width) * height; }
    double differing_fraction() const {
        return total_pixels() ? static_cast<double>(differing_pixels) / static_cast<double>(total_pixels()) : 0.0;
    }
};

// Compares two RGBA32 surfaces. If diff_out is non-null it receives a
// visualisation: matching pixels are dimmed greyscale, differing pixels red.
DiffResult compare_images(SDL_Surface* a, SDL_Surface* b, int tolerance, SurfacePtr* diff_out = nullptr);

} // namespace scr

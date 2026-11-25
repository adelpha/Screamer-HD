#include "../debug_report.h"
// imgcmp: compare two images (PNG/BMP) for the test harness.
//
//   imgcmp expected.png actual.png [--tolerance N] [--max-diff-fraction F]
//          [--diff out.png] [--resize-actual WxH | --resize-to-expected] [--crop X,Y,W,H]
//
// Exit codes: 0 = match, 1 = images differ, 2 = usage / IO error.
// Prints a one-line machine-readable summary to stdout.

#include "core/image.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

scr::SurfacePtr crop(SDL_Surface* s, int x, int y, int w, int h) {
    scr::SurfacePtr out(SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32));
    SDL_Rect r{x, y, w, h};
    if (!out || !SDL_BlitSurface(s, &r, out.get(), nullptr)) return nullptr;
    return out;
}

} // namespace

int main(int argc, char** argv) {
    install_debug_report(true);
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: imgcmp expected actual [--tolerance N] [--max-diff-fraction F] [--diff out.png]\n"
                     "              [--resize-actual WxH | --resize-to-expected] [--crop X,Y,W,H]\n");
        return 2;
    }
    const std::string expected_path = argv[1], actual_path = argv[2];
    int tolerance = 0;
    double max_fraction = 0.0;
    std::string diff_path;
    int rw = 0, rh = 0;
    bool resize_to_expected = false;
    int cx = 0, cy = 0, cw = 0, ch = 0;

    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (a == "--tolerance" && v) { tolerance = std::atoi(v); ++i; }
        else if (a == "--max-diff-fraction" && v) { max_fraction = std::atof(v); ++i; }
        else if (a == "--diff" && v) { diff_path = v; ++i; }
        else if (a == "--resize-actual" && v) { std::sscanf(v, "%dx%d", &rw, &rh); ++i; }
        else if (a == "--resize-to-expected") resize_to_expected = true;
        else if (a == "--crop" && v) { std::sscanf(v, "%d,%d,%d,%d", &cx, &cy, &cw, &ch); ++i; }
        else { std::fprintf(stderr, "imgcmp: bad argument %s\n", a.c_str()); return 2; }
    }

    scr::SurfacePtr expected = scr::load_image_rgba(expected_path);
    if (!expected) { std::fprintf(stderr, "imgcmp: cannot load %s: %s\n", expected_path.c_str(), SDL_GetError()); return 2; }
    scr::SurfacePtr actual = scr::load_image_rgba(actual_path);
    if (!actual) { std::fprintf(stderr, "imgcmp: cannot load %s: %s\n", actual_path.c_str(), SDL_GetError()); return 2; }

    if (cw > 0 && ch > 0) {
        expected = crop(expected.get(), cx, cy, cw, ch);
        actual = crop(actual.get(), cx, cy, cw, ch);
    }
    if (resize_to_expected) { rw = expected->w; rh = expected->h; }
    if (rw > 0 && rh > 0) actual = scr::scale_nearest(actual.get(), rw, rh);

    scr::SurfacePtr diff;
    const scr::DiffResult r = scr::compare_images(expected.get(), actual.get(), tolerance, diff_path.empty() ? nullptr : &diff);
    if (r.size_mismatch) {
        std::printf("RESULT size_mismatch expected=%dx%d actual=%dx%d\n", expected->w, expected->h, actual ? actual->w : 0,
                    actual ? actual->h : 0);
        return 1;
    }
    const bool pass = r.differing_fraction() <= max_fraction && (max_fraction > 0.0 || r.differing_pixels == 0);
    std::printf("RESULT %s size=%dx%d differing=%llu (%.4f%%) max_delta=%d mae=%.4f\n", pass ? "match" : "differ", r.width,
                r.height, static_cast<unsigned long long>(r.differing_pixels), r.differing_fraction() * 100.0,
                r.max_channel_delta, r.mean_abs_error);
    if (diff && !diff_path.empty() && !scr::save_image(diff.get(), diff_path)) {
        std::fprintf(stderr, "imgcmp: cannot write %s: %s\n", diff_path.c_str(), SDL_GetError());
    }
    return pass ? 0 : 1;
}

#pragma once
// A small frame profiler (--perf): time spent in each part of the main loop, averaged over
// the frames since the last report. Main thread only.

#include <chrono>
#include <cstdint>
#include <string>

namespace scr::prof {

enum Section : int {
    Events,       // SDL events
    Step,         // one game step: input, physics, CPU cars, sound, camera
    Tween,        // copying and blending the state drawn between steps
    Geometry,     // 3D scene: transforming, clipping, projecting (renderer begin + draw_*)
    Fill,         // 3D scene: filling the pixels (renderer finish, all threads)
    Hud,          // HUD, overlays, menus
    Upload,       // the framebuffer into the texture (palette conversion)
    Present,      // scaling to the window and presenting (includes waiting for vsync)
    Other,        // the rest of the loop
    kCount
};

void add(Section s, uint64_t ns);
void frame_done(bool stepped);
// Averages since the last report (and resets them); empty before any frame.
std::string report();

struct Scope {
    Section s;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    explicit Scope(Section sec) : s(sec) {}
    ~Scope() { add(s, uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count())); }
};

} // namespace scr::prof

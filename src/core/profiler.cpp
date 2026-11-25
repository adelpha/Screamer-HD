#include "core/profiler.h"

#include <cstdio>

namespace scr::prof {
namespace {
uint64_t g_ns[kCount]{};
uint64_t g_frames = 0, g_steps = 0;
uint64_t g_worst_frame = 0, g_frame_ns = 0;
std::chrono::steady_clock::time_point g_last_frame{}, g_since{};
const char* const kNames[kCount] = {"events", "game step", "blending", "3D geometry", "3D fill", "HUD/overlays",
                                    "texture upload", "present+vsync", "other"};
} // namespace

void add(Section s, uint64_t ns) { g_ns[s] += ns; }

void frame_done(bool stepped) {
    const auto now = std::chrono::steady_clock::now();
    if (g_frames == 0 && g_steps == 0) g_since = now;
    if (g_last_frame.time_since_epoch().count() != 0) {
        const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - g_last_frame).count());
        g_frame_ns += ns;
        if (ns > g_worst_frame) g_worst_frame = ns;
    }
    g_last_frame = now;
    ++g_frames;
    if (stepped) ++g_steps;
}

std::string report() {
    if (g_frames == 0) return {};
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_since).count();
    char buf[256];
    std::string out;
    std::snprintf(buf, sizeof buf, "perf: %.1f frames/s (%.1f game steps/s), frame %.2f ms average, %.2f ms worst\n",
                  double(g_frames) / secs, double(g_steps) / secs, double(g_frame_ns) / 1e6 / double(g_frames),
                  double(g_worst_frame) / 1e6);
    out += buf;
    uint64_t total = 0;
    g_ns[Other] = 0;
    for (uint64_t v : g_ns) total += v;
    g_ns[Other] = g_frame_ns > total ? g_frame_ns - total : 0;  // the rest of the loop
    total += g_ns[Other];
    for (int i = 0; i < kCount; ++i) {
        std::snprintf(buf, sizeof buf, "  %-15s %7.3f ms/frame %5.1f%%\n", kNames[i], double(g_ns[i]) / 1e6 / double(g_frames),
                      total ? 100.0 * double(g_ns[i]) / double(total) : 0.0);
        out += buf;
    }
    for (uint64_t& v : g_ns) v = 0;
    g_frames = g_steps = g_worst_frame = g_frame_ns = 0;
    return out;
}

} // namespace scr::prof

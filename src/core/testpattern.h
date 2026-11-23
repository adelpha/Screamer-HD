#pragma once
// Deterministic placeholder scene drawn until real game rendering exists.
// It exercises the whole pipeline (indexed framebuffer -> palette -> SDL
// texture -> window/screenshot) so the harness has something stable to test.

#include <cstdint>

namespace scr {

class Framebuffer;

// Draws frame number `frame` of the test pattern. Pure function of `frame`.
void draw_test_pattern(Framebuffer& fb, uint64_t frame);

} // namespace scr

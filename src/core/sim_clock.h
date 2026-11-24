#pragma once
// The original game's frame clock (frame_timing_update in STARTL.EXE).
//
// A 100 Hz timer interrupt only counts ticks. Each frame waits until at least 2
// ticks have passed (so at most 50 frames per second), then:
//   raw   = ticks * 0.01 s
//   time  = (time * 9 + raw) / 10, clamped to 0.02 .. 0.2 s     (float, like the game)
//   row   = trunc(1 / time)  -> row of the pow() damping table for this frame
// Physics and replays depend on these exact step sizes, so the port reproduces them.
//
// Frame-rate independent damping uses a table built at startup (build_pow_table):
//   table[row][k] = pow(0.5 + 0.005 * k, 50 / (row + 1))     row 0..49, k 0..99
// i.e. the per-frame factor for "multiply by b every 1/50 s" at (row + 1) fps.
// Quirks of the original, kept on purpose:
//  * the row used is trunc(1 / frame_time) = fps, not fps - 1, so the factor is for
//    one frame per second faster than the real rate;
//  * when frame_time settles at exactly 0.02 the row is 50, one past the end of the
//    50-row table (the game then reads unrelated globals). In practice the smoothing
//    usually stays a few ulps above 0.02 and lands on row 49. pow_table() clamps to 49.

#include <cstdint>

namespace scr {

// One entry of the damping table (row clamped to 0..49, k to 0..99).
float pow_table(int row, int k);

class SimClock {
public:
    static constexpr int kTickHz = 100;
    static constexpr int kMinTicksPerFrame = 2;
    static constexpr uint64_t kTickNs = 1'000'000'000ull / kTickHz;

    // Adds elapsed ticks (from the real-time clock, or a fixed 2 per frame when headless).
    void add_ticks(uint32_t ticks) { ticks_ += ticks; }
    // True once enough ticks have accumulated for the next frame.
    bool frame_ready() const { return ticks_ >= kMinTicksPerFrame; }
    // Consumes all accumulated ticks and updates the frame time, like the game does.
    void begin_frame();

    // Resets the smoothed time the way race_load does (g_frame_time = 0).
    void reset() { frame_time_ = 0.0f; raw_ = 0.0f; ticks_ = 0; }

    float frame_time() const { return frame_time_; }      // g_frame_time
    // Texture animation counter (0..65535, one turn every 1 / 0.3 s at any frame rate): animated
    // track materials show frame frames * (counter & 0x3fff) >> 14 (cell_begin, mat*_draw).
    int32_t anim_counter() const { return anim_counter_; }
    float frame_time_raw() const { return raw_; }         // g_frame_time_raw
    int pow_table_row() const { return pow_row_; }        // index into the per-fps damping table
    uint32_t pending_ticks() const { return ticks_; }

private:
    uint32_t ticks_ = 0;
    float frame_time_ = 0.0f;
    float anim_phase_ = 0.0f;
    int32_t anim_counter_ = 0;
    float raw_ = 0.0f;
    int pow_row_ = 0;
};

} // namespace scr

#include "testing.h"

#include "core/sim_clock.h"

#include <cstring>

namespace {
uint32_t bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
} // namespace

TEST_CASE("sim_clock: two ticks per frame settles at 0.02 s") {
    scr::SimClock c;
    for (int i = 0; i < 100; ++i) {
        c.add_ticks(2);
        CHECK(c.frame_ready());
        c.begin_frame();
    }
    CHECK(c.frame_time() == 0.02f);
    CHECK(c.frame_time_raw() == static_cast<float>(0.02));
    CHECK(c.pending_ticks() == 0);
}

TEST_CASE("sim_clock: one tick is not a frame") {
    scr::SimClock c;
    c.add_ticks(1);
    CHECK(!c.frame_ready());
}

TEST_CASE("sim_clock: slow frames are smoothed and clamped") {
    scr::SimClock c;
    c.add_ticks(50);  // a 0.5 s hitch
    c.begin_frame();
    CHECK(c.frame_time() == static_cast<float>((0.0 * 9.0 + 0.5) / 10.0));
    for (int i = 0; i < 20; ++i) {
        c.add_ticks(50);
        c.begin_frame();
    }
    CHECK(c.frame_time() == 0.2f);
    CHECK(c.pow_table_row() == 4);  // 1 / 0.2f = 4.99999993, truncated like the game
}

TEST_CASE("sim_clock: approaching 0.02 from above sticks a few ulps above (row 49)") {
    // As observed in the running original: frame_time 0.020000007 after slower frames.
    scr::SimClock c;
    for (int i = 0; i < 30; ++i) {  // e.g. loading: 0.1 s frames
        c.add_ticks(10);
        c.begin_frame();
    }
    for (int i = 0; i < 500; ++i) {
        c.add_ticks(2);
        c.begin_frame();
    }
    CHECK(c.frame_time() > 0.02f);
    CHECK(static_cast<double>(c.frame_time()) == 0.02000000700354576);  // the value seen in the running game
    CHECK(c.pow_table_row() == 49);
}

TEST_CASE("sim_clock: damping table matches the original bit for bit") {
    // Values read from the running game's table (build_pow_table output).
    CHECK(bits(scr::pow_table(0, 95)) == 0x3e9060adu);
    CHECK(bits(scr::pow_table(25, 79)) == 0x3f4ed1d9u);
    CHECK(bits(scr::pow_table(48, 50)) == 0x3f3ee045u);
    CHECK(bits(scr::pow_table(49, 0)) == 0x3f000000u);
    CHECK(bits(scr::pow_table(12, 99)) == 0x3f7b1ca2u);
    CHECK(scr::pow_table(50, 0) == scr::pow_table(49, 0));  // clamped
}

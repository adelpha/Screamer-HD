#include "testing.h"

#include "core/framebuffer.h"
#include "core/testpattern.h"

#include <algorithm>
#include <vector>

TEST_CASE("framebuffer: vga 6-bit to 8-bit expansion matches DOSBox") {
    CHECK(scr::vga6_to_8(0) == 0);
    CHECK(scr::vga6_to_8(63) == 255);
    CHECK(scr::vga6_to_8(32) == 130);
}

TEST_CASE("framebuffer: palette_from_vga") {
    std::vector<uint8_t> raw(768, 0);
    raw[3] = 63; raw[4] = 0; raw[5] = 32;
    const scr::Palette p = scr::palette_from_vga(raw.data());
    CHECK((p[0] == scr::Rgb{0, 0, 0}));
    CHECK((p[1] == scr::Rgb{255, 0, 130}));
}

TEST_CASE("framebuffer: fill_rect clips to bounds") {
    scr::Framebuffer fb(8, 4);
    fb.fill_rect(-2, -2, 4, 4, 7);
    CHECK(fb.at(0, 0) == 7);
    CHECK(fb.at(1, 1) == 7);
    CHECK(fb.at(2, 2) == 0);
    fb.fill_rect(6, 3, 10, 10, 9);
    CHECK(fb.at(7, 3) == 9);
    CHECK(fb.at(5, 3) == 0);
}

TEST_CASE("framebuffer: to_rgba uses palette and opaque alpha") {
    scr::Framebuffer fb(2, 1);
    fb.palette()[5] = {10, 20, 30};
    fb.at(1, 0) = 5;
    uint8_t out[8] = {};
    fb.to_rgba(out, 8);
    CHECK(out[3] == 255);
    CHECK(out[4] == 10);
    CHECK(out[5] == 20);
    CHECK(out[6] == 30);
    CHECK(out[7] == 255);
}

TEST_CASE("testpattern: deterministic per frame") {
    scr::Framebuffer a, b;
    scr::draw_test_pattern(a, 42);
    scr::draw_test_pattern(b, 42);
    CHECK(std::equal(a.pixels(), a.pixels() + 320 * 200, b.pixels()));
    scr::draw_test_pattern(b, 43);
    CHECK(!std::equal(a.pixels(), a.pixels() + 320 * 200, b.pixels()));
}

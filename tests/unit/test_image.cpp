#include "testing.h"

#include "core/framebuffer.h"
#include "core/image.h"
#include "core/testpattern.h"

#include <filesystem>

namespace {
std::string temp_path(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}
} // namespace

TEST_CASE("image: identical images compare equal") {
    scr::Framebuffer fb;
    scr::draw_test_pattern(fb, 0);
    auto a = scr::framebuffer_to_surface(fb);
    auto b = scr::framebuffer_to_surface(fb);
    REQUIRE(a && b);
    const auto r = scr::compare_images(a.get(), b.get(), 0);
    CHECK(!r.size_mismatch);
    CHECK(r.differing_pixels == 0);
    CHECK(r.max_channel_delta == 0);
}

TEST_CASE("image: differences are counted and respect tolerance") {
    scr::Framebuffer fb(4, 4);
    fb.palette()[1] = {100, 100, 100};
    fb.palette()[2] = {103, 100, 100};
    fb.palette()[3] = {200, 100, 100};
    fb.clear(1);
    auto a = scr::framebuffer_to_surface(fb);
    fb.at(0, 0) = 2;  // small delta
    fb.at(1, 0) = 3;  // big delta
    auto b = scr::framebuffer_to_surface(fb);

    scr::SurfacePtr diff;
    auto r = scr::compare_images(a.get(), b.get(), 0, &diff);
    CHECK(r.differing_pixels == 2);
    CHECK(r.max_channel_delta == 100);
    REQUIRE(diff);
    CHECK(static_cast<uint8_t*>(diff->pixels)[0] == 255);  // red marker

    r = scr::compare_images(a.get(), b.get(), 5);
    CHECK(r.differing_pixels == 1);
}

TEST_CASE("image: size mismatch detected") {
    scr::Framebuffer small(4, 4), big(8, 4);
    auto a = scr::framebuffer_to_surface(small);
    auto b = scr::framebuffer_to_surface(big);
    CHECK(scr::compare_images(a.get(), b.get(), 0).size_mismatch);
}

TEST_CASE("image: png and bmp round trip losslessly") {
    scr::Framebuffer fb;
    scr::draw_test_pattern(fb, 7);
    auto src = scr::framebuffer_to_surface(fb);
    for (const char* name : {"scr_roundtrip.png", "scr_roundtrip.bmp"}) {
        const std::string p = temp_path(name);
        REQUIRE(scr::save_image(src.get(), p));
        auto back = scr::load_image_rgba(p);
        REQUIRE(back);
        CHECK(scr::compare_images(src.get(), back.get(), 0).differing_pixels == 0);
        std::filesystem::remove(p);
    }
}

TEST_CASE("image: nearest scale by 2 preserves pixels") {
    scr::Framebuffer fb;
    scr::draw_test_pattern(fb, 3);
    auto src = scr::framebuffer_to_surface(fb);
    auto big = scr::scale_nearest(src.get(), 640, 400);
    REQUIRE(big);
    auto back = scr::scale_nearest(big.get(), 320, 200);
    REQUIRE(back);
    CHECK(scr::compare_images(src.get(), back.get(), 0).differing_pixels == 0);
}

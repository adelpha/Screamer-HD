#include "testing.h"

#include "core/archive.h"
#include "core/gamedata.h"
#include "core/pcx.h"

TEST_CASE("pcx: RLE runs, literals and the 6-bit palette") {
    std::vector<uint8_t> f(0x80, 0);
    f[8] = 3;  // xmax = 3 -> width 4
    f[10] = 1; // ymax = 1 -> height 2
    const uint8_t rle[] = {0xC3, 7, 9, 0xC2, 1, 0x05, 0x06};  // 7 7 7 | 9 | 1 1 | 5 6
    f.insert(f.end(), std::begin(rle), std::end(rle));
    for (int i = 0; i < 768; ++i) f.push_back(uint8_t(i & 0xFF));
    auto img = scr::decode_pcx(f);
    REQUIRE(img.has_value());
    CHECK(img->width == 4);
    CHECK(img->height == 2);
    const std::vector<uint8_t> want = {7, 7, 7, 9, 1, 1, 5, 6};
    CHECK(img->pixels == want);
    CHECK(img->vga_pal[3] == (3 >> 2));
    CHECK(img->vga_pal[255] == (255 >> 2));
}

TEST_CASE("pcx: the title screen decodes from GRAF.DAT") {
    auto dir = scr::find_game_dir();
    if (!dir) SKIP("original game data not present");
    auto path = scr::resolve_game_file(*dir, "GFX/GRAF.DAT");
    REQUIRE(path.has_value());
    auto a = scr::Archive::open(*path);
    REQUIRE(a.has_value());
    auto file = a->read("menu1l.pcx");
    REQUIRE(file.has_value());
    auto img = scr::decode_pcx(*file);
    REQUIRE(img.has_value());
    CHECK(img->width == 320);
    CHECK(img->height == 200);
    for (uint8_t c : img->vga_pal) CHECK(c < 64);
    // FNV-1a of pixels + palette. Recorded after checking the decode pixel-exact against
    // the original's VGA memory in DOSBox (tools/dosbox/capture-vga.ps1, background areas
    // not covered by the menu text).
    uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&](uint8_t c) { h = (h ^ c) * 0x100000001b3ull; };
    for (uint8_t c : img->pixels) mix(c);
    for (uint8_t c : img->vga_pal) mix(c);
    CHECK(h == 0x84286da1a63cde1dull);
}

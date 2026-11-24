#include "testing.h"

#include "core/framebuffer.h"
#include "core/gamedata.h"
#include "core/hud.h"

#include <array>

TEST_CASE("hud: fonts, sprites and overlays from GAME.DAT") {
    auto dir = scr::find_game_dir();
    if (!dir) SKIP("original game data not present");
    auto path = scr::resolve_game_file(*dir, "GFX/GAME.DAT");
    REQUIRE(path.has_value());
    scr::Hud hud;
    REQUIRE(hud.load(*path));
    REQUIRE(hud.fonts.fonts.size() == 7);
    CHECK(hud.fonts.fonts[1].height == 27);  // lap / position digits
    CHECK(hud.fonts.fonts[1].first == '0');
    REQUIRE(hud.sprites.sprites.size() == 63);
    CHECK(hud.sprites.sprites[0].w == 96);   // start lights
    CHECK(hud.sprites.sprites[0].h == 31);
    CHECK(!hud.labels.empty());
    CHECK(!hud.gauges.empty());

    // Drawn at 320x200 and 640x400: the same picture, doubled.
    std::array<uint8_t, 256> identity{};
    for (int i = 0; i < 256; ++i) identity[size_t(i)] = uint8_t(i);
    scr::HudState st;
    st.time_left = 15;
    st.position = 10;
    st.countdown_stage = 0;
    scr::Framebuffer small(320, 200), big(640, 400);
    scr::Hud h2 = hud;
    hud.draw(small, st, identity, 0.02f, 49);
    h2.draw(big, st, identity, 0.02f, 49);
    int drawn = 0, same = 0;
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 320; ++x) {
            if (small.at(x, y) != 0) ++drawn;
            if (small.at(x, y) == big.at(2 * x, 2 * y)) ++same;
        }
    CHECK(drawn > 3000);
    CHECK(same > 320 * 200 - 400);  // lines are drawn per pixel at both sizes
}

#include "testing.h"

#include "core/framebuffer.h"
#include "core/gamedata.h"
#include "core/car.h"
#include "core/hud.h"

#include <array>
#include <vector>

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

TEST_CASE("hud: STARTH.EXE's 640x400 set lines up with the 320x200 one") {
    auto dir = scr::find_game_dir();
    if (!dir) SKIP("original game data not present");
    auto path = scr::resolve_game_file(*dir, "GFX/GAME.DAT");
    REQUIRE(path.has_value());
    scr::Hud lo, hi;
    REQUIRE(lo.load(*path));
    REQUIRE(hi.load(*path, true));
    CHECK(lo.scale == 1);
    CHECK(hi.scale == 2);
    REQUIRE(hi.fonts.fonts.size() == lo.fonts.fonts.size());
    REQUIRE(hi.sprites.sprites.size() == lo.sprites.sprites.size());
    CHECK(hi.fonts.fonts[1].height == 2 * lo.fonts.fonts[1].height);
    CHECK(hi.sprites.sprites[0].w == 2 * lo.sprites.sprites[0].w);  // start lights
    // the dashboard: COCK00AH and the COCK00H layout (gears at twice the places, pivots as in L)
    const std::string dtr =
        "COCK00H\n\td24\td16\n\td116\td147\td0\n\td201\td147\td0\n"
        "\td167\td368\td0\n\td213\td368\td0\n\td259\td368\td0\n\td305\td368\td0\n"
        "\td351\td368\td0\n\td397\td368\td0\n\td443\td368\td0\nEND_LIST\n";
    REQUIRE(hi.load_cockpit(*path, "00A", dtr, "COCK00"));
    CHECK(hi.layout.gear[0][1] == 368);
    // Drawn on 640x400: the labels, digits and lights land where the 320x200 ones do, doubled.
    std::array<uint8_t, 256> identity{};
    for (int i = 0; i < 256; ++i) identity[size_t(i)] = uint8_t(i);
    scr::HudState st;
    st.time_left = 15;
    st.position = 10;
    st.countdown_stage = 0;
    scr::Framebuffer small(320, 200), big(640, 400);
    lo.draw(small, st, identity, 0.02f, 49);
    hi.draw(big, st, identity, 0.02f, 49);
    int both = 0, drawn = 0;
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 320; ++x) {
            const bool a = small.at(x, y) != 0;
            // (the 640x400 glyphs are redrawn, not doubled: anything drawn close by counts)
            bool b = false;
            for (int dy = -2; dy <= 3 && !b; ++dy)
                for (int dx = -2; dx <= 3 && !b; ++dx) {
                    const int bx = 2 * x + dx, by = 2 * y + dy;
                    b = bx >= 0 && by >= 0 && bx < 640 && by < 400 && big.at(bx, by) != 0;
                }
            drawn += a;
            both += a && b;
        }
    CHECK(drawn > 3000);
    CHECK(both > drawn * 9 / 10);
}

TEST_CASE("hud: GAME.TXT strings for the end of the race") {
    auto dir = scr::find_game_dir();
    if (!dir) SKIP("original game data not present");
    auto path = scr::resolve_game_file(*dir, "DATA/LOCAL.DAT");
    REQUIRE(path.has_value());
    scr::Hud hud;
    REQUIRE(hud.load_texts(*path));
    REQUIRE(hud.texts.size() > 22);
    CHECK(hud.texts[0] == "POSITION");
    CHECK(hud.texts[1] == "1ST");
    CHECK(hud.texts[11] == "GREAT!");
    CHECK(hud.texts[22] == "TIME OVER");
}

TEST_CASE("views: the cockpit view and the blend between views") {
    scr::Car car;  // a car at rest at the origin is enough for the cameras
    car.setup.maxspd = 21000;
    scr::ViewCamera v;
    v.set(1);
    CHECK(v.viewport == 132);
    CHECK(v.hud_view == 1);
    v.update(car, 300, 0.02f, 49);
    CHECK(!v.car_visible);
    CHECK(v.y == 300);
    v.select(2);  // chase: full view and the gauges at once, the camera blends over
    CHECK(v.viewport == 200);
    CHECK(v.hud_view == 2);
    v.update(car, 300, 0.02f, 49);
    CHECK(v.car_visible);
    const int first_y = v.y;
    for (int i = 0; i < 200; ++i) v.update(car, 300, 0.02f, 49);
    CHECK(v.y != first_y);  // the blend moved the camera to the chase position
    CHECK(v.z > 1000);      // 1300 behind the car (facing -z at yaw 0)
}

TEST_CASE("views: bumper to cockpit switches at once while driving") {
    // camera_handle_view_keys blends from the old view's camera of the same frame, so a moving
    // car does not add its travel to the blend: the bumper and cockpit cameras coincide and
    // the dashboard appears on the next frame.
    scr::Car car;
    car.setup.maxspd = 21000;
    scr::ViewCamera v;
    v.set(0);
    v.update(car, 300, 0.02f, 49);
    car.pos_z -= 300;  // moving 300 units a frame
    v.select(1);
    int frames = 0;
    while (v.hud_view != 1 && frames < 500) {
        car.pos_z -= 300;
        v.update(car, 300, 0.02f, 49);
        ++frames;
    }
    CHECK(frames <= 2);
    CHECK(v.viewport == 132);
}

TEST_CASE("hud surface: on a wide picture the 320 columns keep their shape and their edge") {
    auto columns = [](const scr::Framebuffer& fb) {
        std::vector<int> c;
        for (int x = 0; x < fb.width(); ++x)
            if (fb.at(x, 0)) c.push_back(x);
        return c;
    };
    auto draw = [&](int w, int h, bool wide, scr::HudAnchor a, int x, bool extend = false) {
        scr::Framebuffer fb(w, h);
        fb.wide = wide;
        scr::HudSurface s(fb);
        s.set_anchor(a);
        s.set_extend(extend);
        s.pixel(x, 0, 5);
        return columns(fb);
    };
    using A = scr::HudAnchor;
    // as before on a picture that is not: stretched across it, whatever the anchor
    CHECK((draw(640, 200, false, A::Right, 1) == std::vector<int>{2, 3}));
    CHECK(draw(640, 200, false, A::Centre, -1).empty());
    // 427x240 with square pixels: one pixel a column, 107 columns to spare
    CHECK(draw(427, 240, true, A::Left, 0) == std::vector<int>{0});
    CHECK(draw(427, 240, true, A::Right, 319) == std::vector<int>{426});
    CHECK(draw(427, 240, true, A::Centre, 0) == std::vector<int>{53});
    CHECK(draw(427, 240, true, A::Sides, 10) == std::vector<int>{10});
    CHECK(draw(427, 240, true, A::Sides, 160) == std::vector<int>{213});
    CHECK(draw(427, 240, true, A::Sides, 310) == std::vector<int>{417});
    // past the 320 where there is picture left, no further
    CHECK(draw(427, 240, true, A::Centre, -53) == std::vector<int>{0});
    CHECK(draw(427, 240, true, A::Centre, -54).empty());
    CHECK(draw(427, 240, true, A::Left, 426) == std::vector<int>{426});
    CHECK(draw(427, 240, true, A::Right, 320).empty());
    // a whole-screen graphic's outermost columns carried on to the edges
    CHECK(draw(427, 240, true, A::Centre, 0, true).size() == 54);
    CHECK(draw(427, 240, true, A::Centre, 319, true).size() == 55);  // (372 to 426)
    // a picture no wider than 4:3 has nothing to spare
    CHECK(draw(320, 240, true, A::Right, 5) == std::vector<int>{5});

    scr::Framebuffer fb(427, 240);
    fb.wide = true;
    fb.clear(1);
    std::array<uint8_t, 256> t{};
    t[1] = 2;
    scr::HudSurface s(fb);
    s.darken_all(t);
    CHECK(fb.at(0, 0) == 2);
    CHECK(fb.at(426, 239) == 2);
}

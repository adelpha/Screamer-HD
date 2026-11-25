#include "testing.h"

#include "core/framebuffer.h"
#include "core/gamedata.h"
#include "core/car.h"
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

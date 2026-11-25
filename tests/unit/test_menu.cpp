#include "testing.h"

#include "core/gamedata.h"
#include "core/menu.h"

#include <cstdio>
#include <optional>
#include <string>

namespace {

std::optional<scr::MenuAssets> load_menu() {
    auto dir = scr::find_game_dir();
    if (!dir) return std::nullopt;
    auto md = scr::resolve_game_file(*dir, "GFX/MENU.DAT");
    auto gd = scr::resolve_game_file(*dir, "GFX/GRAF.DAT");
    auto ld = scr::resolve_game_file(*dir, "DATA/LOCAL.DAT");
    if (!md || !gd || !ld) return std::nullopt;
    scr::MenuAssets a;
    if (!a.load(*md, *gd, *ld)) return std::nullopt;
    return a;
}

void run(scr::Menu& m, int frames, scr::MenuKeys k = {}) {
    for (int i = 0; i < frames; ++i) {
        m.update(i == 0 ? k : scr::MenuKeys{}, 0.02f);
    }
}

} // namespace

TEST_CASE("menu: assets from MENU.DAT, GRAF.DAT and LOCAL.DAT") {
    if (!scr::find_game_dir()) SKIP("original game data not present");
    auto a = load_menu();
    REQUIRE(a.has_value());
    CHECK(a->cars.size() == 7);
    CHECK(a->maps.size() == 6);
    REQUIRE(a->texts.size() > 46);
    CHECK(a->texts[0] == "NORMAL GAME");
    CHECK(a->texts[46] == "ENTER YOUR NAME");
    REQUIRE(a->car_info.size() == 14);
    CHECK(a->car_info[0].name == "SHADOW");
    CHECK(a->car_info[0].max_speed == 340);
    REQUIRE(a->track_names.size() == 12);
    CHECK(a->track_names[0] == "PALM TOWN");
    const scr::PcxImage* bg = a->backdrop("MENU1");
    REQUIRE(bg != nullptr);
    CHECK(bg->width == 320);
}

TEST_CASE("menu: main page letters, options and the normal game flow") {
    if (!scr::find_game_dir()) SKIP("original game data not present");
    auto a = load_menu();
    REQUIRE(a.has_value());
    scr::Menu m(*a);
    m.show_title();
    run(m, 100);
    // "NORMAL GAME" ... "OPTIONS": one block per letter, all flown in (k = 0)
    int letters = 0;
    for (const auto& it : m.items)
        if (it.ch != -1) {
            ++letters;
            CHECK(it.k == 0.0f);
        }
    CHECK(letters == 55);  // spaces are blocks too (an empty frame)
    CHECK(m.brightness == 1.0f);
    // Options: HI DETAIL cycles to LOW DETAIL
    scr::MenuKeys down;
    down.down = true;
    for (int i = 0; i < 4; ++i) {
        run(m, 1, down);
        run(m, 1);
    }
    CHECK(m.selected[0] == 4);
    scr::MenuKeys enter;
    enter.enter = true;
    run(m, 1, enter);
    CHECK(m.page == 1);
    run(m, 60);
    run(m, 1, enter);
    run(m, 30);
    CHECK(m.pages[1][0].value == 0);  // LOW DETAIL
    // Back to the main page and into a normal game: name, track, car, race
    scr::MenuKeys esc;
    esc.esc = true;
    run(m, 1, esc);
    CHECK(m.page == 0);
    scr::MenuKeys up;
    up.up = true;
    for (int i = 0; i < 4; ++i) {
        run(m, 1, up);
        run(m, 1);
    }
    run(m, 1, enter);
    run(m, 100);
    CHECK(m.mode == 4);
    scr::MenuKeys right;
    right.right = true;
    run(m, 1, right);
    run(m, 20);
    CHECK(m.name[0] == 'B');
    run(m, 1, enter);
    run(m, 100);
    CHECK(m.mode == 3);
    run(m, 1, right);
    run(m, 60);
    CHECK(m.track == 1);
    run(m, 1, enter);
    run(m, 100);
    CHECK(m.mode == 2);
    run(m, 1, enter);
    REQUIRE(m.race.has_value());
    CHECK(m.race->map == 2);
    CHECK(m.race->car == 0);
    CHECK(m.race->laps == 3);
    CHECK(!m.race->high_detail);
    CHECK(m.race->name == "BAA");
}

TEST_CASE("menu: championship steps, standings and qualifying") {
    if (!scr::find_game_dir()) SKIP("original game data not present");
    auto a = load_menu();
    REQUIRE(a.has_value());
    REQUIRE(a->champ_text.size() >= 20);
    CHECK(a->champ_text[10].size() == 2);  // WELCOME TO / ROOKIE LEAGUE
    scr::Menu m(*a);
    m.show_title();
    run(m, 100);
    scr::MenuKeys down, enter;
    down.down = true;
    enter.enter = true;
    run(m, 1, down);
    run(m, 1);
    run(m, 1, enter);  // CHAMPIONSHIP
    run(m, 60);
    CHECK(m.page == 5);
    run(m, 1, enter);  // NEW
    run(m, 100);
    CHECK(m.mode == 4);
    run(m, 1, enter);  // name
    run(m, 100);
    CHECK(m.mode == 5);
    CHECK(m.champ_step == 0);
    run(m, 1, enter);  // the welcome text -> car selection
    run(m, 100);
    CHECK(m.mode == 2);
    run(m, 1, enter);
    REQUIRE(m.race.has_value());
    CHECK(m.race->map == 1);
    CHECK(m.race->laps == 3);
    CHECK(m.race->level == 0);
    m.race.reset();
    run(m, 1);
    // won the first race: the standings, then the next race (MAP2)
    m.after_race(true, false, 1, 120.0f);
    CHECK(m.mode == 5);
    CHECK(m.champ_step == 1);
    CHECK(m.champ_points[0] == 9);
    scr::Framebuffer fb(320, 200);
    scr::Renderer r;
    m.draw(fb, r);
    run(m, 1, enter);
    run(m, 100);
    CHECK(m.champ_step == 2);
    // fourth place: out of the championship
    m.champ_step = 2;  // the second race
    m.after_race(true, false, 4, 130.0f);
    run(m, 1, enter);
    run(m, 100);
    CHECK(m.champ_step == 52);  // LOSER!!
}

TEST_CASE("menu: cheats typed in the menus") {
    if (!scr::find_game_dir()) SKIP("original game data not present");
    auto a = load_menu();
    REQUIRE(a.has_value());
    scr::Menu m(*a);
    m.show_title();
    auto type = [&](const char* w) {
        bool took = false;
        for (const char* c = w; *c; ++c) took = m.type_letter(*c);
        return took;
    };
    CHECK(!type("HELLO"));
    CHECK(m.race_options == 0);
    CHECK(type("MIRRO"));
    CHECK(m.race_options == 0x10);
    CHECK(type("xxUPDOW"));  // stray letters start the input again
    CHECK(m.race_options == 0x30);
    CHECK(type("MONTY"));    // sets the whole word: the mirror and upside down go
    CHECK(m.race_options == 1);
    CHECK(type("CLOCK"));
    CHECK(m.race_options == 5);
    CHECK(type("INVER"));
    CHECK(m.tracks_unlocked == 9);
    CHECK(type("TAZOR"));
    CHECK(m.cars_unlocked == 1);
}

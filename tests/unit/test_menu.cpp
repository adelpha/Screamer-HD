#include "testing.h"

#include "core/gamedata.h"
#include "core/menu.h"

#include <array>
#include <cstdio>
#include <filesystem>
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

TEST_CASE("menu: CHOICE.DAT keeps our volumes after the original's bytes") {
    const auto path = std::filesystem::temp_directory_path() / "scr_choice_test.dat";
    scr::ChoiceFile c;
    CHECK((c.volume == std::array<int32_t, 4>{10, 10, 10, 10}));
    c.volume = {3, 0, 7, 10};
    c.music_midi = 1;
    c.set(0x14, 2);
    REQUIRE(c.save(path.string()));
    CHECK(std::filesystem::file_size(path) == 0x460 + 4 + 4 + 20);
    scr::ChoiceFile d;
    REQUIRE(d.load(path.string()));
    CHECK((d.volume == std::array<int32_t, 4>{3, 0, 7, 10}));
    CHECK(d.music_midi == 1);
    CHECK(d.get(0x14) == 2);
    // only the four volumes (as the first version wrote them): CD music
    {
        std::FILE* f = std::fopen(path.string().c_str(), "r+b");
        REQUIRE(f != nullptr);
        const int32_t four = 4;
        std::fseek(f, 0x464, SEEK_SET);
        std::fwrite(&four, 4, 1, f);
        std::fclose(f);
    }
    REQUIRE(d.load(path.string()));
    CHECK((d.volume == std::array<int32_t, 4>{3, 0, 7, 10}));
    CHECK(d.music_midi == 0);
    // the original's file (0x460 bytes, nothing after them): the defaults
    std::filesystem::resize_file(path, 0x460);
    scr::ChoiceFile e;
    e.volume = {1, 1, 1, 1};
    e.music_midi = 1;
    REQUIRE(e.load(path.string()));
    CHECK((e.volume == std::array<int32_t, 4>{10, 10, 10, 10}));
    CHECK(e.music_midi == 0);
    CHECK(e.get(0x14) == 2);
    std::filesystem::remove(path);
}

TEST_CASE("menu: OPTIONS > SOUND sets the four volumes and the music source") {
    if (!scr::find_game_dir()) SKIP("original game data not present");
    auto a = load_menu();
    REQUIRE(a.has_value());
    scr::Menu m(*a);
    scr::ChoiceFile c;
    c.volume = {10, 10, 5, 10};
    m.load_choice(c);
    m.show_title();
    run(m, 100);
    auto press = [&](auto set) {
        scr::MenuKeys k;
        set(k);
        run(m, 1, k);
        run(m, 30);
    };
    for (int i = 0; i < 4; ++i) press([](scr::MenuKeys& k) { k.down = true; });
    press([](scr::MenuKeys& k) { k.enter = true; });  // OPTIONS
    REQUIRE(m.page == 1);
    // DETAIL, DIFFICULTY, LAPS, SOUND (in place of SPEECH and MUSIC), VIEW RECORDS
    CHECK(m.pages[1].size() == 5);
    for (int i = 0; i < 3; ++i) press([](scr::MenuKeys& k) { k.down = true; });
    press([](scr::MenuKeys& k) { k.enter = true; });  // SOUND
    REQUIRE(m.page == 7);
    run(m, 60);
    // its lines: "ANNOUNCER 10" ... (one block per letter)
    int letters = 0;
    for (const auto& it : m.items) letters += it.ch != -1;
    CHECK(letters == int(std::string("ANNOUNCER 10ENGINE 10EFFECTS 5MUSIC 10CD MUSIC").size()));
    press([](scr::MenuKeys& k) { k.left = true; });   // announcer 9
    press([](scr::MenuKeys& k) { k.right = true; });  // back to 10, not past it
    press([](scr::MenuKeys& k) { k.right = true; });
    press([](scr::MenuKeys& k) { k.left = true; });
    press([](scr::MenuKeys& k) { k.down = true; });
    press([](scr::MenuKeys& k) { k.left = true; });   // engine 9
    press([](scr::MenuKeys& k) { k.down = true; });
    press([](scr::MenuKeys& k) { k.enter = true; });  // effects 6
    press([](scr::MenuKeys& k) { k.down = true; });
    press([](scr::MenuKeys& k) { k.enter = true; });  // music wraps to 0
    CHECK((m.volumes() == std::array<int, 4>{9, 9, 6, 0}));
    CHECK((m.save_choice().volume == std::array<int32_t, 4>{9, 9, 6, 0}));
    CHECK(!m.music_midi());
    press([](scr::MenuKeys& k) { k.down = true; });
    press([](scr::MenuKeys& k) { k.enter = true; });  // CD MUSIC -> MIDI MUSIC
    CHECK(m.music_midi());
    CHECK(m.save_choice().music_midi == 1);
    letters = 0;
    for (const auto& it : m.items) letters += it.ch != -1;
    CHECK(letters == int(std::string("ANNOUNCER 9ENGINE 9EFFECTS 6MUSIC 0MIDI MUSIC").size()));
    press([](scr::MenuKeys& k) { k.enter = true; });  // and back
    CHECK(!m.music_midi());
    press([](scr::MenuKeys& k) { k.esc = true; });
    CHECK(m.page == 1);
}

TEST_CASE("menu: replay numbers for new total records") {
    if (!scr::find_game_dir()) SKIP("original game data not present");
    auto a = load_menu();
    REQUIRE(a.has_value());
    scr::Menu m(*a);
    m.load_choice(scr::ChoiceFile{});
    // menu_replay_files: the lowest number no other total of the track has
    CHECK(m.race_result(1, 30.0f, 100.0f, 3, true, true) == 1);
    CHECK(m.race_result(1, 30.0f, 90.0f, 3, true, true) == 2);
    CHECK(m.race_result(1, 30.0f, 95.0f, 3, true, false) == 0);  // no replay saved
    CHECK(m.race_result(1, 30.0f, 80.0f, 5, true, true) == 0);   // only 3-lap races
    CHECK(m.race_result(1, 30.0f, 85.0f, 3, true, true) == 3);
    const scr::ChoiceFile c = m.save_choice();
    const size_t t = 0x24c;  // MAP1's totals: 85 (3), 90 (2), 95 (none), 100 (1), 900
    CHECK(c.getf(t) == 85.0f);
    CHECK(c.get(t + 0x28) == 3);
    CHECK(c.get(t + 0x28 + 4) == 2);
    CHECK(c.get(t + 0x28 + 8) == 0);
    CHECK(c.get(t + 0x28 + 12) == 1);
    CHECK(m.race_result(2, 30.0f, 85.0f, 3, true, true) == 1);  // numbers are per track
}

TEST_CASE("menu: the demo is asked for once per 30 idle seconds") {
    if (!scr::find_game_dir()) SKIP("original game data not present");
    auto a = load_menu();
    REQUIRE(a.has_value());
    scr::Menu m(*a);
    m.show_title();
    run(m, 100);
    CHECK(!m.want_demo);
    run(m, 1500);  // 30 seconds and a bit, no key
    CHECK(m.want_demo);
    // the demo ran and the same menu is shown again: it must wait another 30 seconds
    m.want_demo = false;
    run(m, 500);
    CHECK(!m.want_demo);
    run(m, 1100);
    CHECK(m.want_demo);
}

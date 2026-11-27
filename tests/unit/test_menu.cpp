#include "testing.h"

#include "core/gamedata.h"
#include "core/menu.h"

#include <algorithm>
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
    // Options > Video: HI DETAIL cycles to LOW DETAIL
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
    for (int i = 0; i < 2; ++i) {
        run(m, 1, down);
        run(m, 1);
    }
    run(m, 1, enter);
    CHECK(m.page == scr::Menu::kVideoPage);
    run(m, 60);
    for (int i = 0; i < 4; ++i) {
        run(m, 1, down);
        run(m, 1);
    }
    run(m, 1, enter);
    run(m, 30);
    CHECK(m.pages[scr::Menu::kVideoPage][scr::Menu::kDetailLine].value == 0);  // LOW DETAIL
    CHECK(m.save_choice().get(0x00c) == 0);
    // Back to the main page and into a normal game: name, track, car, race
    scr::MenuKeys esc;
    esc.esc = true;
    run(m, 1, esc);
    CHECK(m.video_apply);  // (main applies the VIDEO page when it is left)
    run(m, 1);
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
    // second in the second race: still on (only the last league asks for a win every time)
    m.after_race(true, false, 2, 125.0f);
    CHECK(m.champ_step == 3);
    CHECK(m.champ_points[1] == 7);
    run(m, 1, enter);
    run(m, 100);
    CHECK(m.champ_step == 4);
    // the last league: second place is the end of it
    m.champ_step = 39;
    m.after_race(true, false, 2, 125.0f);
    run(m, 1, enter);
    run(m, 100);
    CHECK(m.champ_step == 52);
    // fourth place: out of the championship
    m.champ_pos.fill(-1);
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
    CHECK(std::filesystem::file_size(path) == 0x460 + 4 + 4 + 14 * 4);  // 4 volumes, music, 9 VIDEO settings
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
    // DIFFICULTY, LAPS, VIDEO (with the DETAIL), SOUND (in place of SPEECH and MUSIC), VIEW RECORDS
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

TEST_CASE("menu: VIDEO resolutions follow the widescreen line") {
    const auto narrow = scr::video_resolutions(false);
    const auto wide = scr::video_resolutions(true);
    REQUIRE(!narrow.empty());
    CHECK((narrow[0] == std::pair<int, int>{0, 0}));  // NATIVE first
    for (size_t i = 1; i < narrow.size(); ++i)  // 4:3, and the original's 320x200 (and 640x400), also shown 4:3
        CHECK((narrow[i].first * 3 == narrow[i].second * 4 || narrow[i].first * 5 == narrow[i].second * 8));
    CHECK((std::find(narrow.begin(), narrow.end(), std::pair<int, int>{320, 200}) != narrow.end()));
    CHECK((std::find(wide.begin(), wide.end(), std::pair<int, int>{7680, 4320}) != wide.end()));
    CHECK((std::find(narrow.begin(), narrow.end(), std::pair<int, int>{1920, 1080}) == narrow.end()));
    CHECK(wide.size() > narrow.size());
}

TEST_CASE("menu: OPTIONS > VIDEO and its settings in CHOICE.DAT") {
    if (!scr::find_game_dir()) SKIP("original game data not present");
    auto a = load_menu();
    REQUIRE(a.has_value());
    scr::Menu m(*a);
    scr::VideoSettings v;
    v.widescreen = true;
    v.width = 2560;
    v.height = 1440;
    v.view_distance = 0;  // no limit
    v.frame_limit = 144;
    m.set_video(v);
    CHECK(m.video() == v);
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
    for (int i = 0; i < 2; ++i) press([](scr::MenuKeys& k) { k.down = true; });
    press([](scr::MenuKeys& k) { k.enter = true; });  // VIDEO
    REQUIRE(m.page == scr::Menu::kVideoPage);
    run(m, 60);
    press([](scr::MenuKeys& k) { k.enter = true; });  // FULLSCREEN ON
    CHECK(m.video().fullscreen);
    press([](scr::MenuKeys& k) { k.down = true; });
    press([](scr::MenuKeys& k) { k.right = true; });  // WIDESCREEN OFF: 2560x1440 is no 4:3 size
    CHECK(!m.video().widescreen);
    CHECK(m.video().width * 3 == m.video().height * 4);
    CHECK(m.video().height == 1440);  // the 4:3 size nearest in height (1920x1440)
    press([](scr::MenuKeys& k) { k.down = true; });
    press([](scr::MenuKeys& k) { k.left = true; });  // the next size down
    CHECK(m.video().height == 1200);
    press([](scr::MenuKeys& k) { k.down = true; });
    press([](scr::MenuKeys& k) { k.right = true; });  // DISTANCE MAX wraps to 1X
    CHECK(m.video().view_distance == 1.0f);
    // the ADVANCED page
    for (int i = 0; i < 2; ++i) press([](scr::MenuKeys& k) { k.down = true; });
    press([](scr::MenuKeys& k) { k.enter = true; });
    REQUIRE(m.page == scr::Menu::kVideoPage + 1);
    run(m, 60);
    press([](scr::MenuKeys& k) { k.enter = true; });  // VSYNC OFF
    CHECK(!m.video().vsync);
    press([](scr::MenuKeys& k) { k.down = true; });
    press([](scr::MenuKeys& k) { k.right = true; });  // FPS LIMIT 165
    CHECK(m.video().frame_limit == 165);
    press([](scr::MenuKeys& k) { k.down = true; });
    press([](scr::MenuKeys& k) { k.enter = true; });  // FAR MODELS LOW
    CHECK(m.video().lod);
    press([](scr::MenuKeys& k) { k.esc = true; });
    CHECK(m.page == scr::Menu::kVideoPage);
    CHECK(!m.video_apply);  // (only leaving VIDEO itself applies)
    press([](scr::MenuKeys& k) { k.esc = true; });
    CHECK(m.page == 1);
    CHECK(m.video_apply);

    // saved after the original's bytes, and read back
    const std::string path = (std::filesystem::temp_directory_path() / "screamer_video_choice.dat").string();
    scr::ChoiceFile c = m.save_choice();
    REQUIRE(c.save(path));
    scr::ChoiceFile d;
    REQUIRE(d.load(path));
    CHECK(d.video == m.video());
    std::filesystem::remove(path);
    scr::Menu n(*a);
    n.load_choice(d);
    CHECK(n.video() == m.video());
}

TEST_CASE("menu: NETWORK GAME says it is not supported") {
    if (!scr::find_game_dir()) SKIP("original game data not present");
    auto a = load_menu();
    REQUIRE(a.has_value());
    scr::Menu m(*a);
    m.show_title();
    run(m, 100);
    auto press = [&](auto set) {
        scr::MenuKeys k;
        set(k);
        run(m, 1, k);
        run(m, 10);
    };
    for (int i = 0; i < 3; ++i) press([](scr::MenuKeys& k) { k.down = true; });
    press([](scr::MenuKeys& k) { k.enter = true; });
    CHECK(m.notice_shown());
    CHECK(m.page == 0);
    CHECK(m.mode == 0);
    press([](scr::MenuKeys& k) { k.down = true; });  // any key closes it, and does nothing else
    CHECK(!m.notice_shown());
    CHECK(m.selected[0] == 3);
    press([](scr::MenuKeys& k) { k.enter = true; });
    run(m, 250);  // and it goes by itself
    CHECK(!m.notice_shown());
    CHECK(m.page == 0);
}

TEST_CASE("menu: lines longer than 15 letters stay centred") {
    if (!scr::find_game_dir()) SKIP("original game data not present");
    auto a = load_menu();
    REQUIRE(a.has_value());
    scr::Menu m(*a);
    scr::VideoSettings v;
    v.widescreen = true;
    v.width = 1680;
    v.height = 1050;  // "1680X1050 (16:10)", 17 letters
    m.set_video(v);
    m.page_stack = {0, 1};
    m.page = scr::Menu::kVideoPage;
    m.show_title();
    m.page = scr::Menu::kVideoPage;
    run(m, 100);
    int letters = 0, lo = 0, hi = 0;
    for (const auto& it : m.items)
        if (it.ch != -1) {
            ++letters;
            lo = std::min(lo, it.x1);
            hi = std::max(hi, it.x1);
        }
    CHECK(letters > 60);
    CHECK(lo >= -8 * 0x280);  // (17 letters 0x280 apart about the middle, x = 0)
    CHECK(hi <= 8 * 0x280);
}

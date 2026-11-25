#include "testing.h"

#include "core/gamedata.h"
#include "core/replay.h"

#include <cstdio>
#include <filesystem>
#include <string>

TEST_CASE("replay: .RPL save and load") {
    scr::Replay r;
    r.car = 5;
    r.laps = 5;
    r.level = 2;
    r.game_mode = 1;
    r.race_options = 0x10 | scr::Replay::kReversed;
    for (int i = 0; i < 100; ++i) {
        scr::ReplayFrame f;
        f.frame_time = f.raw_time = 0.02f + float(i) * 0.0001f;
        f.accel = int16_t(i * 10);
        f.steer_left = int16_t(-i);
        f.steer_right = i % 2 ? int16_t(-32768) : int16_t(0);
        f.gear_up = int16_t(i % 3 == 0);
        r.frames.push_back(f);
    }
    const std::string path = (std::filesystem::temp_directory_path() / "scr_test_replay.rpl").string();
    REQUIRE(r.save(path));
    // 6 x 0x1c + 0x28 x 0x1c + 40 bytes before the header, 0x18 header, 0x14 a frame
    CHECK(std::filesystem::file_size(path) == 6 * 0x1c + 0x28 * 0x1c + 40 + 0x18 + 100 * 0x14);
    scr::Replay l;
    REQUIRE(l.load(path));
    CHECK(l.car == 5);
    CHECK(l.laps == 5);
    CHECK(l.level == 2);
    CHECK(l.race_options == r.race_options);
    REQUIRE(l.frames.size() == 100);
    CHECK(l.frames[42].accel == 420);
    CHECK(l.frames[43].steer_right == -32768);
    CHECK(l.frames[99].frame_time == r.frames[99].frame_time);
    std::filesystem::remove(path);
}

TEST_CASE("replay: the original's replays load") {
    auto dir = scr::find_game_dir();
    if (!dir) SKIP("original game data not present");
    std::error_code ec;
    int n = 0;
    for (const auto& e : std::filesystem::directory_iterator(*dir + "/REPLAYS", ec)) {
        if (e.path().extension() != ".RPL" && e.path().extension() != ".rpl") continue;
        scr::Replay r;
        REQUIRE(r.load(e.path().string()));
        CHECK(!r.frames.empty());
        CHECK(r.car >= 0);
        CHECK(r.car < 13);
        CHECK(r.laps == 3);  // records keep replays of 3-lap races only
        CHECK(r.frames[0].frame_time > 0.0f);
        ++n;
    }
    if (n == 0) SKIP("no replays in the game's REPLAYS folder");
}

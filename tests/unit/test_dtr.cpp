#include "testing.h"

#include "core/archive.h"
#include "core/dtr.h"
#include "core/gamedata.h"

#include <cstring>

namespace {
const char* kSample =
    "TRACK_DATA\r\n"
    "*\tcolor\tcamera_hi\tbonus_time\tcarnage\r\n"
    "\td20\td300\td45\tx1F\r\n"
    "END_LIST\r\n"
    "OPPONENTS\r\n"
    "*\tmax_spd\taccel\r\n"
    "\tf14000\tf1100\r\n"
    "\tf13900\tf1000\r\n"
    "END_LIST\r\n"
    "NAMES\r\n"
    "\tsazcono\tsazbunny\r\n"
    "END_LIST\r\n";

int32_t i32(const std::vector<uint8_t>& v, size_t o) {
    int32_t x;
    std::memcpy(&x, v.data() + o, 4);
    return x;
}
float f32(const std::vector<uint8_t>& v, size_t o) {
    float x;
    std::memcpy(&x, v.data() + o, 4);
    return x;
}
} // namespace

TEST_CASE("dtr: typed values and comments") {
    scr::DtrFile f(kSample);
    auto v = f.section("track_data");
    REQUIRE(v.has_value());
    REQUIRE(v->size() == 4);
    CHECK((*v)[0].type == 'd' && (*v)[0].i == 20);
    CHECK((*v)[3].type == 'x' && (*v)[3].i == 0x1F);
    CHECK(!f.section("MISSING").has_value());
}

TEST_CASE("dtr: records are packed like data_load_section") {
    scr::DtrFile f(kSample);
    size_t n = 0;
    auto opp = f.pack_section("OPPONENTS", 0x20, 8, &n);  // stride larger than the record
    REQUIRE(opp.has_value());
    CHECK(n == 2);
    CHECK(opp->size() == 0x40);
    CHECK(f32(*opp, 0) == 14000.0f);
    CHECK(f32(*opp, 4) == 1100.0f);
    CHECK(f32(*opp, 0x20) == 13900.0f);
    auto names = f.pack_section("NAMES", 12, 12, &n);
    REQUIRE(names.has_value());
    CHECK(n == 2);
    CHECK(std::strcmp(reinterpret_cast<const char*>(names->data()), "azcono") == 0);
    CHECK(std::strcmp(reinterpret_cast<const char*>(names->data() + 12), "azbunny") == 0);
}

TEST_CASE("dtr: CARS_DATA from GEN.DAT matches what the game has in memory") {
    auto dir = scr::find_game_dir();
    if (!dir) SKIP("original game data not present");
    auto path = scr::resolve_game_file(*dir, "DATA/GEN.DAT");
    REQUIRE(path.has_value());
    auto a = scr::Archive::open(*path);
    REQUIRE(a.has_value());
    auto text = a->read("CARS.DTR");
    REQUIRE(text.has_value());
    scr::DtrFile f(std::string(text->begin(), text->end()));
    size_t n = 0;
    auto cars = f.pack_section("CARS_DATA", 0x40, 0x40, &n);
    REQUIRE(cars.has_value());
    CHECK(n == 13);  // g_num_cars in the running game
    // First record as found at g_cars_data in a memory dump of the running game.
    CHECK(std::strcmp(reinterpret_cast<const char*>(cars->data()), "AUTO00") == 0);
    CHECK(i32(*cars, 0x0c) == 0);
    CHECK(f32(*cars, 0x10) == 21000.0f);
    CHECK(f32(*cars, 0x14) == 9000.0f);
    CHECK(f32(*cars, 0x18) == 1.05f);
    CHECK(i32(*cars, 0x24) == 6);
    CHECK(i32(*cars, 0x40 + 0x0c) == 0x8000);
}

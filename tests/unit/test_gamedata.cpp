#include "testing.h"

#include "core/gamedata.h"

#include <SDL3/SDL.h>

TEST_CASE("gamedata: missing directory is rejected") {
    CHECK(!scr::looks_like_game_dir(""));
    CHECK(!scr::looks_like_game_dir("Z:/definitely/not/here"));
}

// The tests below need the user's original files (never in the repo) and skip without them.
TEST_CASE("gamedata: original install is found") {
    auto dir = scr::find_game_dir();
    if (!dir) SKIP("original game data not present");
    CHECK(scr::looks_like_game_dir(*dir));
}

TEST_CASE("gamedata: case-insensitive file lookup") {
    auto dir = scr::find_game_dir();
    if (!dir) SKIP("original game data not present");
    auto p = scr::resolve_game_file(*dir, "gfx/menu.dat");
    REQUIRE(p.has_value());
    auto data = scr::read_file(*p);
    REQUIRE(data.has_value());
    CHECK(data->size() > 0);
    CHECK(!scr::resolve_game_file(*dir, "gfx/nope.dat").has_value());
}

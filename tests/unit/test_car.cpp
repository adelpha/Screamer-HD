#include "testing.h"

#include "core/archive.h"
#include "core/car.h"
#include "core/dtr.h"
#include "core/gamedata.h"
#include "core/sim_clock.h"
#include "core/track.h"

#include <cmath>
#include <optional>
#include <string>

namespace {

struct World {
    scr::Track track;
    scr::Model model;
    scr::CarSetup setup;
};

std::optional<World> load_world() {
    auto dir = scr::find_game_dir();
    if (!dir) return std::nullopt;
    auto map = scr::resolve_game_file(*dir, "GFX/MAP2H.DAT");
    auto gen = scr::resolve_game_file(*dir, "DATA/GEN.DAT");
    if (!map || !gen) return std::nullopt;
    auto track = scr::load_track(*map, "MAP2");
    auto model = scr::load_object(*map, "AUTO00.3DX");
    auto a = scr::Archive::open(*gen);
    auto text = a ? a->read("CARS.DTR") : std::nullopt;
    if (!track || !model || !text) return std::nullopt;
    auto cars = scr::DtrFile(std::string(text->begin(), text->end())).pack_section("CARS_DATA", 0x40, 0x40);
    if (!cars) return std::nullopt;
    World w{std::move(*track), std::move(*model), {}};
    w.setup = scr::car_setup(cars->data(), w.model);
    return w;
}

// Runs the car at 50 fps (2 ticks per frame) like a headless run.
void drive(scr::Car& car, const scr::Track& track, const scr::CarInput& in, int frames) {
    scr::SimClock clock;
    for (int i = 0; i < frames; ++i) {
        clock.add_ticks(2);
        clock.begin_frame();
        car.update(track, in, clock.frame_time(), clock.pow_table_row());
    }
}

} // namespace

TEST_CASE("car: setup from CARS_DATA and the AUTO00 wheels") {
    auto w = load_world();
    if (!w) SKIP("original game data not present");
    CHECK(w->setup.maxspd == 21000.0f);
    CHECK(w->setup.n_shifts == 6);
    // Wheel placements: left wheels at +226, front at -445, rear at +490.
    CHECK(w->setup.wheel_pos[0][0] == 226);
    CHECK(w->setup.wheel_pos[0][1] == -445);
    CHECK(w->setup.wheel_pos[1][1] == 490);
    CHECK(w->setup.wheel_pos[2][0] == -226);
    scr::Car car;
    car.reset(w->setup, w->track, 44000, 4550);
    CHECK(car.track_width == -452);  // -(left x - right x), as player_car_load computes it
    CHECK(car.wheelbase == 935);
    CHECK(car.pos_y == 0);  // the start straight is flat
}

TEST_CASE("car: full throttle on the start straight") {
    auto w = load_world();
    if (!w) SKIP("original game data not present");
    scr::Car car;
    car.reset(w->setup, w->track, 44000, 4550);
    const float yaw0 = car.yaw;
    scr::CarInput in;
    in.accel = 1023;
    // Revving on the grid, then flooring it at the start spins the wheels first
    // (crash_state 5): +1000 units/s^2, as in recordings of the original.
    in.race_started = false;
    drive(car, w->track, in, 100);
    CHECK(car.speed == 0.0f);
    CHECK(car.engine_rpm > 8500.0f);
    in.race_started = true;
    drive(car, w->track, in, 20);
    CHECK(car.crash_state == 5);
    CHECK(std::abs(car.speed - 20.0f * 20.0f) < 25.0f);
    drive(car, w->track, in, 230);
    CHECK(car.speed > 4000.0f);
    CHECK(car.gear >= 2);
    CHECK(car.pos_x > 46000);           // the straight runs along +x
    CHECK(std::abs(car.pos_z - 4550) < 200);
    CHECK(car.yaw == yaw0);
    CHECK(!car.wall_contact);
    const float v = car.speed;
    // Braking slows the car down.
    scr::CarInput b;
    b.brake = 1023;
    drive(car, w->track, b, 50);
    CHECK(car.speed < v * 0.5f);
}

TEST_CASE("car: steering turns the car and the walls keep it on the road") {
    auto w = load_world();
    if (!w) SKIP("original game data not present");
    scr::Car car;
    car.reset(w->setup, w->track, 44000, 4550);
    const float yaw0 = car.yaw;
    scr::CarInput in;
    in.accel = 1023;
    in.steer_left = 1023;
    drive(car, w->track, in, 300);
    CHECK(car.yaw != yaw0);
    // Still over the road: the ground is found under the car.
    CHECK(scr::ground_query(w->track, car.pos_x, car.pos_z).has_value());
}

TEST_CASE("car: deterministic for the same inputs") {
    auto w = load_world();
    if (!w) SKIP("original game data not present");
    scr::Car a, b;
    a.reset(w->setup, w->track, 44000, 4550);
    b.reset(w->setup, w->track, 44000, 4550);
    scr::CarInput in;
    in.accel = 1023;
    in.steer_right = 1023;
    drive(a, w->track, in, 200);
    drive(b, w->track, in, 200);
    CHECK(a.pos_x == b.pos_x);
    CHECK(a.pos_z == b.pos_z);
    CHECK(a.speed == b.speed);
}

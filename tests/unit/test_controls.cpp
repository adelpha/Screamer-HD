#include "testing.h"

#include "core/archive.h"
#include "core/car.h"
#include "core/controls.h"
#include "core/dtr.h"
#include "core/gamedata.h"
#include "core/replay.h"
#include "core/sim_clock.h"
#include "core/track.h"
#include "input.h"

#include <cstring>
#include <filesystem>
#include <optional>
#include <string>

namespace {

// a joystick setup as SETUP.EXE writes it: accel and brake on the y axis (accel forward,
// negative), steering on the x axis
scr::ControlSetup joystick_setup(int device) {
    scr::ControlSetup s;
    s.b[scr::kActAccel] = {1, 1, device, 0, 1, -1, 0};
    s.b[scr::kActBrake] = {1, 1, device, 0, 1, 1, 0};
    s.b[scr::kActSteer] = {1, 1, device, 0, 0, 1, 0};
    s.b[scr::kActSteerRight] = {1, 1, device, 0, 0, 1, 0};
    return s;
}

struct World {
    scr::Track track;
    scr::Model model;
    scr::CarSetup setup;
};

std::optional<World> load_world(int car = 0) {
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
    w.setup = scr::car_setup(cars->data() + 0x40 * car, w.model);
    return w;
}

void drive(scr::Car& car, const scr::Track& track, const scr::CarInput& in, int frames) {
    scr::SimClock clock;
    for (int i = 0; i < frames; ++i) {
        clock.add_ticks(2);
        clock.begin_frame();
        car.update(track, in, clock.frame_time(), clock.pow_table_row());
    }
}

} // namespace

TEST_CASE("controls: the default setup is DEFCTRL.CFG's keyboard") {
    scr::ControlSetup s;
    CHECK(s.b[scr::kActAccel].key == 0x48);    // Up
    CHECK(s.b[scr::kActBrake].key == 0x50);    // Down
    CHECK(s.b[scr::kActGearUp].key == 0x38);   // Alt
    CHECK(s.b[scr::kActGearDown].key == 0x1d); // Ctrl
    CHECK(s.steer_mode() == 1);
    CHECK(!s.brake_pedal());
    for (const auto& b : s.b) CHECK(b.dir == 1);
}

TEST_CASE("controls: controls_update's axes (clamp, dead zone, read as keys, shared axis)") {
    CHECK(scr::controls_axis_value(0x65, 0) == 0);
    CHECK(scr::controls_axis_value(-0x65, 0) == 0);
    CHECK(scr::controls_axis_value(0x66, 0) == 0x66);
    CHECK(scr::controls_axis_value(5000, 0) == 1023);
    CHECK(scr::controls_axis_value(-5000, 0) == -1023);
    // device 2: past half way the whole way, below the dead zone nothing, between as it is
    CHECK(scr::controls_axis_value(0x200, 2) == 1023);
    CHECK(scr::controls_axis_value(-0x200, 2) == -1023);
    CHECK(scr::controls_axis_value(0x1ff, 2) == 0x1ff);
    CHECK(scr::controls_axis_value(0x50, 2) == 0);

    const scr::ControlSetup s = joystick_setup(0);
    std::array<int, 6> in{};
    // the brake shares the accelerator's axis: it gets the same (signed) value
    scr::controls_update(s, {-700, 999, 300, 999, 0, 0}, in);
    CHECK(in[scr::kActAccel] == -700);
    CHECK(in[scr::kActBrake] == -700);
    CHECK(in[scr::kActSteer] == 300);
    CHECK(in[scr::kActSteerRight] == 300);
    CHECK(in[scr::kActGearUp] == 0);  // keyboard, not pressed
    scr::controls_update(s, {0, 0, 0, 0, 1, 0}, in);
    CHECK(in[scr::kActGearUp] == 1023);
    // a disabled binding leaves its value alone
    scr::ControlSetup d = s;
    d.b[scr::kActGearUp].enabled = 0;
    scr::controls_update(d, {0, 0, 0, 0, 0, 0}, in);
    CHECK(in[scr::kActGearUp] == 1023);
}

TEST_CASE("controls: CONTROLS.CFG round trip") {
    scr::ControlsFile f;
    f.setup = joystick_setup(2);
    f.calibration[5] = 7;
    f.ports = {513, 515, 517, 519, 0, 0, 0, 0, 0, 0};
    const auto b = f.bytes();
    CHECK(b.size() == 0x530);  // (CONTROLS.CFG's 1328 bytes)
    scr::ControlsFile g;
    REQUIRE(g.parse(b.data(), b.size()));
    CHECK(g.setup == f.setup);
    CHECK(g.calibration[5] == 7);
    CHECK(g.ports[3] == 519);
    CHECK(!g.parse(b.data(), 100));
}

TEST_CASE("controls: the original's CONTROLS.CFG") {
    auto dir = scr::find_game_dir();
    auto path = dir ? scr::resolve_game_file(*dir, "CONFIG/CONTROLS.CFG") : std::nullopt;
    if (!path) SKIP("original game data not present");
    scr::ControlsFile f;
    REQUIRE(f.load(*path));
    for (const auto& b : f.setup.b) CHECK(b.enabled == 1);
}

TEST_CASE("replay: our setup, the per-frame marks and the analog brake survive a save") {
    scr::Replay r;
    // frames 0-59 keys, 60-99 a stick; the brake a pedal in 20-29, a key in 30-34
    for (int i = 0; i < 100; ++i) {
        scr::CarInput in;
        in.accel = 1023;
        if (i < 60) {
            in.steer = i % 2 ? 1023 : 0;
            in.steer_right = i % 3 ? 0 : 1023;
        } else {
            in.setup.b[scr::kActSteer].device = 0;
            in.setup.b[scr::kActSteer].axis = 1;
            in.steer = -400 + i;
        }
        if (i >= 20 && i < 30) {
            in.setup.b[scr::kActBrake].device = 0;
            in.setup.b[scr::kActBrake].axis = 1;
            in.brake = 300 + i;
        } else if (i >= 30 && i < 35) {
            in.brake = 1023;
        }
        in.gear_up = i == 50 ? 1023 : 0;
        r.add_frame(in, 0.02f, 0.02f);
    }
    scr::ControlsFile base;
    base.ports[0] = 513;
    r.finish_recording(base);
    // keys steered most frames, the pedal braked most of the braking frames
    CHECK(r.controls.setup.steer_mode() == 1);
    CHECK(r.controls.setup.brake_pedal());
    CHECK(r.frames[0].steer_right != scr::Replay::kSteerAxis);
    CHECK(r.frames[70].steer_right == scr::Replay::kSteerAxis);
    CHECK(r.frames[25].brake == 325);                           // as the file's setup says
    CHECK(r.frames[31].brake == scr::Replay::kBrakeKey + 1023); // marked: a key

    const std::string path = (std::filesystem::temp_directory_path() / "scr_test_controls.rpl").string();
    REQUIRE(r.save(path));
    scr::Replay l;
    REQUIRE(l.load(path));
    CHECK(l.has_setup);
    CHECK(l.controls.ports[0] == 513);
    for (int i = 0; i < 100; ++i) {
        scr::CarInput in;
        l.read_frame(l.frames[size_t(i)], in);
        if (i < 60) {
            CHECK(in.setup.steer_mode() == 1);
            CHECK(in.steer == (i % 2 ? 1023 : 0));
            CHECK(in.steer_right == (i % 3 ? 0 : 1023));
        } else {
            CHECK(in.setup.steer_mode() == 0);
            CHECK(in.steer == -400 + i);
            CHECK(in.steer_right == 0);
        }
        if (i >= 20 && i < 30) {
            CHECK(in.setup.brake_pedal());
            CHECK(in.brake == 300 + i);
        } else if (i >= 30 && i < 35) {
            CHECK(!in.setup.brake_pedal());
            CHECK(in.brake == 1023);
        }
        CHECK(in.gear_up == (i == 50 ? 1023 : 0));
    }
    std::filesystem::remove(path);
}

TEST_CASE("replay: ours from before (no setup) play with the keyboard's and the -32768 mark") {
    scr::Replay r;  // has_setup false: as loaded from a file of zeros
    scr::ReplayFrame f;
    f.accel = 1023;
    f.steer_left = 500;
    f.steer_right = -32768;
    scr::CarInput in;
    r.read_frame(f, in);
    CHECK(in.setup.steer_mode() == 0);
    CHECK(in.steer == 500);
    CHECK(in.setup.b[scr::kActAccel].dir == 1);
    f.steer_right = 1023;
    r.read_frame(f, in);
    CHECK(in.setup.steer_mode() == 1);
    CHECK(in.steer_right == 1023);
}

TEST_CASE("replay: a replay recorded with the original's joystick plays with its setup") {
    // A made-up .RPL as the original writes it for a joystick player: accel the y axis's
    // negative half, the brake its positive half (a pedal), steering the x axis.
    scr::Replay r;
    r.controls.setup = joystick_setup(0);
    r.has_setup = true;
    scr::ReplayFrame f;
    f.accel = -1023;
    f.brake = -1023;  // (the same axis)
    f.steer_left = -300;
    r.frames.push_back(f);
    const std::string path = (std::filesystem::temp_directory_path() / "scr_test_joy.rpl").string();
    REQUIRE(r.save(path));
    scr::Replay l;
    REQUIRE(l.load(path));
    scr::CarInput in;
    l.read_frame(l.frames[0], in);
    CHECK(in.setup.b[scr::kActAccel].dir == -1);
    CHECK(in.setup.brake_pedal());
    CHECK(in.setup.steer_mode() == 0);
    CHECK(in.accel == -1023);
    CHECK(in.steer == -300);
    std::filesystem::remove(path);

    auto w = load_world();
    if (!w) SKIP("original game data not present");
    // the physics takes the accel (its sign is the binding's) and not the brake (wrong sign)
    scr::Car car;
    car.reset(w->setup, w->track, 44000, 4550);
    drive(car, w->track, in, 150);
    CHECK(car.speed > 1000.0f);
    CHECK(car.brake == 0.0f);
    in.accel = -700;  // part throttle
    drive(car, w->track, in, 1);
    CHECK(std::abs(car.throttle - 700.0f / 1023.0f) < 1e-6f);
    // the stick steers left (negative); the angle snaps to it while the tyres grip
    CHECK(car.steer > 0.0f);
    // pulling back: brake by the pedal's amount at once, no throttle
    in.accel = in.brake = 400;
    drive(car, w->track, in, 1);
    CHECK(car.throttle == 0.0f);
    CHECK(std::abs(car.brake - 400.0f / 1023.0f) < 1e-6f);
}

TEST_CASE("car: the inputs count only with their bindings' directions") {
    auto w = load_world();
    if (!w) SKIP("original game data not present");
    scr::Car car;
    car.reset(w->setup, w->track, 44000, 4550);
    scr::CarInput in;
    in.accel = -1023;  // keyboard setup: direction +1
    drive(car, w->track, in, 100);
    CHECK(car.speed == 0.0f);
    CHECK(car.throttle == 0.0f);
    in.setup.b[scr::kActAccel].dir = -1;
    drive(car, w->track, in, 100);
    CHECK(car.speed > 0.0f);
    // a ramped brake (keys, and an axis read as keys) against a pedal
    scr::CarInput b;
    b.brake = 300;
    scr::Car k = car, p = car, d = car;
    drive(k, w->track, b, 1);
    b.setup.b[scr::kActBrake].device = 0;
    b.setup.b[scr::kActBrake].axis = 1;
    drive(p, w->track, b, 1);
    b.setup.b[scr::kActBrake].device = 2;
    drive(d, w->track, b, 1);
    CHECK(std::abs(p.brake - 300.0f / 1023.0f) < 1e-6f);
    CHECK(k.brake == d.brake);
    CHECK(k.brake != p.brake);
}

TEST_CASE("car: steering modes (joystick snaps, axis read as keys ramps, keys ramp)") {
    auto w = load_world();
    if (!w) SKIP("original game data not present");
    scr::Car base;
    base.reset(w->setup, w->track, 44000, 4550);
    scr::CarInput in;
    in.steer = 1023;
    scr::Car joy = base, axis = base, keys = base;
    in.setup.b[scr::kActSteer].device = 0;
    drive(joy, w->track, in, 1);
    in.setup.b[scr::kActSteer].device = 2;
    drive(axis, w->track, in, 1);
    in.setup.b[scr::kActSteer].device = 1;
    drive(keys, w->track, in, 1);
    // the same full lock: the stick straight away to the right, the axis eased that way, the
    // (left) key eased the other way
    CHECK(joy.steer < 0.0f);
    CHECK(axis.steer < 0.0f);
    CHECK(axis.steer > joy.steer);
    CHECK(keys.steer == -axis.steer);
    // a device the steering does not know: no change
    in.setup.b[scr::kActSteer].device = 3;
    scr::Car other = joy;
    drive(other, w->track, in, 1);
    CHECK(other.steer == joy.steer);
}

TEST_CASE("car: a manual gearbox changes one gear per press") {
    auto w = load_world();
    if (!w) SKIP("original game data not present");
    scr::CarSetup s = w->setup;
    s.shiftmode = 0;
    scr::Car car;
    car.reset(s, w->track, 44000, 4550);
    scr::CarInput in;
    in.accel = 1023;
    // revving on the grid (below walking pace the gear drops back to 0 every frame)
    in.race_started = false;
    drive(car, w->track, in, 100);
    in.race_started = true;
    in.gear_up = 1023;
    drive(car, w->track, in, 10);  // held: one gear
    CHECK(car.gear == 1);
    in.gear_up = 0;
    drive(car, w->track, in, 150);
    CHECK(car.gear == 1);  // (no automatic changes)
    in.gear_up = 1023;
    drive(car, w->track, in, 1);
    CHECK(car.gear == 2);
    in.gear_up = 0;
    in.gear_down = 1023;
    drive(car, w->track, in, 1);
    CHECK(car.gear == 2);  // the latch: let go of both first
    in.gear_down = 0;
    drive(car, w->track, in, 1);
    in.gear_down = 1023;
    drive(car, w->track, in, 1);
    CHECK(car.gear == 1);
    // a gear binding the other way round ignores a positive value
    in.gear_down = 0;
    drive(car, w->track, in, 1);
    in.setup.b[scr::kActGearUp].dir = -1;
    in.gear_up = 1023;
    drive(car, w->track, in, 1);
    CHECK(car.gear == 1);
    in.gear_up = -1023;
    drive(car, w->track, in, 1);
    CHECK(car.gear == 2);
}

TEST_CASE("input: bindings - defaults, rebinding, CONTROLS.CFG keys, save and load") {
    using namespace scr::input;
    Bindings b = Bindings::defaults();
    CHECK(b.keys[kBrake][0].code == SDL_SCANCODE_SPACE);
    CHECK(b.keys[kBrake][1].code == SDL_SCANCODE_DOWN);
    CHECK(b.keys[kGearUp][0].code == SDL_SCANCODE_PAGEUP);
    CHECK(b.pad[kLookBack][0].code == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    // a key: first place, the old first second; again: alone; taken from other actions
    Bind q;
    q.kind = Bind::kKey;
    q.code = SDL_SCANCODE_Q;
    b.assign(kGearUp, false, q);
    CHECK(b.keys[kGearUp][0].code == SDL_SCANCODE_Q);
    CHECK(b.keys[kGearUp][1].code == SDL_SCANCODE_PAGEUP);
    b.assign(kGearUp, false, q);
    CHECK(b.keys[kGearUp][1].kind == Bind::kNone);
    Bind up;
    up.kind = Bind::kKey;
    up.code = SDL_SCANCODE_UP;
    b.assign(kGearUp, false, up);
    CHECK(b.keys[kAccel][0].kind == Bind::kNone);
    CHECK(b.keys[kGearUp][0].code == SDL_SCANCODE_UP);
    b.toggle_digital(kBrake);
    CHECK(b.pad[kBrake][0].digital);

    // SETUP.EXE's keys: Alt / Ctrl for the gears (ours stay second)
    scr::ControlsFile f;
    Bindings o = Bindings::defaults();
    o.import_original(f);
    CHECK(o.keys[kAccel][0].code == SDL_SCANCODE_UP);
    CHECK(o.keys[kAccel][1].kind == Bind::kNone);
    CHECK(o.keys[kBrake][0].code == SDL_SCANCODE_DOWN);
    CHECK(o.keys[kBrake][1].code == SDL_SCANCODE_SPACE);
    CHECK(o.keys[kGearUp][0].code == SDL_SCANCODE_LALT);
    CHECK(o.keys[kGearUp][1].code == SDL_SCANCODE_PAGEUP);
    CHECK(o.keys[kGearDown][0].code == SDL_SCANCODE_LCTRL);
    CHECK(scancode_to_dos(SDL_SCANCODE_PAGEUP) == 0x49);
    CHECK(scancode_to_dos(SDL_SCANCODE_RCTRL) == 0x1d);
    CHECK(scancode_from_dos(0x48) == SDL_SCANCODE_UP);

    Bind wheel;
    wheel.kind = Bind::kJoyAxis;
    wheel.code = 2;
    wheel.dir = -1;
    wheel.full = true;
    wheel.guid = "0300abcd";
    b.assign(kBrake, true, wheel);
    b.rumble = false;
    b.pad_guid = "03001234";
    const std::string path = (std::filesystem::temp_directory_path() / "scr_test_controls.ini").string();
    REQUIRE(b.save(path));
    Bindings l;
    REQUIRE(l.load(path));
    CHECK(l == b);
    std::filesystem::remove(path);
    CHECK(wheel.name() == "JOY AXIS 3-");
    CHECK(Bindings::defaults().pad[kSteerLeft][0].name() == "PAD LX-");
    CHECK(Bindings::defaults().pad[kAccel][0].name() == "PAD RT");
}

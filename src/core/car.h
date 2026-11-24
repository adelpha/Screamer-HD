#pragma once
// The player car's physics, ported from STARTL.EXE's race_frame pipeline:
//
//   car_integrate_motion  -> Car::integrate_motion   (move from speed/yaw, wall impacts, slides)
//   car_update_driving    -> engine_gearbox, throttle, brake, steering
//   car_update_dynamics   -> wheel_loads (body lean), skid
//   car_update_track_contact -> corner probes (walls), wheel ground heights, track heading
//                            (wall impacts), body pitch/roll from the wheels
//
// Units are the game's: positions in map units (int), angles in radians (float), speed in
// map units per second, the frame time from SimClock (0.02 s at 50 fps). Smoothing goes
// through the game's per-fps damping table (pow_table), indexed like lerp_toward's k.
// Field offsets in the comments are PlayerCar's (reference/symbols/STARTL.types.h).
//
// crash_state: 0 driving, 2 just hit a wall too hard (car_set_state), 1 tumbling until the
// fourth bounce, 5 wheelspin at the start, 6 wheelspin over.
// Not ported yet: spins (spin_state), opponents and obstacles, checkpoints, sounds, debris.
// Network catch-up (speed_factor at 0x300) stays 1.

#include "core/track.h"

#include <array>
#include <cstdint>

namespace scr {

// The controls as the game reads them (g_input_*): analog 0..1023, digital keys give 1023.
struct CarInput {
    int accel = 0;        // g_input_accel
    int brake = 0;        // g_input_brake
    int steer_left = 0;   // g_input_steer        (keyboard mode: two keys)
    int steer_right = 0;  // 0x119888
    int steer = 0;        // analog steering -1023 (left) .. 1023 (right); used when analog
    bool analog_steer = false;  // 0x119814 == 0 (joystick) instead of 1 (keyboard)
    bool analog_brake = false;  // brake pedal on an axis: no ramp
    bool gear_up = false;       // manual gearbox only
    bool gear_down = false;
    bool race_started = true;   // g_race_flags & 1: before the start only the revs move
};

// Per-car constants from CARS_DATA (CarData) and the wheel positions in the .3DX model.
struct CarSetup {
    float maxspd = 0, maxrpm = 0, grip = 0, accel = 0;
    int shiftmode = 1;  // 0 manual, else automatic
    int n_shifts = 5;
    std::array<float, 6> sh_rap{};
    int animation = 0;  // CarData.animation (paint scheme bit)
    // Wheel centres in the model (x, z): front left, rear left, front right, rear right
    // (part placements of flx/fli, blx/rli, frx/fri, brx/rri).
    std::array<std::array<int, 2>, 4> wheel_pos{};
};

// CarData fields from a packed CARS_DATA record (0x40 bytes, see DtrFile::pack_section) and the
// wheel positions from the car model.
CarSetup car_setup(const uint8_t* car_data, const Model& car_model);

struct CarWheel {
    int x = 0, y = 0, z = 0;          // +0x00 world position (rotated with yaw + drift)
    int x2 = 0, y2 = 0, z2 = 0;       // +0x0c second probe (rotated with yaw only)
    int local_x = 0, local_z = 0;     // +0x18 / +0x20 position in the car
    int ground_y = 0;                 // +0x24
    int ground_y2 = 0;                // +0x28
    bool bumpy = false;               // +0x2c surface 'e'
    float load_time = 0;              // +0x30
    int lift = 0;                     // +0x34 suspension wobble
    float load_lat = 0;               // +0x38
    float load_long = 0;              // +0x40
};

struct CarProbe {
    int local_x = 0, local_z = 0;  // +0x00 / +0x08 corner in the car
    int x = 0, z = 0;              // +0x0c / +0x14
    bool hit = false;              // +0x18 off the road
    int push_x = 0, push_z = 0;    // +0x1c / +0x24
    float heading = 0;             // +0x28 heading of the section that pushed (stored as int)
};

class Car {
public:
    Car() = default;
    // player_car_load: places the car at x, z facing the track direction.
    void reset(const CarSetup& setup, const Track& track, int x, int z);

    // One frame of race_frame for the player car, in the game's order.
    void update(const Track& track, const CarInput& in, float frame_time, int pow_row);

    // Takes the state from a copy of the game's g_player_car (at least 0x800 bytes), for
    // comparing single frames with recordings of the original (tools/dosbox/record-race.ps1).
    void load_player_car(const uint8_t* p);

    // State (PlayerCar offsets)
    int pos_x = 0, pos_y = 0, pos_z = 0;   // 0x4b0..0x4b8
    int ground_y = 0;                      // 0x4b4 region: average wheel ground
    float pitch = 0, yaw = 0, roll = 0;    // 0x4c8, 0x4cc, 0x4d0
    float ground_pitch = 0, ground_roll = 0;  // 0x4d4, 0x4dc
    float lean_pitch = 0, lean_roll = 0;   // body lean from the wheel loads
    float speed = 0, prev_speed = 0;       // 0x50c, 0x588
    float speed_limit = 0;
    float engine_rpm = 0;
    int gear = 0;
    bool gear_latch = false;
    float throttle = 0, brake = 0;         // 0x144, 0x114
    float braking = 0;                     // 0x110 brake skid strength (float bits in the game)
    float steer = 0;                       // 0x108 steering angle (radians)
    float steer_max = 0;                   // 0x10c
    float turn_rate = 0;                   // 0x2f4 yaw change this frame
    float turn_radius = 0;                 // 0x2f8
    int arc_dx = 0, arc_dz = 0;            // 0x1c / 0x24 movement along the turning circle
    float yaw_rate = 0;
    float impact_vel_x = 0, impact_vel_z = 0, impact_timer = 0;
    float skid = 0, skid_prev = 0;         // 0x4f4, ...
    float skid_level = 0, slip_a = 0, slip_b = 0;
    float drift_angle = 0;                 // 0x128
    float surface_grip = 1.0f;
    int crash_state = 0;                   // 0x148
    float crash_bounce = 0;                // 0x14c
    int crash_bounce_count = 0;            // 0x150
    float crash_pitch_rate = 0, crash_roll_rate = 0;  // 0x154, 0x15c
    float body_pitch_ofs = 0, body_roll_ofs = 0;      // 0x124, 0x12c (drawing only)
    int body_ofs_y = 0;                    // 0x134
    int cam_shake_x = 0;
    int body_lift = 0;
    bool wall_contact = false;
    int wall_push_x = 0, wall_push_z = 0;
    float wall_heading = 0;                // 0x50 heading of the section that pushed
    float track_heading = 0;
    bool bumpy_front = false;              // brake_mode at 0x2f0: a front wheel on 'e'
    std::array<CarWheel, 4> wheel{};
    std::array<CarProbe, 4> probe{};

    // Derived from the setup (player_car_load)
    CarSetup setup;
    int track_width = 0;    // 0x2e4
    int wheelbase = 0;      // 0x2ec
    float rpm_per_speed = 0, engine_drag = 0, brake_decel = 0, slide_threshold = 0;
    float grip_limit = 0, skid_brake_limit = 0, max_accel = 0, brake_skid_limit = 0;

    uint32_t render_animation() const;  // object animation word (paint, braking, slow)

private:
    float lerp(float value, float target, int k) const;  // lerp_toward
    void integrate_motion();
    void engine_gearbox(const CarInput& in);
    void update_throttle(const CarInput& in);
    void update_brake(const CarInput& in);
    void update_steering(const CarInput& in);
    void wheel_loads();
    void update_skid();
    void corner_probes(const Track& track);
    void resolve_corner_probes(const Track& track);
    void apply_wall_push(const Track& track);
    void wheel_ground(const Track& track);
    void update_track_heading(const Track& track);
    void body_from_wheels();
    void crash_update(const Track& track);
    void set_state(int s) { if (crash_state == 0) crash_state = s; }  // car_set_state

    float dt_ = 0.02f;
    int pow_row_ = 49;
};

// The chase camera (camera_mode_car, views 1/6/9): height above the car, distance behind,
// pitch offset; positions and angles follow the car through lerp_toward / angle_lerp_toward.
struct ChaseCamera {
    float pitch = 0, yaw = 0, roll = 0;  // radians
    int x = 0, y = 0, z = 0;
    bool snap = true;                    // first frame: no smoothing
    void update(const Car& car, float frame_time, int pow_row, int view = 1);
};

} // namespace scr

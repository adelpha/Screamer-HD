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
// Sounds are cues for RaceAudio; obstacles are in core/obstacles.
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
    bool cpu_cars_go = true;    // g_race_flags & 5: the CPU cars drive
    // g_race_flags & 0x4000: out of time or past the finish: no throttle, no gear changes,
    // the brake held at half (car_update_engine_gearbox sets g_input_brake to 512)
    bool coasting = false;
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
    int wheel_y = 0;    // height of the wheel parts (bumper camera)
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

// A puff of tyre smoke (SMOKE.3DX; car_update_debris / car_sync_debris_objects): eight slots
// of 0x28 bytes at PlayerCar +0x308.
struct SmokePuff {
    bool on = false;                // +0x08
    int x = 0, y = 0, z = 0;        // world position (instance +0x08..)
    int local_x = 0, local_z = 0;   // +0x18 / +0x20 where it came out, in the car
    int32_t animation = 0;          // instance +0x20: 0..0xffff over its life (the texture frames)
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
    // camera_update's part in the physics: run after update (and the collisions).
    void camera_update();
    bool camera_car_mode = true;  // the view is a camera_mode_car one (F3, F4)

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
    int spin_state = 0;                    // 0x170: 1 starts a spin, 2 / 4 unwinding it
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
    // Bumpers (car_update_bumpers): two points on the centre line, a quarter of the way in
    // from the front and rear corner probes; collision_radius = half their distance.
    int front_local_z = 0, rear_local_z = 0, collision_radius = 0;
    int front_x = 0, front_z = 0, rear_x = 0, rear_z = 0;
    // Track markers (car_update_track_progress, simplified to marker, lap and progress).
    int track_marker = 0, lap = 0, marker_flags = 0;
    // car_apply_surface: the current marker's flags as the body's +0x3c word (0, 0x8000,
    // 0xff00), which picks the window reflection frame (Pose::reflection).
    int surface_render_flag = 0;
    float track_progress = 0;
    // Sound cues from this frame, played by RaceAudio (the original calls sfx_* inline):
    bool crash_sound = false;  // car_update_crash_state 2: sfx 8 (dentone) and the "crash" line
    bool wall_sound = false;   // car_update_track_heading, a wall without a crash: sfx 4 (wallh1)
    // Tyre smoke: while the skid noise plays, during crash bounces and in crash state 5, a
    // puff leaves each rear wheel every 0.05 s; it follows the car's frame and lives 2/3 s.
    std::array<SmokePuff, 8> smoke{};
    // car_sync_render_object's part table: body lean (x2 pitch, x4 roll), the body sections at
    // their suspension height less body_lift, the front wheels steered (steer / (1.5 max)) and
    // every wheel rolled by speed / 15 a frame.
    int32_t wheel_roll = 0;
    void part_poses(const Model& model, const CarParts& cp, std::vector<PartPose>& out, bool advance = true);
    float smoke_timer = 0;          // +0x304
    uint32_t frame_counter = 0;     // g_frame_counter (the frames this car has run)
    bool skid_sound = false;   // car_update_skid / crash state 5: the skid noise (sfx 0) plays
    void update_bumpers();
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
    void engine_gearbox(CarInput& in);
    void update_throttle(const CarInput& in);
    void update_brake(const CarInput& in);
    void update_steering(const CarInput& in);
    void wheel_loads();
    void update_skid(const CarInput& in);
    void corner_probes(const Track& track);
    void resolve_corner_probes(const Track& track);
    void apply_wall_push(const Track& track);
    void wheel_ground(const Track& track);
    void update_track_heading(const Track& track);
    void body_from_wheels();
    void smoke_spawn();  // car_update_debris
    void update_spin();  // car_update_spin
    bool smoke_cue_ = false;  // car_update_skid's call of car_update_debris
    void smoke_update(); // car_sync_debris_objects
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

// The player's views (camera_handle_view_keys and its table at 0x101da0), F1..F4:
//   0 F1 bumper: camera_mode_chase (on the car, camera_hi up), full screen, compact gauges
//   1 F2 cockpit: the same camera over a 132-line view and the car's dashboard (COCKnn)
//   2 F3 chase: camera_mode_car 1 (1300 behind, 300 up), compact gauges - the default
//   3 F4 far chase: camera_mode_car 0 (1700 behind)
// Switching blends from the old camera to the new one (camera_interpolate); the view size and
// the HUD change once the blend has settled.
class ViewCamera {
public:
    void select(int view);  // starts a blend
    void set(int view);     // no blend (the start of a race)
    void update(const Car& car, int camera_hi, float frame_time, int pow_row);

    int view = 2;
    int hud_view = 2;        // 1 dashboard, 2 compact gauges (applied when the blend ends)
    int viewport = 200;      // rows of 3D view at 320x200 (132 behind the dashboard)
    bool car_visible = true;
    float pitch = 0, yaw = 0, roll = 0;  // render camera (blend included)
    int x = 0, y = 0, z = 0;

private:
    void base(const Car& car, int camera_hi, float frame_time, int pow_row, float& p, float& yw, float& r,
              int& cx, int& cy, int& cz);
    ChaseCamera chase_;
    bool blending_ = false, start_blend_ = false;
    int old_view_ = 2;
    float off_angle_[3]{};
    int off_pos_[3]{};
    int pending_viewport_ = 200, pending_hud_ = 2;
};

} // namespace scr

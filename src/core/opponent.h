#pragma once
// CPU cars ("opponents"), ported from STARTL.EXE's opponent_* functions, and the race loop
// that runs them with the player car in race_frame's order.
//
// Data (DATA/MAPn.DAT):
//   MAPnOPP.SPL  racing lines, one per grid slot: repeated { int32 count, int32 start segment,
//                count * 0x38-byte segments }. Segment: +0x04 x, +0x08 z (knot; the grid
//                position of the start segment), +0x0c point index, +0x10 next segment,
//                +0x14/+0x18 control point A (x, z), +0x20/+0x24 control point B (x, z).
//                The line is a quadratic B-spline (spline_eval_2d): the first half of a
//                segment uses A, B, next.A; the second half B, next.A, next.B.
//   MAPn.MRK     int32 count, then 32-byte markers { int32 radius, flags, float speed factor,
//                ?, ?, int32 x, ?, int32 z } around the lap (speed factor scales the CPU cars'
//                speeds in that part of the track; flags pick the dust render flag).
//   MAPn[A-E].DTR OPPONENTS: max_spd, accel, brakes, skill, sband per CPU car (A..E is the
//                difficulty/championship level).
// The player starts on the racing line after the opponents' (index = number of opponents).

#include "core/car.h"
#include "core/track.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scr {

struct SplineSegment {
    int x = 0, z = 0;
    int point = 0;
    int next = 0;
    int ax = 0, az = 0;
    int bx = 0, bz = 0;
};

struct RacingLine {
    int start = 0;
    std::vector<SplineSegment> segments;
};

struct TrackMarker {
    int radius = 0;
    int flags = 0;               // +0x04 surface: 0 road, 1/2 picks dust, window reflections
    float speed_factor = 1.0f;   // +0x08 CPU car speed factor
    float grip = 1.0f;           // +0x0c the player's surface grip
    int checkpoint = 0;          // +0x10 bit 1: checkpoint that adds time (and starts timing)
    int x = 0, z = 0;
};

struct OpponentDef {
    float max_spd = 0, accel = 0, brakes = 0, skill = 0, sband = 0;
};

std::vector<RacingLine> parse_splines(const std::vector<uint8_t>& spl);
std::vector<TrackMarker> parse_markers(const std::vector<uint8_t>& mrk);

class Opponent {
public:
    int index = 0;
    OpponentDef def;
    CarSetup setup;          // for the body radius and paint
    int spline_index = 0;
    int segment = 0;
    float t = 0;             // position in the current half segment (0..1)
    int step = 0;            // which half of the segment
    float p[6]{};            // current control points p0, p1, p2 (x, z)
    float eval_x = 0, eval_z = 0, dir_x = 0, dir_z = 0;  // spline_eval_2d_ahead
    float ctrl_a_x = 0, ctrl_a_z = 0;
    int line_x = 0, line_z = 0;
    int line_point = 0;
    float line_yaw = 0, line_yaw_prev = 0;
    float speed = 0, prev_speed = 0;
    float speed_factor = 1.0f;
    bool catching_up = false;
    int lateral_x = 0, lateral_z = 0, prev_lateral_x = 0, prev_lateral_z = 0;
    float lateral_push = 0, lateral_dir_x = 0, lateral_dir_z = 0, lateral_timer = 0;
    int pos_x = 0, pos_y = 0, pos_z = 0;
    float pitch = 0, yaw = 0, roll = 0, draw_yaw = 0;
    float yaw_wobble = 0, pitch_ofs = 0, roll_ofs = 0;
    int ground_y = 0;
    float ground_pitch = 0, ground_roll = 0;
    int crash_state = 0;
    float crash_bounce = 0;
    int crash_bounce_count = 0;
    float crash_pitch_rate = 0, crash_yaw_rate = 0, crash_roll_rate = 0;
    float crash_pitch = 0, crash_yaw = 0, crash_roll = 0;
    int track_marker = 0, marker_flags = 0, lap = 0;
    float track_progress = 0;
    int race_position = 0;
    bool active = true;
    float viewer_distance = 0;
    int front_local_z = 0, rear_local_z = 0, collision_radius = 0;
    int front_x = 0, front_z = 0, rear_x = 0, rear_z = 0;
    uint32_t nearby_mask = 0;
    uint32_t render_flags = 0;
};

// The race: the player car plus the CPU cars on one track.
class Race {
public:
    // Loads MAPnOPP.SPL, MAPn.MRK and OPPONENTS from DATA/MAPn.DAT (level letter 'A'..'E').
    // A new game plays level 'B' (DAT_0011d2b0 = 1).
    bool load(const std::string& data_dat, const std::string& track_name, char level = 'B');
    int bonus_time = 0;          // TRACK_DATA.bonus_time: seconds added at a timed checkpoint
    int camera_hi = 300;         // TRACK_DATA.camera_hi: bumper camera height
    int laps = 3;                // DAT_0011d2ac + 1

    // Takes the state from copies of the game's g_player_car and g_opponents[count] (0x590 bytes
    // each), for comparing frames with recordings of the original.
    void load_state(const uint8_t* player_car, const uint8_t* cpu_cars, size_t count);

    // Grid: the player on the racing line after the opponents', CPU cars on theirs.
    // opponent_setups[i] is the CarSetup of CPU car i (from its CARS_DATA record and model).
    void start(const Track& track, const CarSetup& player_setup, const std::vector<CarSetup>& opponent_setups);

    // race_frame: the player and the CPU cars, then the collisions between them.
    void update(const Track& track, const CarInput& in, float frame_time, int pow_row);

    Car player;
    // Race state for the player (car_update_race_timers, car_on_checkpoint, car_complete_lap).
    bool started = true;         // g_race_flags & 1
    bool finished = false;       // g_race_flags & 8
    bool failed = false;         // g_race_flags & 0x10: out of time and stopped
    bool use_countdown = false;  // start() turns on the 3-2-1 countdown (race_countdown_update)
    float countdown_timer = 0;
    int countdown_stage = -1;    // start light shown, -1 when the countdown is over
    bool timing_started = false;
    float time_left = 15.0f;     // starts at 15 s, a timed checkpoint gives time_left / 5 + bonus
    float total_time = 0;        // PlayerCar 0x78c
    float lap_time = 0;          // PlayerCar 0x794
    float best_lap = 10000.0f;
    std::string best_lap_text = "00:00:00";
    std::vector<int> checkpoints_seen;
    int checkpoints_passed = 0;
    std::vector<Opponent> opponents;
    std::vector<RacingLine> lines;
    std::vector<TrackMarker> markers;
    std::vector<OpponentDef> defs;
    int player_position = 1;

private:
    float lerp(float value, float target, int k) const;
    void next_spline_segment(Opponent& o);
    void advance_on_spline(Opponent& o);
    void update_position(Opponent& o);
    void update_target_speed(Opponent& o);
    void smooth_angles(Opponent& o);
    void crash_update(Opponent& o);
    void track_progress(Opponent& o);
    void bumpers(Opponent& o);
    void check_ground(const Track& track, Opponent& o);
    void cpu_contacts(Opponent& o);
    void collide_player(Opponent& o);
    void player_contacts(const Track& track);
    void update_positions();
    void player_progress();

    float dt_ = 0.02f;
    int pow_row_ = 49;
    bool positions_running_ = false;
};

} // namespace scr

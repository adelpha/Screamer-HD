#pragma once
// Replays (replay_save / replay_load / replay_read_frame / replay_write_frame): the game
// records the frame time and the driving inputs of every frame and plays a race back by
// running it again from them.
//
// .RPL file: the control setup as CONFIG/CONTROLS.CFG has it (core/controls.h: 6 bindings,
// joystick calibration, ports; 0x530 bytes), then a 0x18-byte header { int frames, int car
// (CARS_DATA record), int laps - 1, int level, int game mode, int race options } and 0x14
// bytes a frame: { float frame time, float raw frame time, int16 accel, brake, steer (left),
// steer right, gear up, gear down } (g_input_*).
//
// A replay plays back with its own setup (replay_load reads it over g_control_bindings): the
// bindings' directions and devices decide what the physics does with the values. Ours write
// the setup the race was driven with. Our controls can change device from one frame to the
// next (keys, then a stick), which one setup cannot say; a frame that differs from the file's
// setup is marked (our additions; the original never stores such values, its inputs stay
// within -1023..1023):
//   steer right -32768        the steering this frame is a joystick axis (device 0)
//   steer right 0x4000 + R    two keys (device 1), the right one R
//   steer right 0x6000        an axis read as keys (device 2)
//   brake 0x4000 + v          a brake pedal (an axis, device 0): v / 1023 directly
//   brake 0x2000 + v          a brake key (ramped)
// Our replays from before had no setup (zeros): they get the keyboard setup, with the
// -32768 marks.

#include "core/car.h"
#include "core/controls.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace scr {

#pragma pack(push, 1)
struct ReplayFrame {
    float frame_time = 0.02f, raw_time = 0.02f;
    int16_t accel = 0, brake = 0, steer_left = 0, steer_right = 0, gear_up = 0, gear_down = 0;
};
#pragma pack(pop)
static_assert(sizeof(ReplayFrame) == 0x14);

struct Replay {
    static constexpr int kReversed = 0x100;  // in race_options: a reversed track (ours)
    static constexpr int16_t kSteerAxis = -32768, kSteerKeys = 0x4000, kSteerAxisKeys = 0x6000;
    static constexpr int16_t kBrakePedal = 0x4000, kBrakeKey = 0x2000;
    ControlsFile controls;   // the preamble
    bool has_setup = false;  // false: a replay of ours from before (all zeros)
    int car = 0;
    int laps = 3;          // stored as laps - 1
    int level = 1;
    int game_mode = 1;
    int race_options = 0;
    std::vector<ReplayFrame> frames;
    bool load(const std::string& path);
    bool save(const std::string& path) const;

    // replay_read_frame: frame's inputs into in (accel .. gear down and the setup).
    void read_frame(const ReplayFrame& f, CarInput& in) const;
    // replay_write_frame for a race driven with per-frame setups: picks the file's setup (the
    // one most frames used, from those passed to add_frame) and marks the other frames.
    void add_frame(const CarInput& in, float frame_time, float raw_time);
    void finish_recording(const ControlsFile& base);  // sets controls (and the frames' marks)

private:
    struct Pending {
        int steer_mode, brake_pedal;
    };
    std::vector<Pending> pending_;
};

} // namespace scr

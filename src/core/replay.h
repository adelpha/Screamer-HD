#pragma once
// Replays (replay_save / replay_load / replay_read_frame / replay_write_frame): the game
// records the frame time and the driving inputs of every frame and plays a race back by
// running it again from them.
//
// .RPL file: the control setup (6 x 0x1c bytes), key table (0x28 x 0x1c), 10 ints (all
// written as zeros here), then a 0x18-byte header { int frames, int car (CARS_DATA record),
// int laps - 1, int level, int game mode, int race options } and 0x14 bytes a frame:
// { float frame time, float raw frame time, int16 accel, brake, steer (left), steer right,
// gear up, gear down }. An analog steer is kept as its value in the steer field with -32768
// in "steer right" (our addition; the original stored the joystick the same way in steer).

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
    int car = 0;
    int laps = 3;          // stored as laps - 1
    int level = 1;
    int game_mode = 1;
    int race_options = 0;
    std::vector<ReplayFrame> frames;
    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

} // namespace scr

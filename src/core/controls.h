#pragma once
// The original's control setup (SETUP.EXE -> CONFIG/CONTROLS.CFG, read_controls_cfg) and
// controls_update, which turns it into the g_input_* values the car physics reads.
//
// CONTROLS.CFG (and the first 0x530 bytes of every .RPL, see core/replay.h):
//   6 bindings of 0x1c bytes (g_control_bindings at 0x1197d4): accelerate, brake, steer
//     (left), steer right, gear up, gear down
//   0x28 x 0x1c bytes of joystick calibration (DAT_001192a0, joystick * 0x1c + axis * 0x118)
//   10 ints (DAT_00119700): the joystick ports
// A binding is 7 ints:
//   [0] enabled (controls_update skips the others)
//   [1] 1: a joystick axis, else a joystick button
//   [2] device: 0 joystick, 2 joystick axis read as a key (past half way: 1023), any other
//       value the keyboard (SETUP writes 1)
//   [3] joystick, [4] axis
//   [5] direction: the sign the value must have for the physics to take it (car_update_throttle,
//       car_update_brake, the gear changes); unused for steering
//   [6] key: the keyboard's scancode (set 1, without the E0 prefix: Up is 0x48) or the
//       button mask
//
// The physics reads the bindings themselves too, not only the values:
//   accel   used only when sign(g_input_accel) == accel.dir (0x1197e8), throttle |v| / 1023
//   brake   only when sign == brake.dir (0x119804); brake.device == 0 && brake.axis_flag == 1
//           (0x1197f8, 0x1197f4): the pedal sets the brake to |v| / 1023, else it ramps to 1
//   steer   by steer.device (0x119814): 0 the value is the angle (snapped while gripping),
//           1 two keys (steer - steer right, ramped), 2 the value ramped; others: no change
//   gears   only when sign == gear_up.dir (0x119858) / gear_down.dir (0x119874)
// The coasting brake (out of time or past the finish) is brake.dir << 9.

#include <array>
#include <cstdint>
#include <string>

namespace scr {

struct ControlBinding {
    int32_t enabled = 1;
    int32_t axis = 0;    // [1] 1: joystick axis
    int32_t device = 1;  // [2] 0 joystick, 2 axis as key, else keyboard
    int32_t joystick = 0;
    int32_t axis_index = 0;
    int32_t dir = 1;     // [5]
    int32_t key = 0;     // [6] scancode or button mask
    bool operator==(const ControlBinding&) const = default;
};
static_assert(sizeof(ControlBinding) == 0x1c);

enum ControlAction : int { kActAccel, kActBrake, kActSteer, kActSteerRight, kActGearUp, kActGearDown, kActCount };

struct ControlSetup {
    std::array<ControlBinding, 6> b{};
    // DEFCTRL.CFG: Up, Down, Left, Right, Alt, Ctrl on the keyboard
    ControlSetup();
    bool operator==(const ControlSetup&) const = default;

    // What the physics reads
    int steer_mode() const { return b[kActSteer].device; }
    bool brake_pedal() const { return b[kActBrake].device == 0 && b[kActBrake].axis == 1; }
    static int sign(int v) { return v < 0 ? -1 : 1; }  // (the original's: 0 counts as +1)
};

// The whole file: bindings, calibration and ports (0x530 bytes).
struct ControlsFile {
    static constexpr size_t kSize = 6 * 0x1c + 0x28 * 0x1c + 10 * 4;
    ControlSetup setup;
    std::array<uint8_t, 0x28 * 0x1c> calibration{};
    std::array<int32_t, 10> ports{};
    bool parse(const uint8_t* data, size_t size);  // false: too short
    std::array<uint8_t, kSize> bytes() const;
    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

// controls_update's treatment of one axis reading (device 0 or 2): clamped to +-1023, device 2
// past half way to +-1023, below 0x66 nothing.
int controls_axis_value(int raw, int device);

// controls_update for one frame. raw: per binding, what its device gives this frame: a key or
// button 0 / 1, an axis -1023..1023 after calibration (the value before the clamp; the
// original's is rounded from the calibrated reading). Disabled bindings leave their g_input_*
// as it is (in: the last frame's).
// Bindings on the same joystick axis as an earlier one copy its value (accel and brake on one
// axis, each taking its direction).
void controls_update(const ControlSetup& s, const std::array<int, 6>& raw, std::array<int, 6>& inputs);

} // namespace scr

#pragma once
// The player's controls (ours): keyboard keys, gamepads (SDL's mapped controllers) and other
// joysticks (wheels, pedals, sticks), bound to the game's actions, saved in CONTROLS.INI next
// to CHOICE.DAT. Every frame they become the original's inputs (scr::CarInput: g_input_* and
// the control setup the physics reads), each time in one of the original's modes:
//   keys and buttons   1023, keyboard device (brake ramped, steering two keys)
//   an analog axis     -1023..1023 through controls_update's clamp and dead zone, joystick
//                      device (a pedal sets the brake directly, the stick is the angle)
//   a digital axis     the same read as a key (device 2: past half way 1023)
// When several are held, keys win over digital axes over analog ones. With nothing held the
// steering stays in the mode last used (a stick snaps back to the middle as the original's
// joystick did; keys ease back).
//
// Menus and the race's questions always answer to the default keys (arrows, Enter, Esc) and
// the default pad buttons (D-pad / left stick, A, B, Start), whatever the bindings.
// Rumble (optional) follows what the car did; nothing flows back into the simulation.

#include "core/car.h"
#include "core/controls.h"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace scr::input {

enum Action : int { kAccel, kBrake, kSteerLeft, kSteerRight, kGearUp, kGearDown, kLookBack, kActionCount };
const char* action_name(int a);   // "ACCELERATE" ..
const char* action_short(int a);  // "ACCEL" .. (the menu's lines)

struct Bind {
    enum Kind : uint8_t { kNone, kKey, kPadButton, kPadAxis, kJoyButton, kJoyAxis, kJoyHat };
    Kind kind = kNone;
    int code = 0;          // SDL_Scancode, SDL_GamepadButton, SDL_GamepadAxis, joystick button / axis / hat
    int dir = 1;           // axis: +1 / -1 (which way counts); hat: SDL_HAT_* mask
    bool full = false;     // axis resting at one end (a pedal): its whole travel is 0..1023
    bool digital = false;  // axis read as a key (the original's device 2)
    std::string guid;      // joystick binds: the device's GUID
    bool operator==(const Bind&) const = default;
    bool is_axis() const { return kind == kPadAxis || kind == kJoyAxis; }
    std::string name() const;  // "UP", "PAGE UP", "PAD A", "PAD LX-", "JOY AXIS 2+", ...
};

constexpr int kKeySlots = 2, kPadSlots = 2;
struct Bindings {
    std::array<std::array<Bind, kKeySlots>, kActionCount> keys{};
    std::array<std::array<Bind, kPadSlots>, kActionCount> pad{};
    bool rumble = true;
    std::string pad_guid;  // the controller that drives ("": any of them)
    bool operator==(const Bindings&) const = default;
    static Bindings defaults();
    // The rebinding screen: b becomes the action's first key (or pad binding), the first moves
    // to second place; caught again, the same one stays alone. Other actions lose it.
    void assign(int action, bool pad, const Bind& b);
    void toggle_digital(int action);  // the action's pad axes: analog <-> digital
    // Keys of the original's CONFIG/CONTROLS.CFG (keyboard bindings only) as the first key of
    // each action; our default stays as the second.
    void import_original(const ControlsFile& f);
    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

// DOS keyboard scancodes (set 1, as g_key_state is indexed) and SDL's.
SDL_Scancode scancode_from_dos(int code);
int scancode_to_dos(SDL_Scancode sc);  // 0: none

// What a capture (the rebinding screen) caught.
struct Captured {
    bool done = false, cancelled = false;
    Bind bind;
};

class Input {
public:
    // Opens what is plugged in (SDL_INIT_GAMEPAD; nothing in headless runs).
    void init();
    void shutdown();
    // Device events (added / removed) and the capture's keys and buttons. Returns true when
    // the event was taken by a capture (the game must not act on it).
    bool handle_event(const SDL_Event& ev);

    Bindings bindings;
    ControlsFile original;      // the game's CONTROLS.CFG (calibration and ports for our replays)
    // The setup our replays carry (Replay::finish_recording sets the devices): the keyboard
    // keys of the bindings as the original's scancodes, the game's calibration and ports.
    ControlsFile replay_controls() const;

    // One frame of the race's controls (hold: --hold's scripted ones, ORed in).
    CarInput drive(const std::string& hold);
    bool look_back() const;
    // The menus' keys: arrows / D-pad / left stick, Enter / A, Esc / B; start: Start.
    struct Nav {
        bool up = false, down = false, left = false, right = false, enter = false, esc = false, start = false;
    };
    Nav nav() const;     // held now (keyboard and pads)
    Nav pad_nav() const; // pads only
    bool any_pad_button() const;  // A, Start or the right trigger (the intro demo)
    // The main menu's way out on a pad: Back, or on a pad without Back, B held (main times it).
    bool back_held() const;
    bool b_held_without_back() const;

    // Rumble from what the car did this step (off when the option is off).
    void rumble(const Car& car, bool racing);
    void stop_rumble();

    // Rebinding: start waiting for a key (keyboard) or a pad button / axis (pad); poll() gives
    // the result once something was pressed or Esc (pad: Back) cancelled.
    void start_capture(bool pad);
    bool capturing() const { return capture_ != 0; }
    Captured poll_capture();

    // The connected controllers (gamepads and joysticks) by name, with their GUIDs.
    struct Device {
        SDL_JoystickID id = 0;
        SDL_Gamepad* pad = nullptr;
        SDL_Joystick* joy = nullptr;  // (a gamepad's own joystick too)
        std::string name, guid;
    };
    const std::vector<Device>& devices() const { return devices_; }

private:
    void open(SDL_JoystickID id);
    void close(SDL_JoystickID id);
    bool allowed(const Device& d) const;  // the chosen controller (or any)
    int bind_value(const Bind& b) const;  // 0..1023 for this frame (keys and buttons 1023)
    bool bind_held(const Bind& b) const;
    std::vector<Device> devices_;
    int steer_mode_ = 1;   // the mode last used (sticky)
    int capture_ = 0;      // 1 keyboard, 2 pad
    Captured captured_;
    std::vector<std::vector<int16_t>> capture_rest_;  // per device, its axes when the capture began
    uint64_t rumble_until_ = 0;
    float rumble_lo_ = 0, rumble_hi_ = 0;
};

} // namespace scr::input

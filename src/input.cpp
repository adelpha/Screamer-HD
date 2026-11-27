#include "input.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace scr::input {

namespace {

// DOS set-1 scancodes 0x00..0x58 (extended keys share their codes: the game's keyboard handler
// drops the E0 prefix, so 0x48 is Up and keypad 8 alike; arrows and the navigation keys here).
const SDL_Scancode kDosToSdl[0x59] = {
    SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4,
    SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9, SDL_SCANCODE_0,
    SDL_SCANCODE_MINUS, SDL_SCANCODE_EQUALS, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_TAB, SDL_SCANCODE_Q,
    SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R, SDL_SCANCODE_T, SDL_SCANCODE_Y, SDL_SCANCODE_U, SDL_SCANCODE_I,
    SDL_SCANCODE_O, SDL_SCANCODE_P, SDL_SCANCODE_LEFTBRACKET, SDL_SCANCODE_RIGHTBRACKET, SDL_SCANCODE_RETURN,
    SDL_SCANCODE_LCTRL, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_F, SDL_SCANCODE_G,
    SDL_SCANCODE_H, SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L, SDL_SCANCODE_SEMICOLON,
    SDL_SCANCODE_APOSTROPHE, SDL_SCANCODE_GRAVE, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_BACKSLASH, SDL_SCANCODE_Z,
    SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_B, SDL_SCANCODE_N, SDL_SCANCODE_M,
    SDL_SCANCODE_COMMA, SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_KP_MULTIPLY,
    SDL_SCANCODE_LALT, SDL_SCANCODE_SPACE, SDL_SCANCODE_CAPSLOCK, SDL_SCANCODE_F1, SDL_SCANCODE_F2,
    SDL_SCANCODE_F3, SDL_SCANCODE_F4, SDL_SCANCODE_F5, SDL_SCANCODE_F6, SDL_SCANCODE_F7, SDL_SCANCODE_F8,
    SDL_SCANCODE_F9, SDL_SCANCODE_F10, SDL_SCANCODE_NUMLOCKCLEAR, SDL_SCANCODE_SCROLLLOCK, SDL_SCANCODE_HOME,
    SDL_SCANCODE_UP, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_KP_MINUS, SDL_SCANCODE_LEFT, SDL_SCANCODE_KP_5,
    SDL_SCANCODE_RIGHT, SDL_SCANCODE_KP_PLUS, SDL_SCANCODE_END, SDL_SCANCODE_DOWN, SDL_SCANCODE_PAGEDOWN,
    SDL_SCANCODE_INSERT, SDL_SCANCODE_DELETE, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_NONUSBACKSLASH, SDL_SCANCODE_F11, SDL_SCANCODE_F12,
};

const char* const kActionNames[kActionCount] = {"ACCELERATE", "BRAKE", "STEER LEFT", "STEER RIGHT",
                                                "GEAR UP", "GEAR DOWN", "LOOK BACK"};
const char* const kActionKeys[kActionCount] = {"accel", "brake", "left", "right", "gearup", "geardown", "lookback"};
const char* const kKinds[] = {"none", "key", "padbutton", "padaxis", "joybutton", "joyaxis", "joyhat"};

Bind key(SDL_Scancode sc) {
    Bind b;
    b.kind = Bind::kKey;
    b.code = int(sc);
    return b;
}
Bind pad_button(SDL_GamepadButton btn) {
    Bind b;
    b.kind = Bind::kPadButton;
    b.code = int(btn);
    return b;
}
Bind pad_axis(SDL_GamepadAxis ax, int dir) {
    Bind b;
    b.kind = Bind::kPadAxis;
    b.code = int(ax);
    b.dir = dir;
    return b;
}

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// An axis reading (-32768..32767) as how far it is pushed the bound way, 0..1023.
int axis_amount(int raw, int dir, bool full) {
    const long v = long(raw) * long(dir < 0 ? -1 : 1);
    if (full) return int(std::clamp((v + 32768L) * 1023L / 65535L, 0L, 1023L));
    return v <= 0 ? 0 : int(std::min(1023L, v * 1023L / 32767L));
}

constexpr int kStickNav = 16000;     // the left stick as the D-pad in the menus
constexpr int kCaptureAxis = 16000;  // an axis moved this far from where it rested binds it

} // namespace

const char* action_name(int a) { return a >= 0 && a < kActionCount ? kActionNames[a] : ""; }
const char* action_short(int a) {
    static const char* const kShort[kActionCount] = {"ACCEL", "BRAKE", "LEFT", "RIGHT", "GEAR UP", "GEAR DOWN", "LOOK BACK"};
    return a >= 0 && a < kActionCount ? kShort[a] : "";
}

namespace {
bool same_input(const Bind& a, const Bind& b) {
    return a.kind != Bind::kNone && a.kind == b.kind && a.code == b.code && a.guid == b.guid &&
           (a.kind == Bind::kKey || a.kind == Bind::kPadButton || a.kind == Bind::kJoyButton || a.dir == b.dir);
}
} // namespace

void Bindings::assign(int action, bool on_pad, const Bind& b) {
    if (action < 0 || action >= kActionCount || b.kind == Bind::kNone) return;
    auto group = [&](int a) -> std::array<Bind, 2>& { return on_pad ? pad[size_t(a)] : keys[size_t(a)]; };
    for (int a = 0; a < kActionCount; ++a) {
        if (a == action) continue;
        auto& g = group(a);
        if (same_input(g[1], b)) g[1] = {};
        if (same_input(g[0], b)) {
            g[0] = g[1];
            g[1] = {};
        }
    }
    auto& g = group(action);
    if (same_input(g[0], b)) {
        g[1] = {};
    } else {
        g[1] = g[0];  // (the second, if it was this one, gives way)
        g[0] = b;
    }
}

void Bindings::toggle_digital(int action) {
    if (action < 0 || action >= kActionCount) return;
    for (Bind& b : pad[size_t(action)])
        if (b.is_axis()) b.digital = !b.digital;
}

std::string Bind::name() const {
    static const char* const kButtons[] = {"A", "B", "X", "Y", "BACK", "GUIDE", "START", "LS", "RS", "LB", "RB",
                                           "DPAD UP", "DPAD DOWN", "DPAD LEFT", "DPAD RIGHT", "MISC", "PADDLE 1",
                                           "PADDLE 2", "PADDLE 3", "PADDLE 4", "TOUCHPAD"};
    static const char* const kAxes[] = {"LX", "LY", "RX", "RY", "LT", "RT"};
    char buf[64];
    switch (kind) {
    case kKey: {
        const char* n = SDL_GetScancodeName(SDL_Scancode(code));
        return n && *n ? upper(n) : "KEY " + std::to_string(code);
    }
    case kPadButton:
        return std::string("PAD ") + (code >= 0 && code < int(std::size(kButtons)) ? kButtons[code] : std::to_string(code).c_str());
    case kPadAxis: {
        std::string s = std::string("PAD ") + (code >= 0 && code < int(std::size(kAxes)) ? kAxes[code] : "AXIS");
        if (code < SDL_GAMEPAD_AXIS_LEFT_TRIGGER || dir < 0) s += dir < 0 ? "-" : "+";
        return s + (digital ? " DIG" : "");
    }
    case kJoyButton: std::snprintf(buf, sizeof buf, "JOY BUTTON %d", code + 1); return buf;
    case kJoyAxis:
        std::snprintf(buf, sizeof buf, "JOY AXIS %d%s%s", code + 1, dir < 0 ? "-" : "+", digital ? " DIG" : "");
        return buf;
    case kJoyHat: {
        const char* d = dir & SDL_HAT_UP ? "UP" : dir & SDL_HAT_DOWN ? "DOWN" : dir & SDL_HAT_LEFT ? "LEFT" : "RIGHT";
        std::snprintf(buf, sizeof buf, "JOY HAT %s", d);
        return buf;
    }
    default: return "-";
    }
}

Bindings Bindings::defaults() {
    Bindings b;
    b.keys[kAccel] = {key(SDL_SCANCODE_UP), {}};
    b.keys[kBrake] = {key(SDL_SCANCODE_SPACE), key(SDL_SCANCODE_DOWN)};
    b.keys[kSteerLeft] = {key(SDL_SCANCODE_LEFT), {}};
    b.keys[kSteerRight] = {key(SDL_SCANCODE_RIGHT), {}};
    b.keys[kGearUp] = {key(SDL_SCANCODE_PAGEUP), {}};
    b.keys[kGearDown] = {key(SDL_SCANCODE_PAGEDOWN), {}};
    b.keys[kLookBack] = {key(SDL_SCANCODE_B), {}};
    b.pad[kAccel] = {pad_axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1), {}};
    b.pad[kBrake] = {pad_axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1), {}};
    b.pad[kSteerLeft] = {pad_axis(SDL_GAMEPAD_AXIS_LEFTX, -1), pad_button(SDL_GAMEPAD_BUTTON_DPAD_LEFT)};
    b.pad[kSteerRight] = {pad_axis(SDL_GAMEPAD_AXIS_LEFTX, 1), pad_button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)};
    b.pad[kGearUp] = {pad_button(SDL_GAMEPAD_BUTTON_NORTH), {}};
    b.pad[kGearDown] = {pad_button(SDL_GAMEPAD_BUTTON_WEST), {}};
    b.pad[kLookBack] = {pad_button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), {}};
    return b;
}

void Bindings::import_original(const ControlsFile& f) {
    for (int a = 0; a < kActCount; ++a) {
        const ControlBinding& c = f.setup.b[size_t(a)];
        if (c.enabled != 1 || c.device == 0 || c.device == 2) continue;  // (joysticks: not these)
        const SDL_Scancode sc = scancode_from_dos(c.key);
        if (sc == SDL_SCANCODE_UNKNOWN) continue;
        auto& slots = keys[size_t(a)];  // the original's action order is ours up to gear down
        if (slots[0].kind == Bind::kKey && slots[0].code == int(sc)) continue;
        if (slots[1].kind == Bind::kKey && slots[1].code == int(sc)) {
            std::swap(slots[0], slots[1]);
            continue;
        }
        slots[1] = slots[0];
        slots[0] = key(sc);
    }
}

bool Bindings::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "rumble") {
            rumble = v != "0";
            continue;
        }
        if (k == "pad") {
            pad_guid = v;
            continue;
        }
        const size_t dot = k.find('.');
        if (dot == std::string::npos) continue;
        const std::string act = k.substr(0, dot), slot = k.substr(dot + 1);
        int a = -1;
        for (int i = 0; i < kActionCount; ++i)
            if (act == kActionKeys[i]) a = i;
        if (a < 0) continue;
        Bind* b = nullptr;
        if (slot == "key1") b = &keys[size_t(a)][0];
        else if (slot == "key2") b = &keys[size_t(a)][1];
        else if (slot == "pad1") b = &pad[size_t(a)][0];
        else if (slot == "pad2") b = &pad[size_t(a)][1];
        if (!b) continue;
        std::istringstream in(v);
        std::string kind;
        int full = 0, digital = 0;
        Bind nb;
        in >> kind >> nb.code >> nb.dir >> full >> digital >> nb.guid;
        for (size_t i = 0; i < std::size(kKinds); ++i)
            if (kind == kKinds[i]) nb.kind = Bind::Kind(i);
        nb.full = full != 0;
        nb.digital = digital != 0;
        *b = nb;
    }
    return true;
}

bool Bindings::save(const std::string& path) const {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "# Screamer controls (ours): action.slot=kind code direction full digital [joystick guid]\n";
    f << "rumble=" << (rumble ? 1 : 0) << "\n";
    f << "pad=" << pad_guid << "\n";
    auto put = [&](int a, const char* slot, const Bind& b) {
        f << kActionKeys[a] << "." << slot << "=" << kKinds[b.kind] << " " << b.code << " " << b.dir << " "
          << (b.full ? 1 : 0) << " " << (b.digital ? 1 : 0);
        if (!b.guid.empty()) f << " " << b.guid;
        f << "\n";
    };
    for (int a = 0; a < kActionCount; ++a) {
        put(a, "key1", keys[size_t(a)][0]);
        put(a, "key2", keys[size_t(a)][1]);
        put(a, "pad1", pad[size_t(a)][0]);
        put(a, "pad2", pad[size_t(a)][1]);
    }
    return bool(f);
}

SDL_Scancode scancode_from_dos(int code) {
    return code > 0 && code < int(std::size(kDosToSdl)) ? kDosToSdl[code] : SDL_SCANCODE_UNKNOWN;
}

int scancode_to_dos(SDL_Scancode sc) {
    for (int i = 1; i < int(std::size(kDosToSdl)); ++i)
        if (kDosToSdl[i] == sc) return i;
    switch (sc) {  // the keys with an E0 prefix (and the keypad's others), as the game sees them
    case SDL_SCANCODE_RCTRL: return 0x1d;
    case SDL_SCANCODE_RALT: return 0x38;
    case SDL_SCANCODE_KP_ENTER: return 0x1c;
    case SDL_SCANCODE_KP_DIVIDE: return 0x35;
    case SDL_SCANCODE_KP_7: return 0x47;
    case SDL_SCANCODE_KP_8: return 0x48;
    case SDL_SCANCODE_KP_9: return 0x49;
    case SDL_SCANCODE_KP_4: return 0x4b;
    case SDL_SCANCODE_KP_6: return 0x4d;
    case SDL_SCANCODE_KP_1: return 0x4f;
    case SDL_SCANCODE_KP_2: return 0x50;
    case SDL_SCANCODE_KP_3: return 0x51;
    case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_KP_PERIOD: return 0x53;
    default: return 0;
    }
}

// ---------------------------------------------------------------------------------------------

void Input::init() {
    if (!SDL_WasInit(SDL_INIT_GAMEPAD)) return;
    int n = 0;
    if (SDL_JoystickID* ids = SDL_GetJoysticks(&n)) {
        for (int i = 0; i < n; ++i) open(ids[i]);
        SDL_free(ids);
    }
}

void Input::shutdown() {
    stop_rumble();
    while (!devices_.empty()) close(devices_.back().id);
}

void Input::open(SDL_JoystickID id) {
    for (const Device& d : devices_)
        if (d.id == id) return;
    Device d;
    d.id = id;
    if (SDL_IsGamepad(id)) {
        d.pad = SDL_OpenGamepad(id);
        if (!d.pad) return;
        d.joy = SDL_GetGamepadJoystick(d.pad);
        const char* n = SDL_GetGamepadName(d.pad);
        d.name = n ? n : "GAMEPAD";
    } else {
        d.joy = SDL_OpenJoystick(id);
        if (!d.joy) return;
        const char* n = SDL_GetJoystickName(d.joy);
        d.name = n ? n : "JOYSTICK";
    }
    char guid[64] = "";
    SDL_GUIDToString(SDL_GetJoystickGUID(d.joy), guid, sizeof guid);
    d.guid = guid;
    SDL_Log("controller connected: %s%s", d.name.c_str(), d.pad ? "" : " (joystick)");
    devices_.push_back(d);
}

void Input::close(SDL_JoystickID id) {
    for (size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i].id != id) continue;
        SDL_Log("controller removed: %s", devices_[i].name.c_str());
        if (devices_[i].pad) SDL_CloseGamepad(devices_[i].pad);
        else if (devices_[i].joy) SDL_CloseJoystick(devices_[i].joy);
        devices_.erase(devices_.begin() + std::ptrdiff_t(i));
        return;
    }
}

bool Input::allowed(const Device& d) const { return bindings.pad_guid.empty() || d.guid == bindings.pad_guid; }

bool Input::handle_event(const SDL_Event& ev) {
    switch (ev.type) {
    case SDL_EVENT_GAMEPAD_ADDED: open(ev.gdevice.which); return false;
    case SDL_EVENT_JOYSTICK_ADDED:
        if (!SDL_IsGamepad(ev.jdevice.which)) open(ev.jdevice.which);
        return false;
    case SDL_EVENT_GAMEPAD_REMOVED: close(ev.gdevice.which); return false;
    case SDL_EVENT_JOYSTICK_REMOVED: close(ev.jdevice.which); return false;
    default: break;
    }
    if (!capture_) return false;
    auto finish = [&](const Bind& b, bool cancelled) {
        captured_ = Captured{true, cancelled, b};
        capture_ = 0;
        return true;
    };
    switch (ev.type) {
    case SDL_EVENT_KEY_DOWN:
        if (ev.key.repeat) return true;
        if (ev.key.scancode == SDL_SCANCODE_ESCAPE) return finish({}, true);
        if (capture_ == 1) {
            // the race's own keys (views, radar, the car stereo, the F keys) are not offered
            const SDL_Scancode sc = ev.key.scancode;
            if ((sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) || sc == SDL_SCANCODE_P || sc == SDL_SCANCODE_M)
                return true;
            return finish(key(sc), false);
        }
        return true;
    case SDL_EVENT_KEY_UP: return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (capture_ != 2) return true;
        if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) return finish({}, true);
        return finish(pad_button(SDL_GamepadButton(ev.gbutton.button)), false);
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
        if (capture_ != 2 || SDL_IsGamepad(ev.jbutton.which)) return capture_ == 2;
        for (const Device& d : devices_)
            if (d.id == ev.jbutton.which) {
                Bind b;
                b.kind = Bind::kJoyButton;
                b.code = ev.jbutton.button;
                b.guid = d.guid;
                return finish(b, false);
            }
        return true;
    case SDL_EVENT_JOYSTICK_HAT_MOTION:
        if (capture_ != 2 || SDL_IsGamepad(ev.jhat.which) || ev.jhat.value == SDL_HAT_CENTERED) return capture_ == 2;
        for (const Device& d : devices_)
            if (d.id == ev.jhat.which) {
                Bind b;
                b.kind = Bind::kJoyHat;
                b.code = ev.jhat.hat;
                // one direction (a diagonal: its vertical part)
                b.dir = ev.jhat.value & (SDL_HAT_UP | SDL_HAT_DOWN) ? ev.jhat.value & (SDL_HAT_UP | SDL_HAT_DOWN) : ev.jhat.value;
                b.guid = d.guid;
                return finish(b, false);
            }
        return true;
    default: return false;
    }
}

void Input::start_capture(bool pad) {
    capture_ = pad ? 2 : 1;
    captured_ = {};
    capture_rest_.clear();
    for (const Device& d : devices_) {
        std::vector<int16_t> rest;
        if (d.pad) {
            for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) rest.push_back(SDL_GetGamepadAxis(d.pad, SDL_GamepadAxis(a)));
        } else {
            for (int a = 0; a < SDL_GetNumJoystickAxes(d.joy); ++a) rest.push_back(SDL_GetJoystickAxis(d.joy, a));
        }
        capture_rest_.push_back(std::move(rest));
    }
}

Captured Input::poll_capture() {
    if (capture_ == 2 && capture_rest_.size() == devices_.size()) {
        for (size_t i = 0; i < devices_.size() && capture_; ++i) {
            const Device& d = devices_[i];
            const auto& rest = capture_rest_[i];
            for (size_t a = 0; a < rest.size(); ++a) {
                const int v = d.pad ? SDL_GetGamepadAxis(d.pad, SDL_GamepadAxis(a)) : SDL_GetJoystickAxis(d.joy, int(a));
                if (std::abs(v - rest[a]) < kCaptureAxis) continue;
                Bind b;
                b.kind = d.pad ? Bind::kPadAxis : Bind::kJoyAxis;
                b.code = int(a);
                b.dir = v > rest[a] ? 1 : -1;
                // resting at one end (pedals, and triggers seen as plain axes): the whole travel
                b.full = !d.pad && std::abs(int(rest[a])) > 24000;
                if (!d.pad) b.guid = d.guid;
                captured_ = Captured{true, false, b};
                capture_ = 0;
                break;
            }
        }
    }
    Captured c = captured_;
    if (c.done) captured_ = {};
    return c;
}

bool Input::bind_held(const Bind& b) const { return bind_value(b) >= 0x200; }

int Input::bind_value(const Bind& b) const {
    switch (b.kind) {
    case Bind::kKey: {
        const bool* keys = SDL_GetKeyboardState(nullptr);
        return b.code > 0 && b.code < SDL_SCANCODE_COUNT && keys[b.code] ? 1023 : 0;
    }
    case Bind::kPadButton:
        for (const Device& d : devices_)
            if (d.pad && allowed(d) && SDL_GetGamepadButton(d.pad, SDL_GamepadButton(b.code))) return 1023;
        return 0;
    case Bind::kPadAxis: {
        int best = 0;
        for (const Device& d : devices_)
            if (d.pad && allowed(d))
                best = std::max(best, axis_amount(SDL_GetGamepadAxis(d.pad, SDL_GamepadAxis(b.code)), b.dir, b.full));
        return best;
    }
    case Bind::kJoyButton:
    case Bind::kJoyAxis:
    case Bind::kJoyHat: {
        int best = 0;
        for (const Device& d : devices_) {
            if (d.pad || d.guid != b.guid) continue;
            if (b.kind == Bind::kJoyButton && SDL_GetJoystickButton(d.joy, b.code)) best = 1023;
            if (b.kind == Bind::kJoyHat && (SDL_GetJoystickHat(d.joy, b.code) & b.dir)) best = 1023;
            if (b.kind == Bind::kJoyAxis) best = std::max(best, axis_amount(SDL_GetJoystickAxis(d.joy, b.code), b.dir, b.full));
        }
        return best;
    }
    default: return 0;
    }
}

CarInput Input::drive(const std::string& hold) {
    // per action: keys and buttons (1023), digital axes and analog axes, through controls_update
    struct Read {
        int key = 0, dig = 0, ana = 0;
    };
    auto read = [&](int a) {
        Read r;
        for (const Bind& b : bindings.keys[size_t(a)])
            if (bind_value(b)) r.key = 1023;
        for (const Bind& b : bindings.pad[size_t(a)]) {
            const int v = bind_value(b);
            if (!b.is_axis()) {
                if (v) r.key = 1023;
            } else if (b.digital) {
                r.dig = std::max(r.dig, controls_axis_value(v, 2));
            } else {
                r.ana = std::max(r.ana, controls_axis_value(v, 0));
            }
        }
        return r;
    };
    auto held = [&](const char* k) { return hold.find(k) != std::string::npos; };
    CarInput in;  // (the original's keyboard setup; every value comes positive, directions +1)
    ControlSetup& s = in.setup;
    auto device = [&](int act, int dev) {
        s.b[size_t(act)].device = dev;
        s.b[size_t(act)].axis = dev == 1 ? 0 : 1;
    };
    const Read accel = read(kAccel);
    in.accel = std::max({accel.key, accel.dig, accel.ana});
    if (held("accel")) in.accel = 1023;
    if (in.accel && !accel.key) device(kActAccel, accel.dig >= accel.ana ? 2 : 0);

    const Read brake = read(kBrake);
    if (brake.key || held("brake")) {
        in.brake = 1023;
    } else if (brake.dig) {
        in.brake = brake.dig;
        device(kActBrake, 2);  // ramped like a key
    } else if (brake.ana) {
        in.brake = brake.ana;
        device(kActBrake, 0);  // a pedal: the brake follows it
    }

    const Read l = read(kSteerLeft), r = read(kSteerRight);
    const bool kl = l.key || held("left"), kr = r.key || held("right");
    const int dig = std::clamp(r.dig - l.dig, -1023, 1023), ana = std::clamp(r.ana - l.ana, -1023, 1023);
    if (kl || kr) steer_mode_ = 1;
    else if (dig) steer_mode_ = 2;
    else if (ana) steer_mode_ = 0;
    device(kActSteer, steer_mode_);
    if (steer_mode_ == 1) {
        in.steer = kl ? 1023 : 0;
        in.steer_right = kr ? 1023 : 0;
    } else {
        in.steer = steer_mode_ == 2 ? dig : ana;
    }

    const Read up = read(kGearUp), down = read(kGearDown);
    in.gear_up = std::max({up.key, up.dig, up.ana});
    in.gear_down = std::max({down.key, down.dig, down.ana});
    if (held("gearup")) in.gear_up = 1023;
    if (held("geardown")) in.gear_down = 1023;
    return in;
}

ControlsFile Input::replay_controls() const {
    ControlsFile f = original;
    f.setup = ControlSetup{};
    for (int a = 0; a < kActCount; ++a)
        for (const Bind& b : bindings.keys[size_t(a)])
            if (b.kind == Bind::kKey)
                if (const int dos = scancode_to_dos(SDL_Scancode(b.code))) {
                    f.setup.b[size_t(a)].key = dos;
                    break;
                }
    return f;
}

bool Input::look_back() const {
    for (const Bind& b : bindings.keys[kLookBack])
        if (bind_held(b)) return true;
    for (const Bind& b : bindings.pad[kLookBack])
        if (bind_held(b)) return true;
    return false;
}

Input::Nav Input::pad_nav() const {
    Nav n;
    for (const Device& d : devices_) {
        if (!d.pad) continue;
        auto btn = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(d.pad, b); };
        const int lx = SDL_GetGamepadAxis(d.pad, SDL_GAMEPAD_AXIS_LEFTX), ly = SDL_GetGamepadAxis(d.pad, SDL_GAMEPAD_AXIS_LEFTY);
        n.up |= btn(SDL_GAMEPAD_BUTTON_DPAD_UP) || ly < -kStickNav;
        n.down |= btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ly > kStickNav;
        n.left |= btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -kStickNav;
        n.right |= btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > kStickNav;
        n.enter |= btn(SDL_GAMEPAD_BUTTON_SOUTH);
        n.esc |= btn(SDL_GAMEPAD_BUTTON_EAST);
        n.start |= btn(SDL_GAMEPAD_BUTTON_START);
    }
    return n;
}

Input::Nav Input::nav() const {
    Nav n = pad_nav();
    const bool* k = SDL_GetKeyboardState(nullptr);
    n.up |= k[SDL_SCANCODE_UP];
    n.down |= k[SDL_SCANCODE_DOWN];
    n.left |= k[SDL_SCANCODE_LEFT];
    n.right |= k[SDL_SCANCODE_RIGHT];
    n.enter |= k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER];
    n.esc |= k[SDL_SCANCODE_ESCAPE];
    return n;
}

bool Input::back_held() const {
    for (const Device& d : devices_)
        if (d.pad && SDL_GetGamepadButton(d.pad, SDL_GAMEPAD_BUTTON_BACK)) return true;
    return false;
}

bool Input::b_held_without_back() const {
    for (const Device& d : devices_)
        if (d.pad && !SDL_GamepadHasButton(d.pad, SDL_GAMEPAD_BUTTON_BACK) && SDL_GetGamepadButton(d.pad, SDL_GAMEPAD_BUTTON_EAST))
            return true;
    return false;
}

bool Input::any_pad_button() const {
    for (const Device& d : devices_)
        if (d.pad && (SDL_GetGamepadButton(d.pad, SDL_GAMEPAD_BUTTON_SOUTH) || SDL_GetGamepadButton(d.pad, SDL_GAMEPAD_BUTTON_START) ||
                      SDL_GetGamepadAxis(d.pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 1000))
            return true;
    return false;
}

void Input::rumble(const Car& car, bool racing) {
    if (!bindings.rumble || !racing) {
        stop_rumble();
        return;
    }
    const uint64_t now = SDL_GetTicks();
    // knocks: a crash, a wall; while they last the motors keep their strength
    if (car.crash_sound) {
        rumble_lo_ = 1.0f;
        rumble_hi_ = 0.8f;
        rumble_until_ = now + 450;
    } else if (car.wall_sound) {
        rumble_lo_ = std::max(rumble_lo_, 0.6f);
        rumble_hi_ = std::max(rumble_hi_, 0.35f);
        rumble_until_ = std::max(rumble_until_, now + 180);
    }
    float lo = 0, hi = 0;
    if (now < rumble_until_) {
        lo = rumble_lo_;
        hi = rumble_hi_;
    } else {
        rumble_lo_ = rumble_hi_ = 0;
    }
    // a skid hums, a bumpy surface under the front wheels shakes
    if (car.skid_sound) hi = std::max(hi, 0.25f);
    if (car.bumpy_front && car.speed > 500.0f) lo = std::max(lo, 0.3f);
    const auto l = uint16_t(lo * 65535.0f), h = uint16_t(hi * 65535.0f);
    for (const Device& d : devices_) {
        if (d.pad && !allowed(d)) continue;
        if (d.pad) SDL_RumbleGamepad(d.pad, l, h, 200);
        else SDL_RumbleJoystick(d.joy, l, h, 200);
    }
}

void Input::stop_rumble() {
    rumble_until_ = 0;
    rumble_lo_ = rumble_hi_ = 0;
    for (const Device& d : devices_) {
        if (d.pad) SDL_RumbleGamepad(d.pad, 0, 0, 0);
        else if (d.joy) SDL_RumbleJoystick(d.joy, 0, 0, 0);
    }
}

} // namespace scr::input

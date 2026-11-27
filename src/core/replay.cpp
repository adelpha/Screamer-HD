#include "core/replay.h"

#include <algorithm>
#include <cstdio>

namespace scr {

namespace {
constexpr size_t kPreamble = ControlsFile::kSize;
}

bool Replay::load(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t pre[kPreamble];
    bool ok = std::fread(pre, 1, kPreamble, f) == kPreamble;
    int32_t h[6]{};
    ok = ok && std::fread(h, 4, 6, f) == 6 && h[0] > 0 && h[0] < 10000000;
    if (ok) {
        controls.parse(pre, kPreamble);
        has_setup = std::any_of(pre, pre + 6 * 0x1c, [](uint8_t b) { return b != 0; });
        frames.resize(size_t(h[0]));
        ok = std::fread(frames.data(), sizeof(ReplayFrame), frames.size(), f) == frames.size();
        car = h[1];
        laps = h[2] + 1;
        level = h[3];
        game_mode = h[4];
        race_options = h[5];
    }
    std::fclose(f);
    return ok;
}

bool Replay::save(const std::string& path) const {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const auto pre = controls.bytes();
    const int32_t h[6] = {int32_t(frames.size()), car, laps - 1, level, game_mode, race_options};
    bool ok = std::fwrite(pre.data(), 1, kPreamble, f) == kPreamble && std::fwrite(h, 4, 6, f) == 6 &&
              std::fwrite(frames.data(), sizeof(ReplayFrame), frames.size(), f) == frames.size();
    ok = std::fclose(f) == 0 && ok;
    return ok;
}

void Replay::read_frame(const ReplayFrame& f, CarInput& in) const {
    in.setup = has_setup ? controls.setup : ControlSetup{};
    ControlBinding& br = in.setup.b[kActBrake];
    ControlBinding& st = in.setup.b[kActSteer];
    in.accel = f.accel;
    in.brake = f.brake;
    if (f.brake >= kBrakePedal && f.brake <= kBrakePedal + 0x3ff) {
        br.device = 0;
        br.axis = 1;
        in.brake = f.brake - kBrakePedal;
    } else if (f.brake >= kBrakeKey && f.brake <= kBrakeKey + 0x3ff) {
        br.device = 1;
        br.axis = 0;
        in.brake = f.brake - kBrakeKey;
    }
    in.steer = f.steer_left;
    in.steer_right = f.steer_right;
    if (f.steer_right == kSteerAxis) {
        st.device = 0;
        st.axis = 1;
        in.steer_right = 0;
    } else if (f.steer_right >= kSteerKeys && f.steer_right <= kSteerKeys + 0x3ff) {
        st.device = 1;
        st.axis = 0;
        in.steer_right = f.steer_right - kSteerKeys;
    } else if (f.steer_right == kSteerAxisKeys) {
        st.device = 2;
        st.axis = 1;
        in.steer_right = 0;
    }
    in.gear_up = f.gear_up;
    in.gear_down = f.gear_down;
}

void Replay::add_frame(const CarInput& in, float frame_time, float raw_time) {
    ReplayFrame rf;
    rf.frame_time = frame_time;
    rf.raw_time = raw_time;
    rf.accel = int16_t(in.accel);
    rf.brake = int16_t(in.brake);
    rf.steer_left = int16_t(in.steer);
    rf.steer_right = int16_t(in.steer_right);
    rf.gear_up = int16_t(in.gear_up);
    rf.gear_down = int16_t(in.gear_down);
    frames.push_back(rf);
    pending_.push_back({in.setup.steer_mode(), in.setup.brake_pedal() ? 1 : 0});
}

void Replay::finish_recording(const ControlsFile& base) {
    if (pending_.empty() && has_setup) return;  // (done already: the frames carry their marks)
    controls = base;
    has_setup = true;
    // the most used steering device and brake (a brake only counts while pressed)
    size_t steer_n[3]{}, brake_n[2]{};
    for (size_t i = 0; i < pending_.size(); ++i) {
        const int m = pending_[i].steer_mode;
        if (m >= 0 && m <= 2) ++steer_n[m];
        if (frames[i].brake != 0) ++brake_n[pending_[i].brake_pedal];
    }
    ++steer_n[1];  // (ties and no frames: the keys, as the original's default setup)
    const int steer_mode = int(std::max_element(steer_n, steer_n + 3) - steer_n);
    const int pedal = brake_n[1] > brake_n[0] ? 1 : 0;
    ControlSetup& s = controls.setup;
    for (ControlBinding& b : s.b) {
        b.enabled = 1;
        b.dir = 1;  // ours give every value as a positive one
    }
    auto set_device = [](ControlBinding& b, int device, int axis_index) {
        b.device = device;
        b.axis = device == 1 ? 0 : 1;
        if (device != 1) {
            b.joystick = 0;
            b.axis_index = axis_index;
        }
    };
    set_device(s.b[kActSteer], steer_mode, 0);
    set_device(s.b[kActBrake], pedal ? 0 : 1, 1);
    for (size_t i = 0; i < pending_.size() && i < frames.size(); ++i) {
        ReplayFrame& f = frames[i];
        const int m = pending_[i].steer_mode;
        if (m != steer_mode && m >= 0 && m <= 2) {
            f.steer_right = m == 0 ? kSteerAxis : m == 1 ? int16_t(kSteerKeys + std::clamp<int>(f.steer_right, 0, 0x3ff)) : kSteerAxisKeys;
        }
        if (f.brake != 0 && pending_[i].brake_pedal != pedal)
            f.brake = int16_t((pending_[i].brake_pedal ? kBrakePedal : kBrakeKey) + std::clamp<int>(f.brake, 0, 0x3ff));
    }
    pending_.clear();
}

} // namespace scr

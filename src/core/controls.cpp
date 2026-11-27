#include "core/controls.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace scr {

ControlSetup::ControlSetup() {
    // DEFCTRL.CFG (and SETUP's keyboard defaults)
    static constexpr int32_t kKeys[6] = {0x48, 0x50, 0x4b, 0x4d, 0x38, 0x1d};
    static constexpr int32_t kAxis[6] = {1, 1, 0, 0, 0, 0};
    for (size_t i = 0; i < 6; ++i) b[i] = ControlBinding{1, 0, 1, 0, kAxis[i], 1, kKeys[i]};
}

bool ControlsFile::parse(const uint8_t* data, size_t size) {
    if (size < kSize) return false;
    std::memcpy(setup.b.data(), data, 6 * 0x1c);
    std::memcpy(calibration.data(), data + 6 * 0x1c, calibration.size());
    std::memcpy(ports.data(), data + 6 * 0x1c + calibration.size(), 40);
    return true;
}

std::array<uint8_t, ControlsFile::kSize> ControlsFile::bytes() const {
    std::array<uint8_t, kSize> out{};
    std::memcpy(out.data(), setup.b.data(), 6 * 0x1c);
    std::memcpy(out.data() + 6 * 0x1c, calibration.data(), calibration.size());
    std::memcpy(out.data() + 6 * 0x1c + calibration.size(), ports.data(), 40);
    return out;
}

bool ControlsFile::load(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t buf[kSize];
    const size_t n = std::fread(buf, 1, kSize, f);
    std::fclose(f);
    return parse(buf, n);
}

bool ControlsFile::save(const std::string& path) const {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const auto b = bytes();
    const bool ok = std::fwrite(b.data(), 1, b.size(), f) == b.size();
    return std::fclose(f) == 0 && ok;
}

int controls_axis_value(int v, int device) {
    if (0x3ff < v) v = 0x3ff;
    if (device == 2 && 0x1ff < v) v = 0x3ff;
    if (v < -0x3ff) v = -0x3ff;
    if (device == 2 && v < -0x1ff) v = -0x3ff;
    if (std::abs(v) < 0x66) v = 0;
    return v;
}

void controls_update(const ControlSetup& s, const std::array<int, 6>& raw, std::array<int, 6>& inputs) {
    for (size_t i = 0; i < 6; ++i) {
        const ControlBinding& c = s.b[i];
        if (c.enabled != 1) continue;
        if (c.device == 0 || c.device == 2) {
            if (c.axis == 1) {
                bool copied = false;
                for (size_t j = 0; j < i; ++j) {
                    const ControlBinding& e = s.b[j];
                    if (e.enabled == 1 && e.axis == 1 && (e.device == 0 || e.device == 2) && e.axis_index == c.axis_index &&
                        e.joystick == c.joystick) {
                        inputs[i] = inputs[j];
                        copied = true;
                        break;
                    }
                }
                if (copied) continue;
                inputs[i] = controls_axis_value(raw[i], c.device);
            } else {
                inputs[i] = raw[i] != 0 ? 0x3ff : 0;  // joy_read_buttons & mask
            }
        } else {
            inputs[i] = raw[i] != 0 ? 0x3ff : 0;  // g_key_state[key]
        }
    }
}

} // namespace scr

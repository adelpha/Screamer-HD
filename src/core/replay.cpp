#include "core/replay.h"

#include <cstdio>

namespace scr {

namespace {
constexpr size_t kPreamble = 6 * 0x1c + 0x28 * 0x1c + 10 * 4;
}

bool Replay::load(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    bool ok = std::fseek(f, long(kPreamble), SEEK_SET) == 0;
    int32_t h[6]{};
    ok = ok && std::fread(h, 4, 6, f) == 6 && h[0] > 0 && h[0] < 10000000;
    if (ok) {
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
    static const uint8_t zeros[kPreamble] = {};
    const int32_t h[6] = {int32_t(frames.size()), car, laps - 1, level, game_mode, race_options};
    bool ok = std::fwrite(zeros, 1, kPreamble, f) == kPreamble && std::fwrite(h, 4, 6, f) == 6 &&
              std::fwrite(frames.data(), sizeof(ReplayFrame), frames.size(), f) == frames.size();
    std::fclose(f);
    return ok;
}

} // namespace scr

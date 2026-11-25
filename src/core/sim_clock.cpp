#include "core/sim_clock.h"

#include <algorithm>
#include <cmath>

namespace scr {

float pow_table(int row, int k) {
    row = std::clamp(row, 0, 49);
    k = std::clamp(k, 0, 99);
    // FILD k; FMUL float 0.005; FADD double 0.5; FSTP float  -> base
    const float base = static_cast<float>(static_cast<double>(k) * static_cast<double>(0.005f) + 0.5);
    // FILD row; FLD1; FADDP; FDIVR double 50.0 -> exponent; pow() in double, stored as float
    const double exponent = 50.0 / (static_cast<double>(row) + 1.0);
    return static_cast<float>(std::pow(static_cast<double>(base), exponent));
}

void SimClock::begin_frame(uint32_t take) {
    const uint32_t used = ticks_ < take ? ticks_ : take;
    const uint32_t left = ticks_ - used;
    ticks_ = used;
    // The game does this on the x87 in extended precision and stores the results as
    // floats. Doubles for the intermediate values give the same float results here.
    // Constants in the executable: 0.01, 0.2, 0.02 (double), 9.0 and 10.0 (float).
    raw_ = static_cast<float>(static_cast<double>(ticks_) * 0.01);
    const double smoothed = (static_cast<double>(frame_time_) * 9.0 + static_cast<double>(raw_)) / 10.0;
    frame_time_ = static_cast<float>(smoothed);  // FST, then the unrounded value is compared
    ticks_ = left;
    if (0.2 <= smoothed) frame_time_ = 0.2f;
    if (static_cast<double>(frame_time_) <= 0.02) frame_time_ = 0.02f;
    pow_row_ = static_cast<int>(1.0 / static_cast<double>(frame_time_));  // __CHP truncates toward zero
    // Texture animation clock (frame_timing_update, 0x118e08 / 0x121538): 0.3 turns per second.
    anim_phase_ = static_cast<float>(static_cast<double>(frame_time_) * 0.3 + static_cast<double>(anim_phase_));
    if (1.0f <= anim_phase_) anim_phase_ += -1.0f;
    anim_counter_ = static_cast<int32_t>(std::lround(static_cast<double>(anim_phase_) * 65535.0));
}

} // namespace scr

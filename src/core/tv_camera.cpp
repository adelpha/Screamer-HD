#include "core/tv_camera.h"

#include <cmath>
#include <algorithm>
#include <cstdlib>

namespace scr {

namespace {
constexpr float kTwoPi = 6.2831854820251465f;
constexpr float kMinusHalfPi = -1.5707963705062866f;
int chop(double v) { return int(v); }
} // namespace

void TvCamera::load(const std::vector<RacingLine>& lines) {
    points_.clear();
    for (const RacingLine& l : lines)
        for (const SplineSegment& s : l.segments) points_.push_back({s.x, s.point, s.z});
    cars_.clear();
    followed = 0;
    next_ = -1;
    idle_ticks_ = 1000;
}

void TvCamera::track(const Opponent& o, PerCar& c) {
    // camera_tv_track_opponent: the nearest camera point (not the one exactly on the car)
    float best = 2.1474835e+09f;
    c.point = 0;
    for (size_t i = 0; i < points_.size(); ++i) {
        const double dx = double(points_[i].x - o.pos_x), dz = double(points_[i].z - o.pos_z);
        const float d = float(std::sqrt(dx * dx + dz * dz));
        if (std::fabs(d) != 0.0f && d < best) {
            best = float(chop(d));
            c.point = int(i);
        }
    }
    const Point& p = points_[size_t(c.point)];
    const float dx = float(p.x - o.pos_x), dz = float(p.z - o.pos_z), dy = float(p.y - o.pos_y);
    const float horiz = float(std::sqrt(double(dx * dx + dz * dz)));
    float a = horiz > 0 ? float(std::acos(double(dx / horiz))) : 0.0f;
    if (horiz > 0 && dz / horiz < 0.0f) a = kTwoPi - a;
    c.yaw = a + kMinusHalfPi;
    const float d3 = float(std::sqrt(double(dy * dy + dz * dz + dx * dx)));
    c.pitch = kTwoPi - (d3 > 0 ? float(std::fabs(std::acos(double(horiz / d3)))) : 0.0f);
    c.x = p.x;
    c.y = p.y;
    c.z = p.z;
    bool cut_now = false;
    if (c.point != c.prev_point) {
        c.prev_dist = double(horiz);
        c.prev_point = c.point;
        cut_now = true;
    }
    // the last digit of the height: how the camera follows the car
    const int flags = std::abs(p.y) % 10;
    const bool was = c.target;
    const bool target = (flags & 1) || ((flags & 2) && float(c.prev_dist) - horiz <= 0.0f) ||
                        ((flags & 4) && 0.0f <= float(c.prev_dist) - horiz);
    c.target = target;
    c.cut = cut_now || (target != was);
    if (c.target) {
        const float k = float(chop(double(d3 + -1500.0f)));
        if (0.0f < k) {
            c.x = chop(std::sin(double(c.yaw)) * std::cos(double(c.pitch)) * double(k) + double(c.x));
            c.y = chop(std::sin(double(c.pitch)) * double(k) + double(c.y));
            c.z = chop(-std::cos(double(c.pitch)) * std::cos(double(c.yaw)) * double(k) + double(c.z));
        }
    }
    c.prev_dist = double(horiz);
}

void TvCamera::update(const std::vector<Opponent>& cars, int ticks) {
    if (points_.empty() || cars.empty()) return;
    if (cars_.size() != cars.size()) cars_.assign(cars.size(), PerCar{});
    for (size_t i = 0; i < cars.size(); ++i) track(cars[i], cars_[i]);
    const int n = int(cars.size());
    followed = std::clamp(followed, 0, n - 1);
    // idle_timer_check + camera_handle_view_keys (view 4): every 1000 ticks the next car
    // whose camera is another one
    idle_ticks_ += ticks;
    if (1000 < idle_ticks_) {
        idle_ticks_ = 0;
        for (int i = 0; i < n; ++i) {
            next_ = next_ + 1 >= n ? 0 : next_ + 1;
            if (followed == next_ || cars_[size_t(next_)].point != cars_[size_t(followed)].point) break;
        }
    }
    // camera_mode_tv: the switch waits for the followed car's next cut
    if (next_ >= 0 && cars_[size_t(followed)].cut && followed != next_) followed = next_;
    const PerCar& c = cars_[size_t(followed)];
    x = c.x;
    y = c.y;
    z = c.z;
    pitch = c.pitch;
    yaw = c.yaw;
}

} // namespace scr

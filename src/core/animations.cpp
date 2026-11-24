#include "core/animations.h"

#include "core/archive.h"
#include "core/dtr.h"

#include <algorithm>
#include <cmath>

namespace scr {

namespace {

double safe_sqrt(double v) { return v >= 0.0 ? std::sqrt(v) : 0.0; }

} // namespace

bool TrackAnimations::load(const std::string& data_dat, const std::string& track_name) {
    list.clear();
    splines.clear();
    auto a = Archive::open(data_dat);
    if (!a) return false;
    auto dtr = a->read(track_name + "DAT.DTR");
    auto spl = a->read(track_name + "ANM.SPL");
    if (!dtr || !spl) return false;
    splines = parse_splines(*spl);
    auto values = DtrFile(std::string(dtr->begin(), dtr->end())).section("ANIMATIONS");
    if (!values) return false;
    for (size_t i = 0; i + 4 <= values->size(); i += 4) {
        TrackAnimation an;
        an.name = (*values)[i].text;
        an.speed = (*values)[i + 1].f;
        an.spline = (*values)[i + 2].i;
        an.sound_fx = (*values)[i + 3].i;
        if (an.spline < 0 || size_t(an.spline) >= splines.size() || splines[size_t(an.spline)].segments.empty()) continue;
        an.segment = splines[size_t(an.spline)].start;
        list.push_back(an);
    }
    return !list.empty();
}

void TrackAnimations::segment_init(TrackAnimation& a) const {
    // spline_segment_init: the three control points of this half segment and the point at t.
    const RacingLine& line = splines[size_t(a.spline)];
    const SplineSegment& s = line.segments[size_t(a.segment)];
    const SplineSegment& nx = line.segments[size_t(s.next)];
    if (a.step == 0) {
        a.p[0] = float(s.ax); a.p[1] = float(s.az);
        a.p[2] = float(s.bx); a.p[3] = float(s.bz);
        a.p[4] = float(nx.ax); a.p[5] = float(nx.az);
    } else {
        a.p[0] = float(s.bx); a.p[1] = float(s.bz);
        a.p[2] = float(nx.ax); a.p[3] = float(nx.az);
        a.p[4] = float(nx.bx); a.p[5] = float(nx.bz);
    }
    auto eval = [&](float t, float& x, float& z) {
        const float b0 = float((1.0 - double(t)) * (1.0 - double(t)) * 0.5);
        const float b2 = float(double(t) * double(t) * 0.5);
        const float b1 = float(1.0 - (double(b0) + double(b2)));
        x = float(double(a.p[2]) * b1 + double(a.p[0]) * b0 + double(a.p[4]) * b2);
        z = float(double(a.p[1]) * b0 + double(a.p[3]) * b1 + double(a.p[5]) * b2);
    };
    eval(a.t, a.eval_x, a.eval_z);
    float ax, az;
    eval(float(double(a.t) + double(0.002f)), ax, az);
    const float dx = ax - a.eval_x, dz = az - a.eval_z;
    const double len = std::sqrt(double(dz) * dz + double(dx) * dx);
    if (len > 0.0) {
        a.dir_x = float(dx / len);
        a.dir_z = float(dz / len);
    }
    // The height: the knots' +0xc, linear over the two halves.
    a.py = float(double(s.point) + (double(a.step) + double(a.t)) * double(nx.point - s.point) * 0.5);
}

void TrackAnimations::advance(TrackAnimation& a, float d) const {
    const RacingLine& line = splines[size_t(a.spline)];
    for (;;) {
        const float lx = ((a.p[0] - a.p[2] * 2.0f + a.p[4]) * a.t + a.p[2]) - a.p[0];
        const float lz = ((a.p[1] - a.p[3] * 2.0f + a.p[5]) * a.t + a.p[3]) - a.p[1];
        float len = float(safe_sqrt(double(lx * lx + lz * lz)));
        if (std::fabs(len) == 0.0f) len = 1.0f;
        a.t = float(double(d) / double(len) + double(a.t));
        if (a.t < 1.0f) break;
        a.t = 0;
        if (a.step == 0) {
            a.step = 1;
        } else {
            a.step = 0;
            a.segment = line.segments[size_t(a.segment)].next;
        }
        segment_init(a);
        const float ex = a.eval_x - float(a.x), ez = a.eval_z - float(a.z);
        d = float(double(d) - safe_sqrt(double(ex * ex + ez * ez)));
        a.x = int(a.eval_x);
        a.z = int(a.eval_z);
    }
    segment_init(a);
    a.x = int(a.eval_x);
    a.y = int(a.py);
    a.z = int(a.eval_z);
    // The heading from the direction: acos of x, mirrored by the sign of z.
    a.prev_yaw = a.yaw;
    float yaw = float(std::fabs(std::acos(std::clamp(double(a.dir_x), -1.0, 1.0))));
    yaw = a.dir_z <= 0.0f ? 7.853981972f - yaw : yaw + 1.570796371f;
    while (yaw < 0.0f) yaw += 6.283185482f;
    while (6.283185482f <= yaw) yaw += -6.283185482f;
    a.yaw = yaw;
}

void TrackAnimations::start(float frame_time) {
    for (TrackAnimation& a : list) {
        segment_init(a);
        advance(a, a.speed * frame_time);
    }
}

void TrackAnimations::update(int lx, int lz, float frame_time) {
    for (TrackAnimation& a : list) {
        if (std::abs(lx - a.x) >= 52000 || std::abs(lz - a.z) >= 52000) continue;
        advance(a, a.speed * frame_time);
    }
}

} // namespace scr

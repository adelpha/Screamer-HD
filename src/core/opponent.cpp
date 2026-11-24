#include "core/opponent.h"

#include "core/archive.h"
#include "core/dtr.h"
#include "core/sim_clock.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace scr {
namespace {

constexpr float kPi = 3.1415927410125732f;
constexpr float kTwoPi = 6.2831854820251465f;
constexpr float kHalfPi = 1.5707963705062866f;

int32_t rd32(const std::vector<uint8_t>& d, size_t o) {
    int32_t v = 0;
    if (o + 4 <= d.size()) std::memcpy(&v, d.data() + o, 4);
    return v;
}
float rdf(const std::vector<uint8_t>& d, size_t o) {
    float v = 0;
    if (o + 4 <= d.size()) std::memcpy(&v, d.data() + o, 4);
    return v;
}
int chop(double v) { return static_cast<int>(v); }
double safe_sqrt(double v) { return v >= 0.0 ? std::sqrt(v) : 0.0; }
void rotate_xz(int& ox, int& oz, int x, int z, float angle) {
    const double c = std::cos(double(angle)), s = std::sin(double(angle));
    ox = chop(double(z) * s + double(x) * c);
    oz = chop(double(z) * c - double(x) * s);
}
int div4(int v) { return v / 4; }  // the game's signed divide (rounds toward zero)

// point_in_front_box: bit 1 when `other` is ahead (rotated z >= 1), bit 0 when to the side x > 0.
int point_in_front(int x, int z, int ox, int oz, float angle) {
    int rx, rz;
    rotate_xz(rx, rz, ox - x, oz - z, angle);
    return (rx > 0 ? 1 : 0) | (rz >= 1 ? 2 : 0);
}

// angle_from_vector: heading of a unit direction (acos of x, side from z), in 0..2pi.
float angle_from_vector(float x, float z) {
    const float a = float(std::fabs(std::acos(std::clamp(double(x), -1.0, 1.0))));
    float r = z <= 0.0f ? 7.853981971740723f - a : a + kHalfPi;
    while (r < 0.0f) r += kTwoPi;
    while (kTwoPi <= r) r += -kTwoPi;
    return r;
}

// Marker tracking shared by the player and the CPU cars (opponent_update_track_progress).
struct Progress {
    int marker = 0;
    int lap = 0;
    float value = 0;
};

void update_progress(const std::vector<TrackMarker>& m, int& marker, int& lap, float& progress, int& flags,
                     float* speed_factor, int x, int z) {
    const int n = int(m.size());
    if (n == 0) return;
    for (int d = -1; d < 2; ++d) {
        int k = marker + d;
        if (n <= k) k -= n;
        if (k < 0) k += n;
        const TrackMarker& mk = m[size_t(k)];
        if (std::abs(x - mk.x) < mk.radius && std::abs(z - mk.z) < mk.radius &&
            chop(safe_sqrt(double(x - mk.x) * (x - mk.x) + double(z - mk.z) * (z - mk.z))) <= mk.radius) {
            if (k == 0 && marker == n - 1) ++lap;  // crossed the line (checkpoint bookkeeping simplified)
            marker = k;
            flags = mk.flags;
            if (speed_factor) *speed_factor = mk.speed_factor;
        }
    }
    const TrackMarker& a = m[size_t(marker)];
    const TrackMarker& b = m[size_t((marker + 1) % n)];
    const double sx = double(b.x - a.x) * 0.05, sz = double(b.z - a.z) * 0.05;
    const int span = std::max(1, chop(safe_sqrt(sx * sx + sz * sz)));
    const double cx = double(x - a.x) * 0.05, cz = double(z - a.z) * 0.05;
    const int dist = chop(safe_sqrt(cx * cx + cz * cz));
    progress = float(dist) / float(span) + float(marker) + float(lap * 100);
}

} // namespace

std::vector<RacingLine> parse_splines(const std::vector<uint8_t>& d) {
    std::vector<RacingLine> out;
    size_t o = 0;
    while (o + 8 <= d.size()) {
        const int32_t n = rd32(d, o), start = rd32(d, o + 4);
        o += 8;
        if (n <= 0 || o + size_t(n) * 0x38 > d.size()) break;
        RacingLine line;
        line.start = start;
        for (int32_t i = 0; i < n; ++i, o += 0x38) {
            SplineSegment s;
            s.x = rd32(d, o + 4);
            s.z = rd32(d, o + 8);
            s.point = rd32(d, o + 0xc);
            s.next = rd32(d, o + 0x10);
            s.ax = rd32(d, o + 0x14);
            s.az = rd32(d, o + 0x18);
            s.bx = rd32(d, o + 0x20);
            s.bz = rd32(d, o + 0x24);
            line.segments.push_back(s);
        }
        out.push_back(std::move(line));
    }
    return out;
}

std::vector<TrackMarker> parse_markers(const std::vector<uint8_t>& d) {
    std::vector<TrackMarker> out;
    const int32_t n = rd32(d, 0);
    for (int32_t i = 0; i < n && 4 + size_t(i + 1) * 32 <= d.size(); ++i) {
        const size_t o = 4 + size_t(i) * 32;
        out.push_back({rd32(d, o), rd32(d, o + 4), rdf(d, o + 8), rd32(d, o + 0x14), rd32(d, o + 0x1c)});
    }
    return out;
}

bool Race::load(const std::string& data_dat, const std::string& track_name, char level) {
    auto a = Archive::open(data_dat);
    if (!a) return false;
    auto spl = a->read(track_name + "OPP.SPL");
    auto mrk = a->read(track_name + ".MRK");
    auto dtr = a->read(track_name + std::string(1, level) + ".DTR");
    if (!spl || !dtr) return false;
    lines = parse_splines(*spl);
    if (mrk) markers = parse_markers(*mrk);
    size_t n = 0;
    auto packed = DtrFile(std::string(dtr->begin(), dtr->end())).pack_section("OPPONENTS", 0x14, 0x14, &n);
    defs.clear();
    if (packed)
        for (size_t i = 0; i < n; ++i)
            defs.push_back({rdf(*packed, i * 0x14), rdf(*packed, i * 0x14 + 4), rdf(*packed, i * 0x14 + 8),
                            rdf(*packed, i * 0x14 + 12), rdf(*packed, i * 0x14 + 16)});
    return !lines.empty();
}

float Race::lerp(float value, float target, int k) const {
    return (value - target) * pow_table(pow_row_, k) + target;
}

void Race::start(const Track& track, const CarSetup& player_setup, const std::vector<CarSetup>& opponent_setups) {
    const size_t n = std::min({defs.size(), opponent_setups.size(), lines.empty() ? size_t(0) : lines.size() - 1});
    opponents.assign(n, Opponent{});
    for (size_t i = 0; i < n; ++i) {
        Opponent& o = opponents[i];
        o.index = int(i);
        o.def = defs[i];
        o.setup = opponent_setups[i];
        // Corner probes like the player's, then bumpers a quarter of the way in (opponent_car_load).
        const int front = o.setup.wheel_pos[2][1] - 250, rear = o.setup.wheel_pos[3][1] + 150;
        o.front_local_z = front + div4(rear - front);
        o.rear_local_z = rear + div4(front - rear);
        o.collision_radius = std::abs((o.front_local_z - o.rear_local_z) / 2);
        o.spline_index = int(i);
        o.segment = lines[i].start;
        next_spline_segment(o);
        advance_on_spline(o);
        track_progress(o);
        bumpers(o);
        check_ground(track, o);
        update_position(o);
    }
    // The player starts on the next racing line's start knot.
    const RacingLine& pl = lines[std::min(n, lines.size() - 1)];
    const SplineSegment& s = pl.segments[size_t(std::clamp(pl.start, 0, int(pl.segments.size()) - 1))];
    player.reset(player_setup, track, s.x, s.z);
    for (Opponent& o : opponents) o.race_position = o.index + 1;
    player_position = int(opponents.size()) + 1;
}

void Race::next_spline_segment(Opponent& o) {
    const RacingLine& line = lines[size_t(o.spline_index)];
    const SplineSegment& s = line.segments[size_t(o.segment)];
    const SplineSegment& nx = line.segments[size_t(s.next)];
    if (o.step == 0) {
        o.p[0] = float(s.ax); o.p[1] = float(s.az);
        o.p[2] = float(s.bx); o.p[3] = float(s.bz);
        o.p[4] = float(nx.ax); o.p[5] = float(nx.az);
    } else {
        o.p[0] = float(s.bx); o.p[1] = float(s.bz);
        o.p[2] = float(nx.ax); o.p[3] = float(nx.az);
        o.p[4] = float(nx.bx); o.p[5] = float(nx.bz);
    }
    // spline_eval_2d_ahead: point at t and the unit direction towards t + 0.002, with the
    // x87's extended intermediates and float stores.
    auto eval = [&](float t, float& x, float& z) {
        const float b0 = float((1.0 - double(t)) * (1.0 - double(t)) * 0.5);
        const float b2 = float(double(t) * double(t) * 0.5);
        const float b1 = float(1.0 - (double(b0) + double(b2)));
        x = float(double(o.p[2]) * b1 + double(o.p[0]) * b0 + double(o.p[4]) * b2);
        z = float(double(o.p[1]) * b0 + double(o.p[3]) * b1 + double(o.p[5]) * b2);
    };
    eval(o.t, o.eval_x, o.eval_z);
    float ax, az;
    eval(float(double(o.t) + double(0.002f)), ax, az);
    const float dx = float(double(ax) - double(o.eval_x)), dz = float(double(az) - double(o.eval_z));
    const float inv = float(1.0 / std::sqrt(double(dz) * dz + double(dx) * dx));
    o.dir_x = float(double(dx) * inv);
    o.dir_z = float(double(dz) * inv);
}

void Race::advance_on_spline(Opponent& o) {
    float d = o.speed * dt_;
    const RacingLine& line = lines[size_t(o.spline_index)];
    for (;;) {
        // vec_length_nonzero: length of the spline's derivative at t.
        const float lx = ((o.p[0] - o.p[2] * 2.0f + o.p[4]) * o.t + o.p[2]) - o.p[0];
        const float lz = ((o.p[1] - o.p[3] * 2.0f + o.p[5]) * o.t + o.p[3]) - o.p[1];
        float len = float(safe_sqrt(double(lx * lx + lz * lz)));
        if (std::fabs(len) == 0.0f) len = 1.0f;
        o.t = float(double(d) / double(len) + double(o.t));
        if (o.t < 1.0f) break;
        o.t = 0;
        if (o.step == 0) {
            o.step = 1;
        } else {
            o.step = 0;
            o.segment = line.segments[size_t(o.segment)].next;
        }
        next_spline_segment(o);
        const float ex = o.eval_x - float(o.line_x), ez = o.eval_z - float(o.line_z);
        d = float(double(d) - safe_sqrt(double(ex * ex + ez * ez)));
        o.line_x = chop(o.eval_x);
        o.line_z = chop(o.eval_z);
    }
    next_spline_segment(o);
    o.line_x = chop(o.eval_x);
    o.line_z = chop(o.eval_z);
    o.line_point = line.segments[size_t(o.segment)].point;
    o.ctrl_a_x = o.dir_x;
    o.ctrl_a_z = o.dir_z;
    o.line_yaw_prev = o.line_yaw;
    o.line_yaw = angle_from_vector(o.ctrl_a_x, o.ctrl_a_z);
}

void Race::update_position(Opponent& o) {
    // opponent_apply_lateral_offset: pushed sideways by hits, drifting back to the line.
    o.lateral_x = chop(double(o.lateral_dir_x * o.lateral_push * dt_) + double(o.lateral_x));
    o.lateral_z = chop(double(o.lateral_dir_z * o.lateral_push * dt_) + double(o.lateral_z));
    o.lateral_push = pow_table(pow_row_, 0x13c / 4) * o.lateral_push;
    if (o.lateral_push < 10.0f) o.lateral_push = 0.0f;
    if (o.lateral_x == o.prev_lateral_x && o.lateral_z == o.prev_lateral_z) {
        o.lateral_timer = dt_ + o.lateral_timer;
        if (3.0f <= o.lateral_timer) {
            o.lateral_x = chop(double(o.lateral_x) * pow_table(pow_row_, 0x180 / 4));
            o.lateral_z = chop(double(o.lateral_z) * pow_table(pow_row_, 0x180 / 4));
        }
    } else {
        o.lateral_timer = 0.0f;
    }
    // opponent_save_prev_position
    o.prev_lateral_x = o.lateral_x;
    o.prev_lateral_z = o.lateral_z;
    o.prev_speed = o.speed;
    o.pos_x = o.line_x + o.lateral_x;
    o.pos_z = o.line_z + o.lateral_z;
    if (o.crash_state == 0) o.pos_y = o.ground_y;
    o.pitch = o.ground_pitch + o.crash_pitch;
    o.yaw = o.line_yaw + o.crash_yaw;
    o.roll = o.ground_roll + o.crash_roll;
    o.draw_yaw = o.yaw + o.yaw_wobble;
}

void Race::update_target_speed(Opponent& o) {
    float top, cruise;
    if (player_position < o.race_position) {
        top = (o.def.max_spd + o.def.skill) * o.speed_factor;  // behind the player: catch up
        cruise = o.def.max_spd * o.speed_factor;
    } else {
        top = cruise = o.def.max_spd * o.speed_factor;
    }
    if (top <= o.speed) {
        if (o.speed <= top) return;
        if (cruise < o.speed) o.catching_up = true;
        o.speed = o.speed - o.def.brakes * dt_;
        if (top <= o.speed) return;
    } else {
        o.catching_up = false;
        o.speed = o.def.accel * dt_ + o.speed;
        if (o.speed <= top) return;
    }
    o.speed = top;
}

void Race::smooth_angles(Opponent& o) {
    float lean = (o.speed * o.def.sband) * (o.speed * o.def.sband) * (o.line_yaw - o.line_yaw_prev);
    lean = std::clamp(lean, -2.0f, 2.0f);
    o.yaw_wobble = lerp(o.yaw_wobble, float(double(lean) * 0.5), 0x4f);
    o.roll_ofs = lerp(o.roll_ofs, float(double(lean) * 0.02), 0x4c);
    o.pitch_ofs = lerp(o.pitch_ofs, -std::fabs(float(double(lean) * 0.02)), 0x4c);
}

void Race::crash_update(Opponent& o) {
    switch (o.crash_state) {
    case 2:
        o.crash_state = 1;
        o.crash_bounce = 3000.0f;
        o.crash_bounce_count = 0;
        o.crash_pitch_rate = 12.0f;
        o.crash_roll_rate = 0.0f;
        o.crash_yaw_rate = 12.0f;
        [[fallthrough]];
    case 1: {
        o.pos_y = chop(double(o.crash_bounce * dt_) + double(o.pos_y));
        const float k = pow_table(pow_row_, 0x184 / 4);
        o.crash_pitch_rate = k * o.crash_pitch_rate;
        o.crash_pitch = o.crash_pitch_rate * dt_ + o.crash_pitch;
        o.crash_yaw_rate = k * o.crash_yaw_rate;
        o.crash_yaw = o.crash_yaw_rate * dt_ + o.crash_yaw;
        o.crash_roll_rate = k * o.crash_roll_rate;
        o.crash_roll = o.crash_roll_rate * dt_ + o.crash_roll;
        o.crash_bounce = o.crash_bounce - dt_ * 6000.0f;
        if (o.pos_y <= o.ground_y) {
            ++o.crash_bounce_count;
            o.pos_y = o.ground_y;
            o.crash_bounce = float(-(double(o.crash_bounce) * 0.4));
            o.crash_yaw = o.crash_roll = o.crash_pitch = 0.0f;
            o.crash_roll_rate = float(((o.crash_bounce_count & 1) == 0 ? 1.0 : -1.0) * 0.6);
            o.crash_pitch_rate = -1.0f;
            o.crash_yaw_rate = 0.0f;
        }
        if (3 < o.crash_bounce_count) {
            o.crash_roll = o.crash_yaw = o.crash_pitch = 0.0f;
            o.crash_state = 0;
        }
        break;
    }
    case 3:
        o.crash_state = 4;
        o.crash_yaw_rate = 24.0f;
        [[fallthrough]];
    case 4:
        o.crash_yaw_rate = pow_table(pow_row_, 0x184 / 4) * o.crash_yaw_rate;
        o.crash_yaw = o.crash_yaw_rate * dt_ + o.crash_yaw;
        if (12.566370964050293f <= o.crash_yaw) {
            o.crash_yaw = 0.0f;
            o.crash_state = 0;
        }
        break;
    default:
        break;
    }
}

void Race::track_progress(Opponent& o) {
    update_progress(markers, o.track_marker, o.lap, o.track_progress, o.marker_flags, &o.speed_factor, o.pos_x, o.pos_z);
}

void Race::bumpers(Opponent& o) {
    rotate_xz(o.front_x, o.front_z, 0, o.front_local_z, -o.yaw);
    rotate_xz(o.rear_x, o.rear_z, 0, o.rear_local_z, -o.yaw);
    o.front_x += o.pos_x;
    o.front_z += o.pos_z;
    o.rear_x += o.pos_x;
    o.rear_z += o.pos_z;
}

void Race::check_ground(const Track& track, Opponent& o) {
    const auto g = ground_query(track, o.line_x + o.lateral_x, o.line_z + o.lateral_z);
    if (!g) {
        if (3000.0f < o.speed) o.speed = 0.0f;
        return;
    }
    o.ground_y = g->y;
    // Tilt from the face normal turned into the car's frame (rotate_xz by draw_yaw - pi/2).
    int rx, rz;
    rotate_xz(rx, rz, g->normal_x, g->normal_z, o.draw_yaw + -kHalfPi);
    o.ground_pitch = (float(rx) * kPi) / 46080.0f;
    o.ground_roll = (float(rz) * kPi) / 46080.0f;
    while (o.ground_roll < 0.0f) o.ground_roll += kTwoPi;
    while (o.ground_pitch < 0.0f) o.ground_pitch += kTwoPi;
}

void Race::cpu_contacts(Opponent& o) {
    // opponent_find_nearby + opponent_find_cpu_contacts: later cars within 1500 units.
    for (size_t j = size_t(o.index) + 1; j < opponents.size(); ++j) {
        Opponent& other = opponents[j];
        if (!other.active || std::abs(o.pos_x - other.pos_x) >= 1500 || std::abs(o.pos_z - other.pos_z) >= 1500) continue;
        const int limit = o.collision_radius + other.collision_radius;
        const int mine[2][2] = {{o.front_x, o.front_z}, {o.rear_x, o.rear_z}};
        const int theirs[2][2] = {{other.front_x, other.front_z}, {other.rear_x, other.rear_z}};
        for (int a = 0; a < 2; ++a)
            for (int b = 0; b < 2; ++b) {
                const int dx = mine[a][0] - theirs[b][0], dz = mine[a][1] - theirs[b][1];
                int dist = chop(safe_sqrt(double(dx) * dx + double(dz) * dz));
                if (dist >= limit) continue;
                // opponent_resolve_collision: the faster car slows to the average speed, the
                // other is pushed sideways out of the overlap.
                if (dist == 0) dist = 1;
                Opponent& faster = other.speed <= o.speed ? o : other;
                faster.speed = float(double(o.speed + other.speed) * 0.5);
                const float nx = float(dx) / float(dist), nz = float(dz) / float(dist);
                other.lateral_x = chop(double(other.lateral_x) - (double(float(limit) * nx) - double(dx)));
                other.lateral_z = chop(double(other.lateral_z) - (double(float(limit) * nz) - double(dz)));
            }
    }
    if (5000.0f < o.prev_speed - o.speed && o.crash_state == 0) o.crash_state = ((o.pos_x + o.pos_z) >> 2 & 1) + 2;
}

void Race::collide_player(Opponent& o) {
    const Car& p = player;
    const float r = float(o.collision_radius * 3);
    const int fx = chop(std::sin(double(o.yaw)) * double(r) + double(o.pos_x));
    const int fz = chop(double(o.pos_z) - std::cos(double(o.yaw)) * double(r));
    if (std::abs(fx - p.pos_x) >= 5000 || std::abs(fz - p.pos_z) >= 5000) return;
    const int limit = o.collision_radius * 2 + p.collision_radius;
    const int pb[2][2] = {{p.front_x, p.front_z}, {p.rear_x, p.rear_z}};
    for (int i = 0; i < 2; ++i) {
        const int dx = fx - pb[i][0], dz = fz - pb[i][1];
        int dist = chop(safe_sqrt(double(dx) * dx + double(dz) * dz));
        if (dist < limit && p.speed < o.speed) {
            // Steer round the slower player.
            dist += 1;
            const float nx = float(dx) / float(dist), nz = float(dz) / float(dist);
            o.lateral_x = chop(double(float(limit) * nx) - double(dx) + double(o.lateral_x));
            o.lateral_z = chop(double(float(limit) * nz) - double(dz) + double(o.lateral_z));
        }
    }
}

void Race::player_contacts(const Track& track) {
    // car_collide_vs_cpu: bumpers, nearby cars, bumper contacts (car_find_cpu_contacts).
    Car& p = player;
    p.update_bumpers();
    if (p.crash_state != 0) return;
    for (Opponent& o : opponents) {
        if (!o.active || o.crash_state != 0 || std::abs(p.pos_x - o.pos_x) >= 1500 || std::abs(p.pos_z - o.pos_z) >= 1500)
            continue;
        const int limit = p.collision_radius + o.collision_radius;
        const int pb[2][2] = {{p.front_x, p.front_z}, {p.rear_x, p.rear_z}};
        const int ob[2][2] = {{o.front_x, o.front_z}, {o.rear_x, o.rear_z}};
        for (int a = 0; a < 2; ++a) {
            for (int b = 0; b < 2; ++b) {
                const int dx = pb[a][0] - ob[b][0], dz = pb[a][1] - ob[b][1];
                int dist = chop(safe_sqrt(double(dx) * dx + double(dz) * dz));
                if (dist >= limit) continue;
                // car_resolve_cpu_collision
                if (dist == 0) dist = 1;
                const float nx = float(dx) / float(dist), nz = float(dz) / float(dist);
                const double ox = double(float(limit) * nx) - double(dx), oz = double(float(limit) * nz) - double(dz);
                if (ground_query(track, chop(double(o.pos_x) - ox), chop(double(o.pos_z) - oz))) {
                    if ((point_in_front(p.pos_x, p.pos_z, o.pos_x, o.pos_z, p.yaw) & 2) == 0) {
                        o.lateral_push = float(double(p.speed) * 0.5);
                        float wob = float(double(p.speed) * 0.0001);
                        if (kHalfPi < wob) wob = kHalfPi;
                        const int q = point_in_front(o.pos_x, o.pos_z, p.pos_x, p.pos_z, o.yaw);
                        o.yaw_wobble = float((q & 2) ? 1 : -1) * float((q & 1) ? 1 : -1) * wob;
                        p.impact_timer = float(double(p.speed) * 0.21);
                        p.yaw_rate = float(double(p.speed) * ((q & 1) ? 2.0e-4 : -2.0e-4));
                        p.speed = pow_table(pow_row_, 0x168 / 4) * p.speed;
                        p.impact_vel_x = nx;
                        p.impact_vel_z = nz;
                    } else {
                        o.speed = float(double(o.speed) * 0.7);
                    }
                    o.lateral_dir_x = -nx;
                    o.lateral_dir_z = -nz;
                    o.lateral_x = chop(double(o.lateral_x) - ox);
                    o.lateral_z = chop(double(o.lateral_z) - oz);
                } else {
                    p.pos_x = chop(ox + double(p.pos_x));
                    p.pos_z = chop(oz + double(p.pos_z));
                }
                break;
            }
        }
    }
}

void Race::update_positions() {
    // race_update_positions (races against the CPU): at most one swap per frame between the
    // player and a CPU car whose order disagrees with the track progress.
    if (!positions_running_) return;
    for (Opponent& o : opponents) {
        if ((player.track_progress - o.track_progress) * float(o.race_position - player_position) < 0.0f) {
            std::swap(o.race_position, player_position);
            return;
        }
    }
}

void Race::load_state(const uint8_t* pc, const uint8_t* cpu, size_t count) {
    auto i = [](const uint8_t* b, size_t o) { int32_t v; std::memcpy(&v, b + o, 4); return v; };
    auto f = [](const uint8_t* b, size_t o) { float v; std::memcpy(&v, b + o, 4); return v; };
    player.load_player_car(pc);
    player.track_marker = i(pc, 0x598);
    player.track_progress = f(pc, 0x680);
    player.lap = i(pc, 0x684);
    player_position = i(pc, 0x688);
    player.marker_flags = i(pc, 0x66c);  // surface
    player.surface_render_flag = i(pc, 0x7b0);
    opponents.assign(count, Opponent{});
    for (size_t k = 0; k < count; ++k) {
        const uint8_t* c = cpu + k * 0x590;
        Opponent& o = opponents[k];
        o.index = i(c, 0);
        o.prev_lateral_x = i(c, 0x160); o.prev_lateral_z = i(c, 0x168); o.prev_speed = f(c, 0x178);
        o.ground_y = i(c, 0x238); o.ground_pitch = f(c, 0x23c); o.ground_roll = f(c, 0x244);
        o.spline_index = i(c, 0x248); o.segment = i(c, 0x24c); o.t = f(c, 0x250); o.step = i(c, 0x254);
        o.line_x = i(c, 0x258); o.line_point = i(c, 0x25c); o.line_z = i(c, 0x260);
        o.ctrl_a_x = f(c, 0x264); o.ctrl_a_z = f(c, 0x26c);
        o.line_yaw = f(c, 0x27c); o.line_yaw_prev = f(c, 0x280);
        o.p[0] = f(c, 0x284); o.p[1] = f(c, 0x288); o.p[2] = f(c, 0x290); o.p[3] = f(c, 0x294);
        o.p[4] = f(c, 0x29c); o.p[5] = f(c, 0x2a0);
        o.eval_x = f(c, 0x2a8); o.eval_z = f(c, 0x2ac); o.dir_x = f(c, 0x2b4); o.dir_z = f(c, 0x2b8);
        o.catching_up = i(c, 0x2c0) != 0;
        o.def = {f(c, 0x2e4), f(c, 0x2e8), f(c, 0x2ec), f(c, 0x2f0), f(c, 0x2f4)};
        o.crash_state = i(c, 0x2f8); o.crash_bounce = f(c, 0x2fc); o.crash_bounce_count = i(c, 0x300);
        o.crash_pitch_rate = f(c, 0x304); o.crash_yaw_rate = f(c, 0x308); o.crash_roll_rate = f(c, 0x30c);
        o.crash_pitch = f(c, 0x314); o.crash_yaw = f(c, 0x318); o.crash_roll = f(c, 0x31c);
        o.track_marker = i(c, 0x320); o.marker_flags = i(c, 0x3f4); o.speed_factor = f(c, 0x3fc);
        o.track_progress = f(c, 0x408); o.lap = i(c, 0x40c); o.race_position = i(c, 0x410);
        o.active = i(c, 0x414) != 0; o.speed = f(c, 0x418);
        o.front_local_z = i(c, 0x424); o.rear_local_z = i(c, 0x430);
        o.front_x = i(c, 0x434); o.front_z = i(c, 0x43c); o.rear_x = i(c, 0x440); o.rear_z = i(c, 0x448);
        o.collision_radius = i(c, 0x44c); o.lateral_push = f(c, 0x450);
        o.lateral_x = i(c, 0x454); o.lateral_z = i(c, 0x45c);
        o.lateral_dir_x = f(c, 0x460); o.lateral_dir_z = f(c, 0x468); o.lateral_timer = f(c, 0x46c);
        o.pos_x = i(c, 0x498); o.pos_y = i(c, 0x49c); o.pos_z = i(c, 0x4a0);
        o.pitch = f(c, 0x4a4); o.yaw = f(c, 0x4a8); o.roll = f(c, 0x4ac);
        o.pitch_ofs = f(c, 0x4b0); o.roll_ofs = f(c, 0x4b8); o.draw_yaw = f(c, 0x4bc); o.yaw_wobble = f(c, 0x4c8);
        o.viewer_distance = f(c, 0x4e4);
        o.render_flags = uint32_t(i(c, 0x4e8));
    }
}

void Race::update(const Track& track, const CarInput& in, float frame_time, int pow_row) {
    dt_ = frame_time;
    pow_row_ = pow_row;
    positions_running_ = in.race_started;
    update_positions();  // race_update_positions runs before the cars move
    for (Opponent& o : opponents) update_position(o);
    // camera_set_listener ran at the end of the last frame: the player's position then.
    const int listener_x = player.pos_x, listener_z = player.pos_z;
    player.update(track, in, frame_time, pow_row);
    // car_apply_surface (in car_update_dynamics) still sees last frame's marker; the track
    // progress comes later in the frame (car_update_track_contact).
    if (player.crash_state == 0)
        player.surface_render_flag = player.marker_flags == 1 ? 0x8000 : player.marker_flags == 2 ? 0xff00 : 0;
    update_progress(markers, player.track_marker, player.lap, player.track_progress, player.marker_flags, nullptr,
                    player.pos_x, player.pos_z);
    for (Opponent& o : opponents) {
        // opponent_update_driving: only cars near the player are simulated in full.
        const double dx = double(chop(double(o.pos_x - listener_x) * 0.1)), dz = double(chop(double(o.pos_z - listener_z) * 0.1));
        if (std::abs(o.pos_x - listener_x) < 35000 && std::abs(o.pos_z - listener_z) < 35000) {
            o.viewer_distance = float(safe_sqrt(dx * dx + dz * dz) * 10.0);
            o.active = o.viewer_distance < 35000.0f;
        } else {
            o.active = false;
        }
        if (o.crash_state == 0 && in.cpu_cars_go) update_target_speed(o);
        advance_on_spline(o);
    }
    for (Opponent& o : opponents) {
        if (o.active) {
            smooth_angles(o);
            // opponent_update_steer_anim: the marker's flags as the body's +0x3c word (window
            // reflections: 0 open, 0x8000 / 0xff00 e.g. in tunnels).
            o.render_flags = o.marker_flags == 1 ? 0x8000u : o.marker_flags == 2 ? 0xff00u : 0u;
        }
        crash_update(o);
    }
    for (Opponent& o : opponents) {
        track_progress(o);
        if (o.active) bumpers(o);
        check_ground(track, o);
    }
    player_contacts(track);
    for (Opponent& o : opponents)
        if (o.crash_state == 0 && o.active) {
            cpu_contacts(o);
            collide_player(o);
        }
}

} // namespace scr

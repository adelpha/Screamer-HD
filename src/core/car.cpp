#include "core/car.h"

#include "core/sim_clock.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace scr {
namespace {

constexpr float kPi = 3.1415927410125732f;   // the game's float pi
constexpr float kTwoPi = 6.2831854820251465f;
constexpr float kHalfPi = 1.5707963705062866f;

float rd_f32(const uint8_t* p, size_t o) {
    float v;
    std::memcpy(&v, p + o, 4);
    return v;
}
int32_t rd_i32(const uint8_t* p, size_t o) {
    int32_t v;
    std::memcpy(&v, p + o, 4);
    return v;
}

int chop(double v) { return static_cast<int>(v); }  // __CHP: truncate toward zero
double safe_sqrt(double v) { return v >= 0.0 ? std::sqrt(v) : 0.0; }

// rotate_xz(out, in, angle): out.x = z sin a + x cos a, out.z = z cos a - x sin a.
void rotate_xz(int& ox, int& oz, int x, int z, float angle) {
    const double c = std::cos(double(angle)), s = std::sin(double(angle));
    ox = chop(double(z) * s + double(x) * c);
    oz = chop(double(z) * c - double(x) * s);
}

// Gear factors for the engine's pull (0x101d84, indexed by gear; gear 0 is unused).
constexpr float kGearPull[7] = {0.0f, 5.0f, 3.5f, 2.0f, 0.95f, 0.78f, 0.72f};

} // namespace

CarSetup car_setup(const uint8_t* d, const Model& model) {
    CarSetup s;
    s.animation = rd_i32(d, 0x0c);
    s.maxspd = rd_f32(d, 0x10);
    s.maxrpm = rd_f32(d, 0x14);
    s.grip = rd_f32(d, 0x18);
    s.accel = rd_f32(d, 0x1c);
    s.shiftmode = rd_i32(d, 0x20);
    s.n_shifts = std::clamp(rd_i32(d, 0x24), 1, 6);
    for (size_t i = 0; i < 6; ++i) s.sh_rap[i] = rd_f32(d, 0x28 + 4 * i);
    // Wheel order of PlayerCar.wheel: flx/fli, blx/rli, frx/fri, brx/rri (0x101d58); the
    // wheel position is the placement of the inner part.
    const char* inner[4] = {"fli", "rli", "fri", "rri"};
    for (size_t w = 0; w < 4; ++w)
        for (const Part& p : model.parts)
            if (p.name == inner[w]) s.wheel_pos[w] = {p.offset[0], p.offset[2]};
    return s;
}

void Car::reset(const CarSetup& su, const Track& track, int x, int z) {
    *this = Car{};
    setup = su;
    for (size_t i = 0; i < 4; ++i) {
        wheel[i].local_x = su.wheel_pos[i][0];
        wheel[i].local_z = su.wheel_pos[i][1];
        probe[i].local_x = wheel[i].local_x;
        probe[i].local_z = wheel[i].local_z + ((i & 1) == 0 ? -250 : 150);
    }
    track_width = -(wheel[0].local_x - wheel[2].local_x);
    wheelbase = -(wheel[0].local_z - wheel[1].local_z);
    rpm_per_speed = float(double(su.maxrpm / su.maxspd) * 0.02223);
    engine_drag = float(double(su.maxspd) * 0.03);
    brake_decel = float(double(su.maxspd) * 0.3);
    slide_threshold = float(double(su.maxspd) * 0.3);
    grip_limit = su.grip * 16000.0f;
    skid_brake_limit = su.grip * 26000.0f;
    max_accel = su.grip * 16000.0f;
    brake_skid_limit = su.grip * 11500.0f;

    pos_x = x;
    pos_z = z;
    if (const TrackPoint* p = nearest_track_point(track, x, z)) {
        float h = p->heading;
        if (kPi * 2 <= h) h += -kTwoPi;  // track_find_nearest_point keeps it in 0..2pi
        yaw = h;
    }
    prev_speed = speed;
    // car_update_track_contact, as player_car_load ends with it.
    corner_probes(track);
    resolve_corner_probes(track);
    wheel_ground(track);
    body_from_wheels();
}

float Car::lerp(float value, float target, int k) const {
    // lerp_toward: target + (value - target) * pow(0.5 + 0.005 k, 50 / (fps + 1))
    return (value - target) * pow_table(pow_row_, k) + target;
}

uint32_t Car::render_animation() const {
    // car_integrate_motion: CarData.animation | 0x4000 when not braking | 0x2000 when slow.
    return uint32_t(setup.animation) | (brake == 0.0f ? 0x4000u : 0u) | (speed <= 4000.0f ? 0x2000u : 0u);
}

void Car::update(const Track& track, const CarInput& in, float frame_time, int pow_row) {
    dt_ = frame_time;
    pow_row_ = pow_row;
    // race_frame order for the player: integrate, driving, dynamics, track contact.
    integrate_motion();
    engine_gearbox(in);
    update_throttle(in);
    update_brake(in);
    update_steering(in);
    wheel_loads();
    update_skid();
    // car_update_track_contact (progress and checkpoints are not ported)
    corner_probes(track);
    resolve_corner_probes(track);
    apply_wall_push(track);
    wheel_ground(track);
    update_track_heading(track);
    body_from_wheels();
}

void Car::integrate_motion() {
    if (pitch < 0.0f) pitch += kTwoPi;
    if (yaw < 0.0f) yaw += kTwoPi;
    if (roll < 0.0f) roll += kTwoPi;
    if (kTwoPi < yaw) yaw += -kTwoPi;
    prev_speed = speed;
    const float speed_factor = 1.0f;  // 0x300, network catch-up
    if (steer == 0.0f) {
        const float fx = float(std::sin(double(yaw)) * std::cos(double(pitch)));
        const float fz = float(-std::cos(double(pitch)) * std::cos(double(yaw)));
        pos_x = chop(double(fx * speed * dt_ * speed_factor) + double(pos_x));
        pos_z = chop(double(fz * speed * dt_ * speed_factor) + double(pos_z));
    } else {
        // Along the turning circle computed by car_update_steering last frame.
        const double cr = std::fabs(std::cos(double(roll)));
        pos_x = chop(cr * double(arc_dx) * speed_factor + double(pos_x));
        pos_z = chop(cr * double(arc_dz) * speed_factor + double(pos_z));
    }
    pos_x = chop(double(impact_vel_x * impact_timer * dt_) + double(pos_x));
    pos_z = chop(double(impact_vel_z * impact_timer * dt_) + double(pos_z));
    yaw_rate = pow_table(pow_row_, 0x140 / 4) * yaw_rate;
    yaw = yaw_rate * dt_ + yaw;
    impact_timer = pow_table(pow_row_, 0x16c / 4) * impact_timer;
    if (std::fabs(impact_timer) < 250.0f) impact_timer = 0.0f;
    // Sideways slide while skidding.
    const double slide = safe_sqrt(std::fabs(std::cos(double(roll))) * 2.0);
    pos_x = chop(slide * dt_ * double(skid_level * slip_a) + double(pos_x));
    pos_z = chop(slide * dt_ * double(skid_level * slip_b) + double(pos_z));
}

void Car::engine_gearbox(const CarInput& in) {
    const float maxrpm = setup.maxrpm;
    if (setup.shiftmode == 0) {
        if (in.gear_up) {
            if (gear < setup.n_shifts && !gear_latch) { ++gear; gear_latch = true; }
        } else if (in.gear_down) {
            if (gear > 0 && !gear_latch) { --gear; gear_latch = true; }
        } else {
            gear_latch = false;
        }
    } else {
        if (float(gear + 8500) < engine_rpm) {
            if (gear < setup.n_shifts) ++gear;
        } else if (engine_rpm < float(4000 - gear) && gear > 1) {
            --gear;
        }
    }
    if (gear != 0) {
        const float ratio = setup.sh_rap[size_t(gear - 1)];
        speed_limit = float(double(setup.maxspd) - (1.0 + std::sin(double(pitch)) * 4.0) * 1000.0);
        if (setup.grip * 6000.0f < engine_rpm && speed < 2000.0f && in.accel >= 0xc9) {
            // Wheelspin at the start (car_set_state): the revs drop instead of following speed.
            engine_rpm -= dt_ * 500.0f;
        } else {
            engine_rpm = ratio * speed * rpm_per_speed;
        }
        const float top = maxrpm / (rpm_per_speed * ratio);  // top speed in this gear
        float pull, gain;
        if (setup.n_shifts == 1) {
            gain = setup.accel;
            pull = float(safe_sqrt(double((top - speed) / top)));
        } else {
            float t = float(2.0 - double(top - speed) / (double(top) * 0.5) + -1.0);
            if (gear == 1 && t < 0.0f) t = 0.2f;
            pull = float((std::sin(double(t * kHalfPi)) + 1.0) * 0.5);
            gain = setup.accel * kGearPull[std::min(gear, 6)];
        }
        if (engine_rpm < throttle * maxrpm) {
            engine_rpm = float((double(throttle * gain * pull) - std::sin(double(pitch)) * 100.0) * dt_ + double(engine_rpm));
        }
        const float rpm = engine_rpm;
        if (maxrpm < rpm) {
            engine_rpm = float(double(engine_rpm) -
                               (std::sin(double(pitch)) * 100.0 + 4000.0 + (double(rpm) - double(maxrpm)) * 2.0) * dt_);
            if (double(engine_rpm) < double(maxrpm) * double(throttle)) engine_rpm = maxrpm * throttle;
        } else if (throttle * maxrpm < rpm) {
            engine_rpm = float(double(engine_rpm) - safe_sqrt(double(rpm / maxrpm)) * double(engine_drag) * dt_);
            if (double(engine_rpm) < double(maxrpm) * double(throttle)) engine_rpm = maxrpm * throttle;
        }
        const float old = speed;
        speed = engine_rpm / (rpm_per_speed * ratio);
        if (speed_limit < speed) speed = speed_limit;
        if (max_accel * dt_ < old - speed) braking = 1.0f;
    } else {
        speed -= dt_ * 500.0f;
        if (speed < 0.0f) speed = 0.0f;
        engine_rpm = lerp(engine_rpm, throttle * maxrpm, 0x4b);
    }
    if (speed < dt_ * 500.0f) gear = 0;
}

void Car::update_throttle(const CarInput& in) {
    if (0.0f <= throttle && skid == 0.0f && braking == 0.0f && in.accel > 0) {
        throttle = float(std::abs(in.accel)) / 1023.0f;
    } else {
        throttle = 0.0f;
    }
}

void Car::update_brake(const CarInput& in) {
    if (in.brake > 0) {
        brake = float(std::abs(in.brake)) / 1023.0f;  // digital/keyboard path
    } else {
        brake = 0.0f;
    }
    if (drift_angle < 0.1f) {
        speed = speed - brake_decel * dt_ * brake;
        if (speed < 0.0f) speed = 0.0f;
        if (brake_skid_limit < brake * speed) braking = float(int(brake));  // brake_mode 0
    }
}

void Car::update_steering(const CarInput& in) {
    const int k = (braking == 0.0f && skid == 0.0f) ? 0x50 : 0x55;
    steer_max = float((1.0 - safe_sqrt(double(speed / setup.maxspd))) * 0.2967059910297394 + 0.01745329238474369);
    const float mirror = 1.0f;  // g_race_options & 0x10 (MIRRO cheat) gives -1
    if (in.analog_steer) {
        const float target = -(float(in.steer) * 0.0009765625f * steer_max) * mirror;
        steer = (k == 0x50) ? target : lerp(steer, target, k);
    } else {
        float target = -(float(in.steer_left) * 0.0009765625f * steer_max) * mirror;
        target = float(in.steer_right) * 0.0009765625f * steer_max * mirror + target;
        steer = lerp(steer, -target, k);
        if (std::fabs(steer) < 1e-05f) steer = 0.0f;
    }
    if (bumpy_front) {
        // Bumpy surface ('e') under a front wheel shakes the steering.
        const int h = pos_x + pos_z;
        const double shake = double(h & 0x3f) * 0.0008 * ((h & 1) == 0 ? 1.0 : -1.0);
        steer = float(safe_sqrt(1.0 - double(speed / setup.maxspd)) * shake + double(steer));
        if (steer_max < std::fabs(steer)) steer = (steer < 0.0f ? -1.0f : 1.0f) * steer_max;
    }
    if ((0.0f < steer && probe[0].hit) || (steer < 0.0f && probe[2].hit)) steer = 0.0f;
    if (steer != 0.0f) {
        turn_radius = float(double(wheelbase) / std::tan(double(steer)));
        turn_rate = (dt_ * speed) / turn_radius;
        yaw = turn_rate + yaw;
        int ax, az, bx, bz;
        rotate_xz(ax, az, chop(turn_radius), wheel[3].local_z, -yaw);
        rotate_xz(bx, bz, ax, az, -turn_rate);
        arc_dx = ax - bx;
        arc_dz = az - bz;
    } else {
        turn_radius = 0.0f;
        turn_rate = 0.0f;
    }
}

void Car::wheel_loads() {
    const float accel_load = float(double(speed - prev_speed) * 0.00175);
    const float corner_load = float(double(speed * speed) * 1.5e-09) * (turn_rate / dt_);
    int lift_sum = 0;
    for (size_t i = 0; i < 4; ++i) {
        CarWheel& w = wheel[i];
        const float lng = (i & 1) == 0 ? -accel_load : accel_load * 3.0f;
        const float lat = (i & 2) == 0 ? -corner_load : corner_load;
        if (std::fabs(w.load_lat) < std::fabs(lat / dt_)) {
            w.load_time = 0;
            w.load_lat = lat / dt_;
        }
        if (std::fabs(w.load_long) < std::fabs(lng / dt_)) {
            w.load_time = 0;
            w.load_long = lerp(w.load_long, lng / dt_, 0x50);
        }
        const float load = w.load_lat + w.load_long;
        w.lift = chop(std::cos(double(w.load_time * 12.566370964f) + double(kPi)) * double(load));
        w.lift = std::clamp(w.lift, -18, 18);
        w.load_lat = pow_table(pow_row_, 0xf4 / 4) * w.load_lat;
        if (std::fabs(w.load_lat) < 2.0f) w.load_lat = 0.0f;
        w.load_long = pow_table(pow_row_, 0x11c / 4) * w.load_long;
        if (std::fabs(w.load_long) < 2.0f) w.load_long = 0.0f;
        w.load_time = dt_ + w.load_time;
        lift_sum += w.lift;
    }
    body_lift = lift_sum / 4;
    lean_roll = float(-std::atan(double(float(wheel[0].lift - wheel[2].lift)) / double(track_width)));
    lean_pitch = float(std::atan(double(float(wheel[2].lift - wheel[3].lift)) / double(wheelbase)));
}

void Car::update_skid() {
    bool skidding = false;
    if (braking == 0.0f) {
        const float sign = turn_rate < 0.0f ? -1.0f : 1.0f;
        const int bumps = (int(wheel[0].bumpy) + int(wheel[2].bumpy)) * 1000 + (int(wheel[1].bumpy) + int(wheel[3].bumpy)) * 600;
        float force = float(bumps) * sign +
                      float((double(speed * speed * turn_rate) * 0.0001) / dt_) * surface_grip;
        if (std::fabs(force) <= grip_limit) {
            skid = 0.0f;
            skid_prev = 0.0f;
        } else if ((brake + 0.5f) * force <= skid_brake_limit) {
            force = force <= 0.0f ? force + grip_limit : force - grip_limit;
            const float side = yaw + -kHalfPi;
            skid_prev = float(double(force) * 0.00015);
            if (1.539380443096161 < std::fabs(double(skid_prev)))
                skid_prev = float((force < 0.0f ? -1.0 : 1.0) * 1.539380443096161);
            skid = float(double(speed * speed) / double(turn_radius) * (double(surface_grip) * 0.2));
            slip_a = float(std::sin(double(side)));
            slip_b = float(-std::cos(double(side)));
        } else {
            // spin_state = 1: spins are not ported; treat as a hard skid.
            skid_prev = 0.0f;
        }
    } else {
        braking = pow_table(pow_row_, 0x188 / 4) * braking;
        if (braking * speed < 2000.0f) braking = 0.0f;
        const float strength = (braking * speed) / setup.maxspd;
        const float sign = turn_rate < 0.0f ? -1.0f : 1.0f;
        skid_prev = lerp(skid_prev, sign * strength * dt_ * 7.0f, 0x50);
        yaw = float(double(skid_prev) * 0.2 + double(yaw));
        skidding = true;
    }
    // value_lerp_toward(drift_angle, sqrt(|cos roll| * 2) * skid_prev, 0x5a)
    const float target = float(safe_sqrt(std::fabs(std::cos(double(roll))) * 2.0) * double(skid_prev));
    if (std::fabs(target - drift_angle) < kHalfPi)
        drift_angle = float(std::sin(double(drift_angle - target)) * pow_table(pow_row_, 0x5a) + double(target));
    else
        drift_angle = (drift_angle - target) * pow_table(pow_row_, 0x5a) + target;
    skid_level = lerp(skid_level, skid, 0x50);
    if (skid_level < 10.0f) skid_level = 0.0f;
    (void)skidding;
}

void Car::corner_probes(const Track& track) {
    const double cr = std::fabs(std::cos(double(roll))), cp = std::fabs(std::cos(double(pitch)));
    wall_contact = false;
    for (CarProbe& p : probe) {
        int ox, oz;
        rotate_xz(ox, oz, chop(double(p.local_x) * cr), chop(double(p.local_z) * cp), -yaw);
        p.x = pos_x + ox;
        p.z = pos_z + oz;
        p.hit = !ground_query(track, p.x, p.z).has_value();
        if (p.hit) wall_contact = true;
    }
}

void Car::resolve_corner_probes(const Track& track) {
    if (!wall_contact) return;
    for (CarProbe& p : probe) {
        if (!p.hit) {
            p.push_x = p.push_z = 0;
            p.heading = 0;
            continue;
        }
        const TrackSection* s = nearest_track_section(track, p.x, p.z);
        if (!s) continue;
        const int rx = p.x - s->x, rz = p.z - s->z;
        int lx, lz;
        rotate_xz(lx, lz, rx, rz, -s->heading);
        lx = (lx < 0 ? 1 : -1) * 30;
        int bx, bz;
        rotate_xz(bx, bz, lx, lz, s->heading);
        p.push_x = bx - rx;
        p.push_z = bz - rz;
        p.heading = float(int(s->heading));  // stored as an int in the game
    }
}

void Car::apply_wall_push(const Track& track) {
    if (!wall_contact) return;
    wall_push_x = wall_push_z = 0;
    for (const CarProbe& p : probe) {
        if (!p.hit) continue;
        if (std::abs(wall_push_x) < std::abs(p.push_x)) { wall_push_x = p.push_x; wall_heading = p.heading; }
        if (std::abs(wall_push_z) < std::abs(p.push_z)) { wall_push_z = p.push_z; wall_heading = p.heading; }
    }
    pos_x += wall_push_x;
    pos_z += wall_push_z;
    wheel_ground(track);
}

void Car::wheel_ground(const Track& track) {
    const double cr = std::fabs(std::cos(double(roll))), cp = std::fabs(std::cos(double(pitch)));
    bumpy_front = false;
    for (size_t i = 0; i < 4; ++i) {
        CarWheel& w = wheel[i];
        const int lx = chop(double(w.local_x) * cr), lz = chop(double(w.local_z) * cp);
        int ox, oz;
        rotate_xz(ox, oz, lx, lz, -(yaw + drift_angle));
        w.x = pos_x + ox;
        w.z = pos_z + oz;
        if (auto g = ground_query(track, w.x, w.z)) {
            w.ground_y = g->y;
            w.bumpy = g->surface == 'e';
            if ((i & 1) == 0 && w.bumpy) bumpy_front = true;
        }
        rotate_xz(ox, oz, lx, lz, -yaw);
        w.x2 = pos_x + ox;
        w.z2 = pos_z + oz;
        if (auto g = ground_query(track, w.x2, w.z2)) w.ground_y2 = g->y;
    }
}

void Car::update_track_heading(const Track& track) {
    if (const TrackPoint* p = nearest_track_point(track, pos_x, pos_z)) track_heading = p->heading;
    if (!wall_contact) return;
    const float hit = float(std::fabs(std::sin(double(yaw + wall_heading))));
    float diff = std::fabs(yaw - track_heading);
    while (kPi < diff) diff += -kPi;
    if (kHalfPi < diff) diff = kPi - diff;
    diff = diff / kHalfPi;
    (void)diff;  // speed * diff > slide_threshold starts a crash (car_set_state), not ported
    const float len = float(safe_sqrt(double(wall_push_x) * wall_push_x + double(wall_push_z) * wall_push_z));
    if (len > 0.0f) {
        impact_vel_x = float(wall_push_x) / len;
        impact_vel_z = float(wall_push_z) / len;
    }
    impact_timer = float(std::pow(double(hit), 2.0) * double(speed * 2.0f) + 2000.0);
    if (5000.0f < impact_timer) impact_timer = 5000.0f;
    speed = float(double(speed) - double(speed) * 0.9 * double(hit));
    if (speed <= 0.0f) speed = 0.0f;
    yaw_rate = float(int(probe[2].hit) - int(probe[0].hit)) * hit * 5.0f;
    braking = 0.0f;
    skid_prev = 0.0f;
    skid = 0.0f;
    drift_angle = 0.0f;
}

void Car::body_from_wheels() {
    int ground_sum = 0, y_sum = 0;
    for (CarWheel& w : wheel) {
        w.y = w.ground_y;
        w.y2 = w.ground_y2;
        if (w.bumpy) {
            const int h = w.x + w.z;
            w.y += ((h & 2) == 0 ? 1 : -1) * (h & 0xf);
        }
        ground_sum += w.ground_y;
        y_sum += w.y;
    }
    ground_y = ground_sum / 4;
    pos_y = y_sum / 4;
    roll = float(-std::atan(double(float(wheel[0].y - wheel[2].y)) / double(track_width)));
    pitch = float(std::atan(double(float(wheel[2].y - wheel[3].y)) / double(wheelbase)));
    ground_roll = float(-std::atan(double(float(wheel[0].y2 - wheel[2].y2)) / double(track_width)));
    ground_pitch = float(std::atan(double(float(wheel[2].y2 - wheel[3].y2)) / double(wheelbase)));
}

void ChaseCamera::update(const Car& car, float frame_time, int pow_row, int view) {
    (void)frame_time;
    int height = 300, dist = 1300;
    float pitch_target = car.ground_pitch + 6.094689917564392f;
    if (view == 9) { dist = 1700; pitch_target = car.ground_pitch + 6.157521772384643f; }
    if (view == 6) { height = 100; dist = 1400; pitch_target = car.ground_pitch + 6.157521772384643f; }
    auto angle_lerp = [&](float& v, float target, int k) {
        // angle_lerp_toward: the short way round, result kept in 0..2pi
        float d = v - target;
        while (d > kPi) d -= kTwoPi;
        while (d < -kPi) d += kTwoPi;
        v = d * pow_table(pow_row, k) + target;
        if (v < 0.0f) v += kTwoPi;
        if (kTwoPi <= v) v += -kTwoPi;
    };
    const int y_target = chop(double(float(car.pos_y + height)) - std::sin(double(pitch)) * double(dist));
    if (snap) {
        y = y_target;
        yaw = car.yaw;
        roll = car.ground_roll;
        pitch = pitch_target;
        snap = false;
    } else {
        y = chop((double(y) - double(y_target)) * pow_table(pow_row, 0x51) + double(y_target));
        angle_lerp(yaw, car.yaw, 0x51);
        angle_lerp(roll, car.ground_roll, 0x51);
        angle_lerp(pitch, pitch_target, 0x51);
    }
    const double horiz = std::cos(double(pitch)) * double(dist);
    x = chop(double(car.pos_x) - std::sin(double(yaw)) * horiz);
    z = chop(std::cos(double(yaw)) * horiz + double(car.pos_z));
}

} // namespace scr

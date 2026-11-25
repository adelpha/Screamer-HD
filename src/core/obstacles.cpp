#include "core/obstacles.h"

#include "core/archive.h"
#include "core/dtr.h"
#include "core/opponent.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace scr {

namespace {

constexpr float kPi = 3.141592741f;


} // namespace

bool Obstacles::load(const std::string& data_dat, const std::string& gfx_dat, const std::string& track_name,
                     const std::string& spline, const Track& track, int column) {
    list.clear();
    models.clear();
    hits = 0;
    auto a = Archive::open(data_dat);
    if (!a) return false;
    auto dtr = a->read(track_name + "DAT.DTR");
    auto spl = a->read(track_name + spline + ".SPL");
    if (!dtr || !spl) return false;
    auto values = DtrFile(std::string(dtr->begin(), dtr->end())).section("OSTACLES");
    if (!values) return false;
    std::vector<std::string> names;  // the first name of each row (g_race_options & 3 = 0)
    column = std::clamp(column, 0, 2);
    for (size_t i = 0; i + 3 <= values->size(); i += 3) names.push_back((*values)[i + size_t(column)].text);
    const auto lines = parse_splines(*spl);
    if (lines.empty() || lines[0].segments.empty()) return false;
    const RacingLine& line = lines[0];
    std::map<int, int> loaded;  // OSTACLES row -> model
    int k = line.start;
    for (size_t n = 0; n < line.segments.size(); ++n) {
        if (k < 0 || size_t(k) >= line.segments.size()) break;
        const SplineSegment& s = line.segments[size_t(k)];
        const int row = s.point;
        if (row >= 0 && size_t(row) < names.size()) {
            auto it = loaded.find(row);
            if (it == loaded.end()) {
                std::string file = names[size_t(row)];
                for (char& c : file) c = char(std::toupper(static_cast<unsigned char>(c)));
                auto m = load_object(gfx_dat, file + ".3DX");
                it = loaded.emplace(row, m ? int(models.size()) : -1).first;
                if (m) models.push_back(std::move(*m));
            }
            if (it->second >= 0) {
                Obstacle o;
                o.x = s.x;
                o.z = s.z;
                o.model = it->second;
                // on the ground, facing along the track, tilted with the ground
                const auto g = ground_query(track, o.x, o.z);
                if (g) o.y = g->y;
                if (const TrackPoint* p = nearest_track_point(track, o.x, o.z)) o.ay = p->heading;
                if (g) {
                    int lx, lz;
                    rotate_xz(lx, lz, g->normal_x, g->normal_z, o.ay + -1.570796371f);
                    o.ax = float(lx) * kPi / 46080.0f;
                    o.az = float(lz) * kPi / 46080.0f;
                }
                list.push_back(o);
            }
        }
        k = s.next;
    }
    return !list.empty();
}

int Obstacles::collide(Car& car) {
    // car_check_obstacle_hits: either bumper within the car's radius + 200
    int n = 0;
    if (car.crash_state != 0) return 0;
    car.update_bumpers();
    for (Obstacle& o : list) {
        if (o.hit || std::abs(car.pos_x - o.x) >= 0x5dc || std::abs(car.pos_z - o.z) >= 0x5dc ||
            std::abs(car.pos_y - o.y) >= 0x5dc)
            continue;
        const int limit = car.collision_radius + 100;
        const int pts[2][2] = {{car.front_x, car.front_z}, {car.rear_x, car.rear_z}};
        for (const auto& p : pts) {
            const double dx = double(p[0] - o.x), dz = double(p[1] - o.z);
            const int dist = int(std::sqrt(dx * dx + dz * dz));
            if (dist - limit < 100) {
                o.vx = float(std::sin(double(car.yaw)));
                o.vz = float(-std::cos(double(car.yaw)));
                o.lift = float(double(car.speed) * 0.02);
                o.speed = float(double(car.speed) * 1.4);
                if (!o.flying) {
                    ++hits;
                    ++n;
                }
                o.flying = o.hit = true;
            }
        }
    }
    return n;
}

void Obstacles::update(const Track& track, float dt) {
    // obstacles_update
    for (Obstacle& o : list) {
        if (!o.hit) continue;
        const int ox = o.x, oz = o.z;
        o.x = int(o.speed * o.vx * dt + float(o.x));
        o.z = int(o.speed * o.vz * dt + float(o.z));
        o.ax = float(double(o.ax) - double(o.speed * o.vx * dt) * 0.008);
        o.ay = float(double(o.ay) - double(o.speed * o.vy_spin * dt) * 0.004);
        o.az = float(double(o.az) - double(o.speed * o.vz * dt) * 0.0015);
        o.speed -= dt * 2000.0f;
        if (o.speed < 0) o.speed = 0;
        auto g = ground_query(track, o.x, o.z);
        int ground = g ? g->y : o.y;
        const bool off_road = !g;
        if (off_road) {
            // bounced back off the edge of the road
            o.x = ox;
            o.z = oz;
            o.vx = -o.vx;
            o.vz = -o.vz;
            o.speed = float(double(o.speed) * 0.3);
        }
        o.lift -= dt * 500.0f;
        o.y = int(float(o.y) + o.lift);
        if (o.y <= ground) {
            o.y = ground;
            o.lift = float(-(double(o.lift) * 0.8));
            if (std::fabs(o.lift) < 50.0f) o.lift = 0;
            if (o.speed == 0 && o.lift == 0) {
                if (!off_road && g) {
                    // comes to rest on its side
                    int lx, lz;
                    rotate_xz(lx, lz, g->normal_x, g->normal_z, o.ay + -1.570796371f);
                    o.ax = float(lx) * kPi / 46080.0f + 1.570796371f;
                    o.hit = false;
                } else {
                    o.y = -20000;  // gone
                }
            } else {
                o.hit = true;
            }
        }
    }
}

} // namespace scr

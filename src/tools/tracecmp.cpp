#include "../debug_report.h"
// tracecmp: replays a recording of the original game's player car (tools/dosbox/record-race.ps1)
// through the ported physics, one frame at a time: each frame starts from the recorded state,
// runs Car::update with that frame's controls and frame time, and is compared with the next
// recorded state. Prints the first frames and fields that differ.
//
// Usage: tracecmp TRACE GFX/MAPnH.DAT MAPn [--from N] [--to N] [--all] [--max-diff N]
// Exit code 0 when at most N frames (default 0) differ.

#include "core/car.h"
#include "core/opponent.h"
#include "core/track.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

struct Record {
    float frame_time;
    int pow_row;
    uint32_t flags;
    int accel, brake, left, right;
    std::vector<uint8_t> car;
};

std::vector<Record> load_trace(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    std::vector<Record> out;
    if (d.size() < 16 || std::memcmp(d.data(), "SCRTRACE", 8) != 0) return out;
    uint32_t rs;
    std::memcpy(&rs, d.data() + 12, 4);
    for (size_t o = 16; o + rs <= d.size(); o += rs) {
        Record r;
        std::memcpy(&r.frame_time, d.data() + o, 4);
        std::memcpy(&r.pow_row, d.data() + o + 4, 4);
        std::memcpy(&r.flags, d.data() + o + 8, 4);
        std::memcpy(&r.accel, d.data() + o + 12, 4);
        std::memcpy(&r.brake, d.data() + o + 16, 4);
        std::memcpy(&r.left, d.data() + o + 20, 4);
        std::memcpy(&r.right, d.data() + o + 24, 4);
        r.car.assign(d.begin() + std::ptrdiff_t(o + 28), d.begin() + std::ptrdiff_t(o + rs));
        out.push_back(std::move(r));
    }
    return out;
}

struct Field {
    const char* name;
    double ours, game;
    double tolerance;
};

} // namespace

int main(int argc, char** argv) {
    install_debug_report(true);
    if (argc < 4) {
        std::fprintf(stderr, "usage: tracecmp TRACE GFX/MAPnH.DAT MAPn [--from N] [--to N] [--all]\n");
        return 2;
    }
    auto trace = load_trace(argv[1]);
    auto track = scr::load_track(argv[2], argv[3]);
    if (trace.size() < 2 || !track) {
        std::fprintf(stderr, "cannot load the trace or the track\n");
        return 1;
    }
    size_t from = 0, to = trace.size() - 1;
    bool all = false;
    int max_diff = 0;
    std::string data_dat;
    for (int i = 4; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--from") && i + 1 < argc) from = size_t(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--to") && i + 1 < argc) to = size_t(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--all")) all = true;
        else if (!std::strcmp(argv[i], "--max-diff") && i + 1 < argc) max_diff = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--data") && i + 1 < argc) data_dat = argv[++i];
    }
    constexpr size_t kCar = 0x800, kCpu = 0x590;
    if (!data_dat.empty() && trace[0].car.size() >= kCar + kCpu) {
        // Recordings with g_opponents: step the whole race (player and CPU cars).
        const size_t n_cpu = (trace[0].car.size() - kCar) / kCpu;
        scr::Race race;
        if (!race.load(data_dat, argv[3])) {
            std::fprintf(stderr, "cannot load the racing lines from %s\n", data_dat.c_str());
            return 1;
        }
        int bad = 0, cmp = 0;
        for (size_t k = from; k < to && k + 1 < trace.size(); ++k) {
            const Record& r = trace[k];
            race.load_state(r.car.data(), r.car.data() + kCar, n_cpu);
            scr::CarInput in;
            in.accel = r.accel;
            in.brake = r.brake;
            in.steer_left = r.left;
            in.steer_right = r.right;
            in.race_started = (r.flags & 1) != 0;
            in.cpu_cars_go = (r.flags & 5) != 0;
            race.update(*track, in, r.frame_time, r.pow_row);
            scr::Race next;
            next.load_state(trace[k + 1].car.data(), trace[k + 1].car.data() + kCar, n_cpu);
            ++cmp;
            bool frame_bad = false;
            for (size_t c = 0; c < n_cpu; ++c) {
                const scr::Opponent& a = race.opponents[c];
                const scr::Opponent& g = next.opponents[c];
                const Field fields[] = {
                    {"pos_x", double(a.pos_x), double(g.pos_x), 0},
                    {"pos_y", double(a.pos_y), double(g.pos_y), 0},
                    {"pos_z", double(a.pos_z), double(g.pos_z), 0},
                    {"line_x", double(a.line_x), double(g.line_x), 0},
                    {"line_z", double(a.line_z), double(g.line_z), 0},
                    {"t", a.t, g.t, 1e-6},
                    {"segment", double(a.segment), double(g.segment), 0},
                    {"speed", a.speed, g.speed, 0.01},
                    {"yaw", a.yaw, g.yaw, 1e-5},
                    {"draw_yaw", a.draw_yaw, g.draw_yaw, 1e-5},
                    {"pitch", a.pitch, g.pitch, 1e-5},
                    {"roll", a.roll, g.roll, 1e-5},
                    {"lateral_x", double(a.lateral_x), double(g.lateral_x), 0},
                    {"lateral_z", double(a.lateral_z), double(g.lateral_z), 0},
                    {"crash_state", double(a.crash_state), double(g.crash_state), 0},
                    {"active", double(a.active), double(g.active), 0},
                    {"speed_factor", a.speed_factor, g.speed_factor, 1e-6},
                    {"race_position", double(a.race_position), double(g.race_position), 0},
                    {"render_flags", double(a.render_flags), double(g.render_flags), 0},
                };
                bool cbad = false;
                for (const Field& fl : fields)
                    if (std::fabs(fl.ours - fl.game) > fl.tolerance) cbad = true;
                if (!cbad) continue;
                frame_bad = true;
                if (bad >= 12 && !all) continue;
                std::printf("frame %zu cpu %zu:", k, c);
                for (const Field& fl : fields)
                    if (std::fabs(fl.ours - fl.game) > fl.tolerance) std::printf("  %s %.6g vs %.6g", fl.name, fl.ours, fl.game);
                std::printf("\n");
            }
            {
                const scr::Car& a = race.player;
                const scr::Car& g = next.player;
                const Field fields[] = {
                    {"player pos_x", double(a.pos_x), double(g.pos_x), 0},
                    {"player pos_z", double(a.pos_z), double(g.pos_z), 0},
                    {"player speed", a.speed, g.speed, 0.01},
                    {"player yaw_rate", a.yaw_rate, g.yaw_rate, 1e-4},
                    {"player impact_timer", a.impact_timer, g.impact_timer, 0.5},
                    {"player position", double(race.player_position), double(next.player_position), 0},
                    {"player surface flag", double(a.surface_render_flag), double(g.surface_render_flag), 0},
                };
                bool pbad = false;
                for (const Field& fl : fields)
                    if (std::fabs(fl.ours - fl.game) > fl.tolerance) pbad = true;
                if (pbad) {
                    frame_bad = true;
                    if (bad < 12 || all) {
                        std::printf("frame %zu:", k);
                        for (const Field& fl : fields)
                            if (std::fabs(fl.ours - fl.game) > fl.tolerance) std::printf("  %s %.6g vs %.6g", fl.name, fl.ours, fl.game);
                        std::printf("%c", 10);
                    }
                }
            }
            if (frame_bad) ++bad;
        }
        std::printf("%d of %d frames differ (CPU cars)\n", bad, cmp);
        return bad > max_diff ? 1 : 0;
    }
    int bad_frames = 0, compared = 0;
    for (size_t k = from; k < to && k + 1 < trace.size(); ++k) {
        const Record& r = trace[k];
        scr::Car car;
        car.load_player_car(r.car.data());
        scr::CarInput in;
        in.accel = r.accel;
        in.brake = r.brake;
        in.steer_left = r.left;
        in.steer_right = r.right;
        in.race_started = (r.flags & 1) != 0;
        car.update(*track, in, r.frame_time, r.pow_row);
        scr::Car game;
        game.load_player_car(trace[k + 1].car.data());
        const Field fields[] = {
            {"pos_x", double(car.pos_x), double(game.pos_x), 0},
            {"pos_y", double(car.pos_y), double(game.pos_y), 0},
            {"pos_z", double(car.pos_z), double(game.pos_z), 0},
            {"yaw", car.yaw, game.yaw, 1e-5},
            {"pitch", car.pitch, game.pitch, 1e-5},
            {"roll", car.roll, game.roll, 1e-5},
            {"speed", car.speed, game.speed, 0.01},
            {"engine_rpm", car.engine_rpm, game.engine_rpm, 0.5},
            {"gear", double(car.gear), double(game.gear), 0},
            {"throttle", car.throttle, game.throttle, 1e-6},
            {"brake", car.brake, game.brake, 1e-6},
            {"steer", car.steer, game.steer, 1e-6},
            {"turn_rate", car.turn_rate, game.turn_rate, 1e-6},
            {"drift_angle", car.drift_angle, game.drift_angle, 1e-5},
            {"skid_level", car.skid_level, game.skid_level, 0.01},
            {"yaw_rate", car.yaw_rate, game.yaw_rate, 1e-4},
            {"impact_timer", car.impact_timer, game.impact_timer, 0.5},
            {"crash_state", double(car.crash_state), double(game.crash_state), 0},
            {"body_lift", double(car.body_lift), double(game.body_lift), 0},
            {"lean_pitch", car.lean_pitch, game.lean_pitch, 1e-5},
            {"lean_roll", car.lean_roll, game.lean_roll, 1e-5},
            {"wall_contact", double(car.wall_contact), double(game.wall_contact), 0},
            {"ground_y", double(car.ground_y), double(game.ground_y), 0},
        };
        ++compared;
        bool bad = false;
        for (const Field& f : fields)
            if (std::fabs(f.ours - f.game) > f.tolerance) bad = true;
        if (!bad) continue;
        if (++bad_frames > 12 && !all) continue;
        std::printf("frame %zu (flags %x, in %d/%d/%d/%d, ft %.4f row %d):", k, r.flags, r.accel, r.brake, r.left, r.right,
                    double(r.frame_time), r.pow_row);
        for (const Field& f : fields)
            if (std::fabs(f.ours - f.game) > f.tolerance) std::printf("  %s %.6g vs %.6g", f.name, f.ours, f.game);
        std::printf("\n");
        if (car.wall_contact || game.wall_contact)
            std::printf("    wall: ours push %d,%d heading %g, game push %d,%d heading %g\n", car.wall_push_x, car.wall_push_z,
                        double(car.wall_heading), game.wall_push_x, game.wall_push_z, double(game.wall_heading));
        if (car.wall_contact != game.wall_contact || car.wall_push_x != game.wall_push_x || car.wall_push_z != game.wall_push_z)
            for (size_t i = 0; i < 4; ++i)
                std::printf("    probe %zu: ours %d,%d %s, game %d,%d %s\n", i, car.probe[i].x, car.probe[i].z,
                            car.probe[i].hit ? "off" : "on", game.probe[i].x, game.probe[i].z, game.probe[i].hit ? "off" : "on");
    }
    std::printf("%d of %d frames differ\n", bad_frames, compared);
    return bad_frames > max_diff ? 1 : 0;
}

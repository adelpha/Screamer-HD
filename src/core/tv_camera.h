#pragma once
// The TV camera of the intro demo (camera_mode_tv, camera_tv_track_opponent): every CPU car
// keeps the nearest of the track's fixed cameras (MAPnCAM.SPL) aimed at it; the picture shows
// one car's and moves on to another every 10 seconds, at that car's next cut.
//
// A camera point is a spline knot: x, z, and the height in the knot's +0xc field, whose last
// digit says how the camera behaves (1: zooms in to 1500 units from the car, 2: zooms while
// the car drives away, 4: while it comes closer; otherwise it stays at the point).

#include "core/opponent.h"

#include <vector>

namespace scr {

class TvCamera {
public:
    // The knots of MAPnCAM.SPL (all lines).
    void load(const std::vector<RacingLine>& lines);
    bool empty() const { return points_.empty(); }

    // Once a frame (attract_mode_update_cars: camera_update after the cars moved); ticks of
    // the 100 Hz clock since the last call (idle_timer_check).
    void update(const std::vector<Opponent>& cars, int ticks);

    // Replays (camera_mode_trackside, view 3): the knots of MAPnRPL.SPL; the nearest one to
    // the car, zoomed in to 1500 units from it.
    void load_trackside(const std::vector<RacingLine>& lines);
    bool has_trackside() const { return !trackside_.empty(); }
    void trackside(int car_x, int car_y, int car_z);

    // The picture's camera (radians, the game's camera conventions).
    int x = 0, y = 0, z = 0;
    float pitch = 0, yaw = 0;
    int followed = 0;  // DAT_00110a14

private:
    struct Point {
        int x, y, z;
    };
    struct PerCar {
        int point = -1, prev_point = -1;
        double prev_dist = 0;
        bool target = false, cut = false;
        int x = 0, y = 0, z = 0;
        float pitch = 0, yaw = 0;
    };
    void track(const Opponent& o, PerCar& c);
    std::vector<Point> points_, trackside_;
    std::vector<PerCar> cars_;
    int next_ = -1;          // DAT_00110a18
    int idle_ticks_ = 1000;  // idle_timer_check: a new car every 1000 ticks (the first at once)
};

} // namespace scr

#pragma once
// Track-side obstacles: cones, bunnies, skittles, barrels... (track_load_obstacles,
// obstacles_update, car_check_obstacle_hits in STARTL.EXE).
//
// OSTACLES in MAPnDAT.DTR lists the objects, three names per row (the one used is picked by
// g_race_options & 3; the game leaves it at 0). Where they stand comes from a spline file of
// the track: MAPnOST.SPL in races, MAPnCNG.SPL in Cone Carnage, MAPnSLA.SPL in Slalom; each
// knot is one obstacle at (x, z) and its +0xc field is the OSTACLES row.
// An obstacle sits on the ground, turned to the track heading and tilted with the ground.
// The player knocks them over in every race (car_check_obstacle_hits, from car_collide_vs_cpu and
// car_collide_obstacles): a hit throws
// it along the car's heading at 1.4 x the car's speed; it spins, bounces (0.8 of the speed back
// up, gravity 500 per frame per second), slides to a stop, and ends lying on its side - or
// disappears when it lands off the road.

#include "core/car.h"
#include "core/model3d.h"
#include "core/track.h"

#include <string>
#include <vector>

namespace scr {

struct Obstacle {
    int x = 0, y = 0, z = 0;
    float ax = 0, ay = 0, az = 0;   // +0x0c.. radians
    int model = 0;                  // index into Obstacles::models
    float vx = 0, vy_spin = 0, vz = 0;  // +0x24.. direction of the throw
    float speed = 0;                // +0x30
    float lift = 0;                 // +0x34 vertical speed (per frame)
    bool hit = false;               // +0x38 moving after a hit
    bool flying = false;            // +0x3c counted as hit
};

class Obstacles {
public:
    // data_dat: DATA/MAPn.DAT, gfx_dat: GFX/MAPnH.DAT; spline: "OST", "CNG" or "SLA".
    bool load(const std::string& data_dat, const std::string& gfx_dat, const std::string& track_name,
              const std::string& spline, const Track& track);
    // obstacles_update: the ones in the air or sliding
    void update(const Track& track, float frame_time);
    // car_check_obstacle_hits: returns how many were knocked over this time (sfx 5 each)
    int collide(Car& car);
    int hits = 0;                   // PlayerCar.obstacles_hit
    std::vector<Obstacle> list;
    std::vector<Model> models;
};

} // namespace scr

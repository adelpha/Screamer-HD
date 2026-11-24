#pragma once
// Moving scenery: the trucks, buses, jets, trams, helicopters and cable cars that drive or fly
// along their own splines, and the invisible sound sources (AKKNULLA: waterfall, sea, fairground)
// that stand still. STARTL.EXE: track_load_animations, animations_update,
// animation_advance_on_spline, spline_segment_init.
//
// ANIMATIONS in DATA/MAPn.DAT MAPnDAT.DTR: { name (an object in GFX/MAPnH.DAT), speed (units
// per second), spln (spline in MAPnANM.SPL), sound_fx (index into the track's SOUND_FX) }.
// The splines have the racing-line layout (quadratic B-splines, two halves per segment); +0xc of
// a knot is its height, interpolated linearly along the segment.
// Each frame an animation within 52000 units of the listener (the player's car) moves on by
// speed * frame time; its sound plays within 40000 units (sfx_update_positional, RaceAudio).

#include "core/opponent.h"

#include <string>
#include <vector>

namespace scr {

struct TrackAnimation {
    std::string name;   // object, e.g. "awtruck" -> AWTRUCK.3DX
    float speed = 0;
    int spline = 0;
    int sound_fx = 0;   // index in the track's SOUND_FX (after the base table)
    // Position on the spline (runtime +0x2c..)
    int segment = 0, step = 0;
    float t = 0;
    float p[6]{}, py = 0;           // control points (x, z) and the height
    float eval_x = 0, eval_z = 0, dir_x = 0, dir_z = 1;
    int x = 0, y = 0, z = 0;        // where the object is
    float yaw = 0, prev_yaw = 0;
};

class TrackAnimations {
public:
    // data_dat: DATA/MAPn.DAT. Returns false when the track has none.
    bool load(const std::string& data_dat, const std::string& track_name);
    // track_load_animations' first animations_update(1): every animation takes its first step.
    void start(float frame_time);
    // animations_update(0) with the listener at (lx, lz).
    void update(int lx, int lz, float frame_time);
    std::vector<TrackAnimation> list;
    std::vector<RacingLine> splines;

private:
    void segment_init(TrackAnimation& a) const;
    void advance(TrackAnimation& a, float d) const;
};

} // namespace scr
